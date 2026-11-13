#include "starlet/gx/transform.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace starlet::gx;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void ok(const TransformStatus& r) { require(static_cast<bool>(r),r.message.c_str()); }
template<class T> void ok(const T& r) { require(static_cast<bool>(r), r.status.message.c_str()); }
void close(float actual, float expected, const char* message) {
    if (std::abs(actual-expected) > 2e-6f*std::max(1.0f,std::abs(expected))) {
        std::cerr << actual << " != " << expected << '\n'; throw std::runtime_error(message);
    }
}
void append32(std::vector<std::uint8_t>& bytes, std::uint32_t w) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(w >> shift));
}
std::uint32_t bits(float f) { std::uint32_t w; std::memcpy(&w, &f, 4); return w; }
struct Memory final : MemoryReader {
    bool copy(std::uint32_t, std::uint8_t*, std::size_t) override { return false; }
};
struct Sink final : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return false; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
using Matrix = std::array<TransformVector,3>;
const Matrix identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
struct Fixture {
    Memory memory; Sink sink; Decoder decoder{memory,sink};
    explicit Fixture(bool normals = true, unsigned colors = 3, unsigned texcoords = 255) {
        cp(0x50, 0x1ffu | (1u<<9) | (normals ? 1u<<11 : 0) |
            ((colors&1) ? 1u<<13 : 0) | ((colors&2) ? 1u<<15 : 0));
        std::uint32_t high=0; for (unsigned i=0;i<8;++i) if (texcoords&(1u<<i)) high |= 1u<<(2*i);
        cp(0x60,high);
        cp(0x70,1u | (4u<<1) | (1u<<9) | (4u<<10) | (1u<<13) | (5u<<14) |
            (1u<<17) | (5u<<18) | (1u<<21) | (4u<<22));
        constexpr std::uint32_t st=1u|(4u<<1);
        cp(0x80,st|(st<<9)|(st<<18)|(st<<27)); cp(0x90,(st<<5)|(st<<14)|(st<<23));
        matrix(0,0,identity); matrix(0,60,identity); matrix(0x500,61,identity);
        normal_matrix(0,identity);
        xf(0x1026,1); projection({1,0,1,0,1,0});
    }
    void command(const std::vector<std::uint8_t>& b) {
        const auto s=decoder.feed({b.data(),b.size()}); require(bool(s),s.message.c_str());
    }
    void cp(std::uint8_t a,std::uint32_t v) { std::vector<std::uint8_t>b{8,a}; append32(b,v); command(b); }
    void xf(std::uint16_t a,std::uint32_t v) {
        std::vector<std::uint8_t>b{0x10,0,0,static_cast<std::uint8_t>(a>>8),static_cast<std::uint8_t>(a)};
        append32(b,v); command(b);
    }
    void f(std::uint16_t a,float v) { xf(a,bits(v)); }
    void matrix(unsigned base,unsigned row,const Matrix& m) {
        for(unsigned r=0;r<3;++r) for(unsigned c=0;c<4;++c) f(static_cast<std::uint16_t>(base+(row+r)*4+c),m[r][c]);
    }
    void normal_matrix(unsigned row,const Matrix& m) {
        for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) f(static_cast<std::uint16_t>(0x400+(row+r)*3+c),m[r][c]);
    }
    void projection(std::array<float,6> values) { for(unsigned i=0;i<6;++i) f(static_cast<std::uint16_t>(0x1020+i),values[i]); }
    void texgen(unsigned i,unsigned row,bool abc,bool projected=false,unsigned type=0) {
        xf(0x103f,i+1); xf(static_cast<std::uint16_t>(0x1040+i),(row<<7)|(abc?4:0)|(projected?2:0)|(type<<4));
    }
    void channel(unsigned i,bool enabled,bool material_vertex,bool ambient_vertex,unsigned mask,unsigned diffuse,unsigned attenuation) {
        xf(static_cast<std::uint16_t>(0x100e + i),unsigned(material_vertex)|(enabled?2:0)|(ambient_vertex?64:0)|
            ((mask&15)<<2)|((mask&240)<<7)|(diffuse<<7)|(attenuation?512:0)|(attenuation==1||attenuation==0?1024:0));
    }
    void light(unsigned i,TransformVector position,TransformVector direction,TransformVector angle,TransformVector distance,std::uint32_t color) {
        unsigned base=0x600+i*16; xf(static_cast<std::uint16_t>(base+3),color);
        for(unsigned k=0;k<3;++k) {
            f(static_cast<std::uint16_t>(base+4+k),angle[k]); f(static_cast<std::uint16_t>(base+7+k),distance[k]);
            f(static_cast<std::uint16_t>(base+10+k),position[k]); f(static_cast<std::uint16_t>(base+13+k),direction[k]);
        }
    }
    VertexResult vertices(const std::vector<ExpandedVertex>& input) {
        Draw d; d.primitive=0xb8; d.vertex_count=static_cast<std::uint16_t>(input.size());
        std::string error; require(decoder.state().vertex_layout(0,d.layout,error),error.c_str());
        std::vector<std::uint8_t> bytes;
        for(const auto& v:input) {
            bytes.insert(bytes.end(),v.matrix_indices.begin(),v.matrix_indices.end());
            for(float c:v.position) append32(bytes,bits(c));
            if(d.layout.normal.mode!=AttributeMode::none) for(float c:v.normal) append32(bytes,bits(c));
            for(unsigned i=0;i<2;++i) if(d.layout.colors[i].mode!=AttributeMode::none)
                bytes.insert(bytes.end(),v.colors[i].begin(),v.colors[i].end());
            for(unsigned i=0;i<8;++i) if(d.layout.texcoords[i].mode!=AttributeMode::none)
                for(float c:v.texcoords[i]) append32(bytes,bits(c));
        }
        d.vertices={bytes.data(),bytes.size()}; return capture_vertices(decoder.state(),d,memory);
    }
    TransformStatus diagnose(const ExpandedVertex& v) {
        const auto vs=vertices({v}); require(bool(vs),vs.message.c_str());
        const auto c=capture_transform(decoder.state(),*vs.snapshot);
        if (!c) return c.status;
        return validate_transform_vertices(*c.snapshot,*vs.snapshot,TransformArithmetic::float32_candidate_v1);
    }
    TransformCapture capture(const ExpandedVertex& v) {
        const auto vs=vertices({v}); require(bool(vs),vs.message.c_str());
        return capture_transform(decoder.state(),*vs.snapshot);
    }
};
ExpandedVertex vertex() {
    ExpandedVertex v; v.position={2,3,-4}; v.normal={0,0,2, 1,0,0, 0,1,0};
    v.colors[0]={64,128,192,32}; v.colors[1]={20,40,80,160};
    for(unsigned i=0;i<8;++i) { v.texcoords[i]={float(i)+0.25f,float(i)+0.5f}; v.matrix_indices[i+1]=60; }
    return v;
}
TransformOutput evaluate(const TransformCapture& c,const ExpandedVertex& v) {
    ok(c); const auto r=evaluate_transform(c.snapshot->pipeline,c.snapshot->uniforms,v,TransformArithmetic::float32_candidate_v1);
    ok(r);
    const TransformEvaluator prepared(*c.snapshot,TransformArithmetic::float32_candidate_v1);
    ok(prepared.status());const auto fast=prepared.evaluate(v);ok(fast);
    require(fast.output.eye_position==r.output.eye_position && fast.output.normal==r.output.normal &&
            fast.output.gx_clip==r.output.gx_clip && fast.output.gles_clip==r.output.gles_clip &&
            fast.output.colors==r.output.colors && fast.output.texcoords==r.output.texcoords,
            "prepared evaluator preserves every transform output");
    return r.output;
}
void projection_and_aliases() {
    Fixture f; auto v=vertex();
    f.matrix(0,33,{{{2,0,0,10},{0,3,0,-5},{0,0,4,1}}});
    f.normal_matrix(1,{{{0,0,4,0},{0,2,0,0},{1,0,0,0}}}); v.matrix_indices[0]=33;
    f.xf(0x1009,1); f.channel(0,true,false,false,1,2,0);
    f.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0xffffffff);
    f.projection({2,3,4,5,6,7});
    auto c=f.capture(v); auto o=evaluate(c,v);
    close(o.eye_position[0],14,"position row 33"); close(o.eye_position[1],4,"position translation");
    close(o.eye_position[2],-15,"position scale"); close(o.normal[0],1,"normal row modulo 32");
    close(o.normal[2],0,"normal transform"); close(o.gx_clip[0],31,"orthographic projection");
    close(o.gx_clip[1],21,"orthographic Y"); close(o.gx_clip[2],-83,"orthographic Z");
    close(o.gles_clip[2],-165,"GX to GLES homogeneous depth");
    require(c.snapshot->uniforms.position[32]==TransformVector{},"unused position rows are omitted");
    require(c.snapshot->uniforms.normal[0]==TransformVector{},"unused normal rows are omitted");
    const auto key=generate_transform_glsl(c.snapshot->pipeline,TransformArithmetic::float32_candidate_v1).key;
    f.matrix(0,33,identity); const auto newer=f.capture(v);
    for (unsigned i=0;i<6;++i) f.f(static_cast<std::uint16_t>(0x101a+i),float(i*100+7));
    const auto viewport=f.capture(v); ok(viewport);
    for (unsigned i=0;i<6;++i) close(viewport.snapshot->uniforms.viewport[i/4][i%4],float(i*100+7),"viewport XF values preserved without scaling");
    close(evaluate(c,v).eye_position[0],14,"snapshot remains immutable after matrix mutation");
    require(key==generate_transform_glsl(newer.snapshot->pipeline,TransformArithmetic::float32_candidate_v1).key,"uniform values excluded from pipeline key");
    f.xf(0x1026,0); f.projection({2,0.25f,3,-0.5f,1.0f/9,10.0f/9}); v.matrix_indices[0]=0;
    v.position={0,0,-1}; o=evaluate(f.capture(v),v);
    close(o.gx_clip[2],1,"custom projection coefficients preserved");
    f.projection({2,0.25f,3,-0.5f,-1.0f/9,-10.0f/9});
    o=evaluate(f.capture(v),v); close(o.gx_clip[2],-1,"perspective near=-w"); close(o.gles_clip[2],-1,"GLES near=-w");
    v.position[2]=-10; o=evaluate(f.capture(v),v); close(o.gx_clip[2],0,"perspective far=0"); close(o.gles_clip[2],10,"GLES far=w");
    close(o.gx_clip[0],-2.5f,"off-center perspective X"); close(o.gx_clip[1],5,"off-center perspective Y");
    const auto vs=f.vertices({v}); require(bool(vs),vs.message.c_str()); f.f(0,2);
    require(capture_transform(f.decoder.state(),*vs.snapshot).status.error==TransformError::state_mismatch,"capture revisions must match");
}
void texture_coordinates() {
    Fixture f; auto v=vertex();
    // Position and texture rows really alias, including the nominal identity row.
    f.matrix(0,60,{{{2,0,7,11},{0,3,13,17},{0,0,5,19}}});
    f.texgen(0,5,false,false); auto c=f.capture(v); auto o=evaluate(c,v);
    close(o.texcoords[0][0],18.5f,"ST input must be (s,t,1,1)");
    close(o.texcoords[0][1],31.5f,"ST translations preserved"); close(o.texcoords[0][2],1,"2x4 q=1");
    f.texgen(0,5,false,true); o=evaluate(f.capture(v),v); close(o.texcoords[0][2],24,"3x4 q preserved, no per-vertex divide");
    f.texgen(1,0,true,true); v.matrix_indices[2]=0; o=evaluate(f.capture(v),v);
    close(o.texcoords[1][0],2,"position texgen uses object space"); close(o.texcoords[1][2],-4,"position texgen raw z");
    f.matrix(0,0,{{{1,0,0,10},{0,1,0,20},{0,0,1,30}}});
    o=evaluate(f.capture(v),v); close(o.texcoords[1][0],12,"position texgen aliases model matrix");
    f.texgen(2,1,true,true); f.texgen(3,3,true,true); f.texgen(4,4,true,true);
    v.matrix_indices[3]=v.matrix_indices[4]=v.matrix_indices[5]=60; f.matrix(0,60,identity);
    o=evaluate(f.capture(v),v); close(o.texcoords[2][2],2,"normal texgen uses unnormalized input");
    close(o.texcoords[3][0],1,"NBT binormal source"); close(o.texcoords[4][1],1,"NBT tangent source");
    f.xf(0x1012,1);
    for(unsigned i=0;i<5;++i) f.xf(static_cast<std::uint16_t>(0x1050+i),61);
    f.xf(0x1052,61|256); f.matrix(0x500,61,{{{2,0,0,3},{0,4,0,5},{0,0,6,7}}});
    o=evaluate(f.capture(v),v); close(o.texcoords[2][2],13,"normalize before loaded post identity row");
    close(o.texcoords[2][0],3,"post translation");
    // Color texgen consumes computed channel RGB even if no channel is sent to TEV.
    Fixture color(false,0,0); auto cv=vertex(); color.xf(0x100c,0x804020ff); color.texgen(0,2,false,false,2);
    auto co=evaluate(color.capture(cv),cv); close(co.texcoords[0][0],128.0f/255,"SRTG red"); close(co.texcoords[0][1],64.0f/255,"SRTG green");
}
void emboss() {
    Fixture f; auto v=vertex();
    f.texgen(0,5,false,false);
    // Texgen 1 embosses texgen 0 towards light 2; the vertex's binormal is +x, its tangent +y.
    f.xf(0x103f,2); f.xf(0x1041,(1u<<4)|(0u<<12)|(2u<<15));
    f.light(2,{5,7,-4,0},{0,0,0,0},{1,0,0,0},{1,0,0,0},0xffffffffu);
    auto c=f.capture(v); auto o=evaluate(c,v);
    close(o.texcoords[1][0],0.25f+0.6f,"emboss s offset is the light direction on the binormal");
    close(o.texcoords[1][1],0.5f+0.8f,"emboss t offset is the light direction on the tangent");
    close(o.texcoords[1][2],1,"emboss keeps the source q");
    f.normal_matrix(0,{{{2,0,0,0},{0,1,0,0},{0,0,1,0}}});
    o=evaluate(f.capture(v),v);
    close(o.texcoords[1][0],0.25f+1.2f,"emboss axes go through the normal matrix unnormalized");
    f.normal_matrix(0,identity);
    const auto key=generate_transform_glsl(c.snapshot->pipeline,TransformArithmetic::float32_candidate_v1);
    ok(key.status);
    require(key.source.find("gx_tex1 = gx_tex0 + vec3(offset,0.0)")!=std::string::npos,"emboss GLSL offsets its source");
    require(generate_transform_glsl(c.snapshot->pipeline,TransformArithmetic::float32_candidate_v1,false,false,0,false,true).status.error==
            TransformError::unsupported_state,"instanced transforms reject emboss texgens");
    auto other=c.snapshot->pipeline; other.texgens[1].emboss_light=3;
    require(generate_transform_glsl(other,TransformArithmetic::float32_candidate_v1,false).key!=key.key,"the emboss light is program identity");
    f.xf(0x1041,(1u<<4)|(1u<<12)|(2u<<15));
    require(f.capture(v).status.error==TransformError::unsupported_state,"emboss needs an earlier source");
    Fixture plain(false); auto pv=vertex(); plain.texgen(0,5,false,false);
    plain.xf(0x103f,2); plain.xf(0x1041,(1u<<4)|(2u<<15));
    require(plain.capture(pv).status.error==TransformError::missing_input,"emboss without NBT is rejected");
}
// The instanced variant reads the position and normal matrix rows per instance and
// everything else from the shared block.
void instanced_variant() {
    Fixture lit; auto v=vertex(); v.position={0,0,0};
    lit.xf(0x1009,1); lit.xf(0x100c,0xffffffff); lit.xf(0x100a,0);
    lit.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x64281414);
    lit.channel(0,true,false,false,1,2,0);
    const auto c=lit.capture(v); ok(c.status);
    const auto& p=c.snapshot->pipeline;
    const auto plain=generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1); ok(plain.status);
    const auto instanced=generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1,true,false,0,false,true); ok(instanced.status);
    require(instanced.key!=plain.key,"instancing is program identity");
    require(instanced.source.find("dot(u_instance[ib],position)")!=std::string::npos &&
            instanced.source.find("dot(u_instance[ib+3].xyz,normalIn)")!=std::string::npos &&
            instanced.source.find("u_matrix[pn]")==std::string::npos && instanced.source.find("u_matrix[nr]")==std::string::npos,
            "instanced GLSL takes both matrices from the instance rows");
    require(plain.source.find("dot(u_matrix[pn],position)")!=std::string::npos && plain.source.find("int nr = int(a_matrix.x >> 24u);")!=std::string::npos &&
            plain.source.find("u_matrix[nr+2]")!=std::string::npos && plain.source.find("GXPost")==std::string::npos,
            "the plain variant reads both matrices from GXMatrices, the normal matrix at the row the matrix indices name");
    require(plain.source.find("u_instance")==std::string::npos,"the plain variant has no instance block");
    require(generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1,true,true,0,false,true).status.error==
            TransformError::unsupported_state,"a transform is batched or instanced, not both");
}
// Draws without normals light with the vectors the XF latched from an earlier vertex.
void latched_normals() {
    Fixture lit; auto v=vertex(); v.position={0,0,0};
    lit.xf(0x1009,1); lit.xf(0x100c,0xffffffff); lit.xf(0x100a,0);
    lit.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x64281414);
    lit.channel(0,true,false,false,1,2,0);
    const auto with_normal=evaluate(lit.capture(v),v);
    Fixture bare(false,3,255); auto bv=vertex(); bv.position={0,0,0};
    bare.xf(0x1009,1); bare.xf(0x100c,0xffffffff); bare.xf(0x100a,0);
    bare.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x64281414);
    bare.channel(0,true,false,false,1,2,0);
    require(bare.capture(bv).status.error==TransformError::missing_input,"lighting without a normal or latch is rejected");
    const auto vs=bare.vertices({bv}); require(bool(vs),vs.message.c_str());
    TransformPipeline p;
    ok(capture_transform_pipeline(bare.decoder.state(),vs.snapshot->layout(),3,p,3));
    require(p.latched_normal && p.normal_components==3,"a latched normal stands in for the attribute");
    TransformUniforms u; direct_transform_uniforms(bare.decoder.state(),u);
    u.latched_normal[0]={0,0,2,0};
    const auto r=evaluate_transform(p,u,bv,TransformArithmetic::float32_candidate_v1); ok(r.status);
    require(r.output.colors[0]==with_normal.colors[0],"latched normal lights like the same normal attribute");
    TransformPipeline unlit; bare.channel(0,false,false,false,0,0,0);
    ok(capture_transform_pipeline(bare.decoder.state(),vs.snapshot->layout(),3,unlit,3));
    require(!unlit.latched_normal && !unlit.normal_components,"pipelines that need no normal ignore the latch");
    const auto shader=generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1);
    ok(shader.status);
    require(shader.source.find("vec3 normalIn = u_latched_normal[0].xyz;")!=std::string::npos,"latched GLSL reads the uniform");
    TransformPipeline attribute=p; attribute.latched_normal=false;
    require(generate_transform_glsl(attribute,TransformArithmetic::float32_candidate_v1,false).key!=shader.key,"the latch is program identity");
    // The latch comes from the last enabled vertex of a draw with normals.
    std::vector<std::uint8_t> bytes;
    ExpandedVertex a=vertex(), b=vertex(); b.normal={0,1,0, 1,0,0, 0,0,1};
    for(const auto* x:{&a,&b}) {
        bytes.insert(bytes.end(),x->matrix_indices.begin(),x->matrix_indices.end());
        for(float c:x->position) append32(bytes,bits(c));
        for(float c:x->normal) append32(bytes,bits(c));
        for(unsigned i=0;i<2;++i) bytes.insert(bytes.end(),x->colors[i].begin(),x->colors[i].end());
        for(unsigned i=0;i<8;++i) for(float c:x->texcoords[i]) append32(bytes,bits(c));
    }
    Draw d; d.primitive=0xb8; d.vertex_count=2;
    std::string error; require(lit.decoder.state().vertex_layout(0,d.layout,error),error.c_str());
    d.vertices={bytes.data(),bytes.size()};
    std::array<float,9> latch{};
    require(latest_normal(lit.decoder.state(),d,lit.memory,latch)==9 && latch==b.normal,"last vertex's NBT is latched");
    d.layout.normal.mode=AttributeMode::none;
    require(latest_normal(lit.decoder.state(),d,lit.memory,latch)==0,"draws without normals leave the latch");
}
void lighting() {
    Fixture f; auto v=vertex(); v.position={0,0,0};
    f.xf(0x1009,2); f.xf(0x100c,0x804020ff); f.xf(0x100d,0x10203040);
    auto o=evaluate(f.capture(v),v); close(o.colors[0][0],128.0f/255,"unlit register material"); close(o.colors[1][3],64.0f/255,"second alpha channel");
    f.channel(0,false,true,false,0,0,0); f.channel(2,false,false,false,0,0,0);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],64.0f/255,"vertex RGB material"); close(o.colors[0][3],1,"alpha material stays independent");
    f.xf(0x100c,0xffffffff); f.xf(0x100a,0x141e2832);
    f.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x64281414);
    f.light(7,{0,0,-2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x28140a64);
    f.channel(0,true,false,false,1,2,0); f.channel(2,true,false,false,128,1,0);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],120.0f/255,"clamped diffuse plus ambient");
    close(o.colors[0][3],0,"alpha uses separate upper light mask and signed diffuse");
    f.channel(0,true,false,false,128,2,0); o=evaluate(f.capture(v),v); close(o.colors[0][0],20.0f/255,"back-facing clamped diffuse");
    f.channel(0,true,false,false,128,1,0); o=evaluate(f.capture(v),v); close(o.colors[0][0],0,"back-facing signed diffuse subtracts");
    f.channel(0,true,false,true,0,0,0); o=evaluate(f.capture(v),v); close(o.colors[0][0],64.0f/255,"vertex ambient source");
    f.channel(0,true,false,false,1,0,1);
    f.light(0,{0,0,2,0},{0,0,1,0},{0,1,1,0},{1,1,1,0},0x64646464);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],48.0f/255,"spot quadratic over distance quadratic, truncate sum");
    f.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x64646464);
    const float finite_direction=evaluate(f.capture(v),v).colors[0][0];
    f.light(0,{0,0,2,0},{std::numeric_limits<float>::infinity(),0,std::numeric_limits<float>::quiet_NaN(),0},{1,0,0,0},{1,0,0,0},0x64646464);
    close(evaluate(f.capture(v),v).colors[0][0],finite_direction,"a distance-only light ignores its uninitialized direction");
    f.channel(0,true,false,false,129,0,1);
    f.xf(0x100a,0); f.xf(0x100c,0x80808080);
    for (unsigned light : {0u,7u}) f.light(light,{0,0,2,0},{0,0,1,0},{3,0,0,0},{5,0,0,0},0x01010101);
    o=evaluate(f.capture(v),v);
    close(o.colors[0][0],128.0f/(255*255),"truncate accumulated light sum once, before material multiplication");
    f.xf(0x100a,0x141e2832); f.xf(0x100c,0xffffffff);
    f.channel(0,true,false,false,1,0,2);
    f.light(0,{0,0,1e18f,0},{0,0,0.5f,0},{0,0,1,0},{0.5f,0,0.5f,0},0x64646464);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],60.0f/255,"specular half-angle polynomial");
    f.light(0,{0,0,-1e18f,0},{0,0,1,0},{0,0,1,0},{1,0,0,0},0x64646464);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],20.0f/255,"specular back-facing rejection");
    // Games leave unused lights enabled with every coefficient zero (Twilight Princess's fields).
    f.channel(0,true,false,false,1,0,1);
    f.light(0,{0,0,2,0},{0,0,1,0},{0,0,0,0},{0,0,0,0},0);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],20.0f/255,"a black light with zero attenuation adds nothing");
    f.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{0,0,0,0},0x64646464);
    o=evaluate(f.capture(v),v); close(o.colors[0][0],1,"a zero divisor saturates a lit light");
}
void prepared_lighting_channels() {
    Fixture f;auto v=vertex();v.position={0,0,0};
    f.xf(0x1009,2);f.xf(0x100a,0);f.xf(0x100b,0);
    f.xf(0x100c,0xffffffff);f.xf(0x100d,0xffffffff);
    f.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x64281414);
    f.light(7,{0,0,-2,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x28140a64);
    f.channel(0,true,false,false,1,2,0);f.channel(1,true,false,false,128,1,0);
    f.channel(2,true,false,false,1,2,0);f.channel(3,true,false,false,1,1,0);
    const auto captured=f.capture(v);ok(captured);
    const TransformEvaluator prepared(*captured.snapshot,TransformArithmetic::float32_candidate_v1);
    const auto first=prepared.evaluate(v);ok(first);
    close(first.output.colors[0][0],100.0f/255,"shared RGB/alpha function retains RGB light color");
    close(first.output.colors[0][3],20.0f/255,"shared RGB/alpha function retains alpha light color");
    close(first.output.colors[1][0],0,"different light mask and signed diffuse remain separate");
    close(first.output.colors[1][3],20.0f/255,"signed diffuse computes independent positive light");
    v.position[2]=4;const auto second=prepared.evaluate(v);ok(second);
    require(second.output.colors[0]==TransformVector{},"light factors are recomputed for each vertex");
    close(second.output.colors[1][3],0,"function cache does not retain previous vertex factor");
    v.position[0]=std::numeric_limits<float>::infinity();
    require(prepared.evaluate(v).status.error==TransformError::invalid_input,"prepared draws still reject non-finite vertices");
    const TransformEvaluator unsupported(*captured.snapshot,static_cast<TransformArithmetic>(0));
    require(unsupported.evaluate(vertex()).status.error==TransformError::unsupported_arithmetic,"prepared evaluator rejects unsupported arithmetic");
    auto pipeline=captured.snapshot->pipeline;pipeline.texgen_count=9;
    const TransformSnapshot invalid{pipeline,captured.snapshot->uniforms,captured.snapshot->state_revision};
    const TransformEvaluator rejected(invalid,TransformArithmetic::float32_candidate_v1);
    require(rejected.evaluate(vertex()).status.error==TransformError::unsupported_state,"invalid pipeline cannot enter prepared evaluation");
}
void failures_and_keys() {
    Fixture f; auto v=vertex();
    f.texgen(0,2,false,false,3); require(f.capture(v).status.error==TransformError::unsupported_state,"SRTG channel ordering constraint");
    f.texgen(0,2,false,false,2); f.texgen(1,2,false,false,2);
    require(f.capture(v).status.error==TransformError::unsupported_state,"duplicate SRTG color0 rejected");
    f.texgen(0,5,false,false,1); require(f.capture(v).status.error==TransformError::unsupported_state,"emboss without an earlier source fails explicitly");
    // Bits above the 18-bit texgen and 9-bit post registers do not exist; bits inside them that do nothing are rejected.
    f.texgen(0,5,false); f.xf(0x1040,0xffff4000u|(5u<<7)); f.xf(0x1050,0xfffffe00u|61u); ok(f.capture(v));
    f.xf(0x1040,(5u<<7)|1u); require(f.capture(v).status.error==TransformError::unsupported_state,"reserved texgen bit 0 rejected");
    f.xf(0x1040,5u<<7); f.xf(0x1050,61u|64u); require(f.capture(v).status.error==TransformError::unsupported_state,"reserved post bit 6 rejected");
    f.xf(0x1050,61u);
    f.texgen(0,5,false); v.matrix_indices[1]=63; require(f.capture(v).status.error==TransformError::invalid_matrix,"2-row matrix cannot start at 63");
    v.matrix_indices[1]=62; f.f(62*4,1); f.f(63*4+1,1); ok(f.capture(v));
    f.texgen(0,5,false,true); require(f.capture(v).status.error==TransformError::invalid_matrix,"3-row matrix cannot start at 62");
    v.matrix_indices[1]=60; v.matrix_indices[0]=30; f.matrix(0,30,identity);
    ok(f.capture(v));
    f.xf(0x1009,1); f.channel(0,true,false,false,1,2,0);
    require(f.capture(v).status.error==TransformError::invalid_matrix,"consumed normal row crossing rejected");
    f.xf(0x1009,0);
    v.matrix_indices[0]=0; f.xf(0x1005,1); ok(f.capture(v)); f.xf(0x1005,2); require(f.capture(v).status.error==TransformError::unsupported_state,"reserved clip mode is unsupported"); f.xf(0x1005,0);
    v.position[0]=std::numeric_limits<float>::infinity(); require(f.capture(v).status.error==TransformError::invalid_input,"non-finite position rejected"); v=vertex();
    v.normal[2]=0;
    const auto unlit=f.capture(v); ok(unlit);
    require(evaluate(unlit,v).normal==std::array<float,3>{},"unused zero normal is not transformed");
    const auto unlit_shader=generate_transform_glsl(unlit.snapshot->pipeline,TransformArithmetic::float32_candidate_v1); ok(unlit_shader);
    require(unlit_shader.source.find("int nr = pn % 32")==std::string::npos,"unused normal matrix work excluded from shader");
    f.xf(0x1009,1); f.channel(0,true,false,false,1,2,0);
    ok(f.capture(v));
    require(f.diagnose(v).error==TransformError::unsupported_arithmetic,"diagnostic rejects consumed zero lighting normal");
    f.xf(0x1009,0);
    f.texgen(0,1,true,true); const auto raw=f.capture(v); ok(raw);
    require(evaluate(raw,v).texcoords[0]==std::array<float,3>{},"raw zero normal remains valid for unnormalized texgen");
    f.xf(0x1012,1); f.xf(0x1050,61|256);
    ok(f.capture(v));
    require(f.diagnose(v).error==TransformError::unsupported_arithmetic,"diagnostic rejects zero post-normalized texgen");
    f.xf(0x1012,0); f.texgen(0,5,false,true);
    v=vertex(); f.xf(0x1009,1); f.channel(0,true,false,false,1,0,1);
    f.light(0,{0,0,2,0},{0,0,1,0},{1,0,0,0},{0,0,0,0},0xffffffff);
    ok(f.capture(v));
    ok(f.diagnose(v)); // A zero divisor saturates the light.
    f.xf(0x1009,0);
    Fixture missing(false,0,0); auto mv=vertex(); missing.texgen(0,1,true); require(missing.capture(mv).status.error==TransformError::missing_input,"absent normals are not zeros");
    // An absent ST reads (0, 0, 1, 1) whatever the vertex holds: s and t are the
    // matrix's third and fourth columns summed.
    missing.matrix(0,60,{{TransformVector{1,0,2,3},TransformVector{0,1,4,5},TransformVector{0,0,1,0}}});
    missing.texgen(0,5,false); const auto absent_st=missing.capture(mv); ok(absent_st);
    require(evaluate(absent_st,mv).texcoords[0]==std::array<float,3>{{5,9,1}},"absent ST reads (0, 0, 1, 1)");
    const auto absent_shader=generate_transform_glsl(absent_st.snapshot->pipeline,TransformArithmetic::float32_candidate_v1); ok(absent_shader);
    require(absent_shader.source.find("vec4 source = vec4(0.0,0.0,1.0,1.0)")!=std::string::npos &&
            absent_shader.source.find("vec4 source = vec4(a_tex0")==std::string::npos,"the shader supplies (0, 0, 1, 1) for an absent ST");
    {
        Fixture present(false,0,1); auto pv=vertex(); present.texgen(0,5,false); const auto with_st=present.capture(pv); ok(with_st);
        require(generate_transform_glsl(with_st.snapshot->pipeline,TransformArithmetic::float32_candidate_v1).key!=absent_shader.key,
                "ST presence is part of the program key");
    }
    missing.matrix(0,60,identity);
    missing.xf(0x103f,0); missing.xf(0x1009,1); missing.channel(0,false,true,false,0,0,0);
    require(bool(missing.capture(mv)),"an absent color reads white instead of failing");
    f.xf(0x103f,0); v=vertex(); auto c=f.capture(v); ok(c); auto p=c.snapshot->pipeline;
    const auto shader=generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1); ok(shader);
    require(shader.source.find("#version 310 es")!=std::string::npos,"standalone GLES3.1 source");
    p.channels[0].light_mask=255; p.texgens[7].post_row=52;
    require(generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1).key==shader.key,"inactive state excluded from key");
    p.orthographic=!p.orthographic; require(generate_transform_glsl(p,TransformArithmetic::float32_candidate_v1).key!=shader.key,"projection form changes key");
    require(!generate_transform_glsl(p,static_cast<TransformArithmetic>(0)),"unknown arithmetic model rejected");
    const auto empty_vertices=f.vertices({}); require(bool(empty_vertices),empty_vertices.message.c_str());
    const auto empty_transform=capture_transform(f.decoder.state(),*empty_vertices.snapshot); ok(empty_transform);
    require(validate_transform_vertices(*empty_transform.snapshot,*empty_vertices.snapshot,static_cast<TransformArithmetic>(0)).error==TransformError::unsupported_arithmetic,
            "diagnostic rejects unknown arithmetic even for empty geometry");
}
void write_bytes(const std::string& path,const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path,std::ios::binary); stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    require(bool(stream),"write fixture bytes");
}
void little32(std::vector<std::uint8_t>& b,std::uint32_t w) {
    for(unsigned shift=0;shift<32;shift+=8) b.push_back(static_cast<std::uint8_t>(w>>shift));
}
void write_fixture(const std::string& directory,const std::string& name,Fixture& f,std::vector<ExpandedVertex> input) {
    const auto vertices=f.vertices(input); require(bool(vertices),vertices.message.c_str());
    const auto capture=capture_transform(f.decoder.state(),*vertices.snapshot); ok(capture);
    const auto& snapshot=*capture.snapshot;
    const auto domain=validate_transform_vertices(snapshot,*vertices.snapshot,TransformArithmetic::float32_candidate_v1);
    require(bool(domain),domain.message.c_str());
    const auto shader=generate_transform_glsl(snapshot.pipeline,TransformArithmetic::float32_candidate_v1); ok(shader);
    std::ofstream source(directory+'/'+name+".vert"); source << shader.source; require(bool(source),"write vertex shader");
    std::vector<std::uint8_t> attributes, expected, uniforms;
    auto add_float=[](std::vector<std::uint8_t>& b,float v) { little32(b,bits(v)); };
    for(const auto& v:vertices.snapshot->vertices()) {
        for(float c:v.position) add_float(attributes,c);
        for(float c:v.normal) add_float(attributes,c);
        for(const auto& color:v.colors) for(auto c:color) add_float(attributes,float(c)/255);
        for(const auto& st:v.texcoords) for(float c:st) add_float(attributes,c);
        for(unsigned i=0;i<3;++i) little32(attributes,std::uint32_t(v.matrix_indices[i*3])|
            (std::uint32_t(v.matrix_indices[i*3+1])<<8)|(std::uint32_t(v.matrix_indices[i*3+2])<<16)|
            (i?0u:std::uint32_t(transform_normal_row(v.matrix_indices[0]))<<24));
        const auto r=evaluate_transform(snapshot.pipeline,snapshot.uniforms,v,TransformArithmetic::float32_candidate_v1); ok(r);
        for(float c:r.output.eye_position) add_float(expected,c);
        for(float c:r.output.normal) add_float(expected,c);
        for(float c:r.output.gx_clip) add_float(expected,c);
        for(float c:r.output.gles_clip) add_float(expected,c);
        for(const auto& color:r.output.colors) for(float c:color) add_float(expected,c);
        for(const auto& st:r.output.texcoords) for(float c:st) add_float(expected,c);
    }
    const auto* raw=reinterpret_cast<const unsigned char*>(&snapshot.uniforms);
    for(std::size_t i=0;i<sizeof(TransformUniforms);i+=4) {
        std::uint32_t word; std::memcpy(&word,raw+i,4); little32(uniforms,word);
    }
    write_bytes(directory+'/'+name+".vertices.bin",attributes);
    write_bytes(directory+'/'+name+".uniforms.bin",uniforms);
    write_bytes(directory+'/'+name+".expected.bin",expected);
    write_bytes(directory+'/'+name+".key.bin",shader.key);
    std::ofstream manifest(directory+'/'+name+".json");
    manifest << "{\n  \"name\": \"" << name << "\",\n  \"vertex_count\": " << input.size()
      << ",\n  \"vertex_stride\": 156,\n  \"feedback_stride\": 184,\n"
      << "  \"byte_order\": \"little\",\n  \"model\": \"float32_candidate_v1\",\n"
      << "  \"uniform_blocks\": [{\"name\": \"GXMatrices\", \"binding\": 2, \"offset\": 0, \"bytes\": 1536},"
         " {\"name\": \"GXPost\", \"binding\": 3, \"offset\": 1536, \"bytes\": 1024},"
         " {\"name\": \"GXTransform\", \"binding\": 0, \"offset\": 2560, \"bytes\": 784}],\n"
      << "  \"uniform_offsets\": {\"position\": 0, \"normal\": 1024, \"post\": 1536, \"projection\": 2560, \"ambient\": 2592, \"material\": 2624, \"lights\": 2656, \"latched_normal\": 3296, \"viewport\": 3344},\n"
      << "  \"light_stride\": 80,\n  \"light_offsets\": {\"color\": 0, \"angle\": 16, \"distance\": 32, \"position\": 48, \"direction\": 64},\n"
      << "  \"attributes\": [";
    for(unsigned i=0;i<15;++i) {
        if(i) manifest << ',';
        unsigned offset=i<4?i*12:(i<6?48+(i-4)*16:(i<14?80+(i-6)*8:144));
        unsigned components=i<4?3:(i<6?4:(i<14?2:3));
        manifest << "{\"location\":" << i << ",\"offset\":" << offset << ",\"components\":" << components
                 << ",\"type\":\"" << (i==14?"uint32":"float32") << "\",\"integer\":" << (i==14?"true":"false") << '}';
    }
    manifest << "],\n  \"feedback\": [\"gx_eye\",\"gx_normal\",\"gx_clip\",\"gx_gles_clip\",\"gx_color0\",\"gx_color1\"";
    for(unsigned i=0;i<8;++i) manifest << ",\"gx_tex" << i << "\"";
    manifest << "],\n  \"feedback_mode\": \"interleaved\",\n  \"absolute_tolerance\": 0.00002,\n  \"relative_tolerance\": 0.00004,\n"
             << "  \"color_absolute_tolerance\": 0.003922,\n  \"color_float_range\": [14,22],\n"
             << "  \"computed_color_mask\": " << unsigned(snapshot.pipeline.computed_colors)
             << ",\n  \"texgen_count\": " << unsigned(snapshot.pipeline.texgen_count)
             << ",\n  \"evidence_boundary\": \"CPU/GLES consistency of a declared numerical candidate; not original GX precision proof\"\n}\n";
    require(bool(manifest),"write fixture manifest");
}
void fixtures(const std::string& directory) {
    std::vector<ExpandedVertex> input;
    for(unsigned i=0;i<24;++i) {
        auto v=vertex(); v.position={float(int(i%5)-2)/4,float(int(i%7)-3)/3,-1.0f-float(i)/8};
        v.normal={0.2f,0.3f,0.8f, 1,0.1f,0, 0.1f,1,0.2f};
        for(unsigned t=0;t<8;++t) v.texcoords[t]={float(t)/8+float(i)/32,float(t)/16-float(i)/64};
        v.colors[0]={static_cast<std::uint8_t>(11+i*7),static_cast<std::uint8_t>(230-i*5),120,static_cast<std::uint8_t>(20+i*8)};
        v.colors[1]={200,80,static_cast<std::uint8_t>(15+i*9),160};
        input.push_back(v);
    }
    {
        Fixture f; f.projection({1.25f,-0.25f,0.75f,0.3f,0.1f,-0.8f});
        f.xf(0x1009,2); f.channel(0,false,true,false,0,0,0); f.channel(2,false,true,false,0,0,0);
        f.xf(0x100d,0x7f38c1e9); write_fixture(directory,"orthographic",f,input);
        f.xf(0x1026,0); f.projection({1.4f,0.15f,1.8f,-0.2f,-1.0f/19,-20.0f/19});
        write_fixture(directory,"perspective",f,input);
    }
    {
        Fixture f; f.xf(0x1012,1); f.matrix(0,33,{{{0.5f,0,0,0.1f},{0,0.75f,0,-0.2f},{0,0,1,0.3f}}});
        f.normal_matrix(1,{{{0,0,2,0},{0,3,0,0},{1,0,0,0}}});
        f.xf(0x1009,1); f.xf(0x100c,0xffffffff); f.channel(0,true,false,false,1,2,0);
        f.light(0,{0,0,5,0},{0,0,1,0},{1,0,0,0},{1,0,0,0},0x88aaccff);
        f.matrix(0,60,{{{0.3f,0.2f,0.7f,-0.1f},{0.4f,0.6f,0.2f,0.1f},{0.2f,0.1f,0.4f,0.9f}}});
        f.matrix(0x500,61,{{{0.5f,0,0,0.5f},{0,0.5f,0,0.5f},{0,0,0.75f,0.25f}}});
        for(unsigned i=0;i<8;++i) {
            const unsigned sources[8]={5,5,0,1,3,4,8,12}; const bool abc=sources[i]<5;
            f.texgen(i,sources[i],abc,i!=0); f.xf(static_cast<std::uint16_t>(0x1050+i),61|(i==3?256:0));
        }
        auto v=input; for(unsigned i=0;i<v.size();++i) {
            v[i].matrix_indices[0]=i%2?33:0; v[i].matrix_indices[3]=v[i].matrix_indices[0];
        }
        write_fixture(directory,"texgen_alias_post",f,v);
    }
    for(unsigned mode=0;mode<3;++mode) {
        Fixture f; f.xf(0x1009,2); f.xf(0x100a,0x18304c30); f.xf(0x100b,0x40506070);
        f.xf(0x100c,0xe1c381ab); f.xf(0x100d,0x7f99c1e9);
        f.light(0,{2,1,3,0},{0.2f,0.4f,0.7f,0},{0.2f,0.4f,0.8f,0},{0.8f,0.3f,0.1f,0},0x62421e8a);
        f.light(7,{-1,2,-3,0},{-0.3f,0.2f,0.8f,0},{0.3f,0.7f,0.4f,0},{0.6f,0.2f,0.3f,0},0x39713b65);
        f.channel(0,true,false,false,129,mode==2?0:2,mode);
        f.channel(2,true,false,true,128,mode==2?0:1,mode);
        f.channel(1,true,true,false,1,0,mode); f.channel(3,false,true,false,0,0,0);
        write_fixture(directory,mode==0?"diffuse":(mode==1?"spot":"specular"),f,input);
    }
    {
        Fixture f; f.xf(0x1009,0); f.xf(0x100a,0x33445566); f.xf(0x100c,0x99aabbcc); f.xf(0x100d,0xddeeff99);
        f.channel(0,true,false,true,0,0,0); f.channel(1,false,true,false,0,0,0);
        f.texgen(0,2,false,false,2); f.texgen(1,2,false,false,3);
        write_fixture(directory,"color_texgen",f,input);
    }
}
} // namespace
void clip_disabled_domain() {
    TransformPipeline pipeline; pipeline.clipping_disabled = true;
    TransformOutput output; output.gles_clip = {0,0,0,1};
    ok(validate_transform_clipping(pipeline,output));
    for (unsigned axis = 0; axis < 3; ++axis) for (float sign : {-1.0f,1.0f}) {
        output.gles_clip = {0,0,0,1}; output.gles_clip[axis] = sign;
        ok(validate_transform_clipping(pipeline,output));
        output.gles_clip[axis] = sign * 1.001f;
        ok(validate_transform_clipping(pipeline,output));
    }
    for (float w : {0.0f,-1.0f}) {
        output.gles_clip = {0,0,0,w};
        ok(validate_transform_clipping(pipeline,output));
    }
    output.gles_clip = {0,0,0,std::numeric_limits<float>::infinity()};
    require(!validate_transform_clipping(pipeline,output), "clip-disabled infinite W rejected");
    output.gles_clip = {std::numeric_limits<float>::quiet_NaN(),0,0,1};
    require(!validate_transform_clipping(pipeline,output), "clip-disabled nonfinite component rejected");
    pipeline.clipping_disabled = false;
    ok(validate_transform_clipping(pipeline,output));
}
void unused_raster_colors() {
    Fixture f(false, 0, 0);auto v=vertex();
    f.xf(0x1009,1);f.channel(0,false,true,false,0,0,0);f.channel(2,false,true,false,0,0,0);
    auto vertices=f.vertices({v});require(bool(vertices),"no-color vertex capture");
    // A live channel without a vertex color reads white.
    const auto live=capture_transform(f.decoder.state(),*vertices.snapshot);ok(live);
    const auto white=evaluate(live,vertices.snapshot->vertices()[0]);
    require(white.colors[0]==std::array<float,4>{{1,1,1,1}},"missing vertex color reads white");
    require(generate_transform_glsl(live.snapshot->pipeline,TransformArithmetic::float32_candidate_v1).source.find("material = vec4(255.0)")!=std::string::npos,
            "the shader supplies white for the absent color");
    auto second_only=live.snapshot->pipeline; second_only.vertex_colors=2;
    require(generate_transform_glsl(second_only,TransformArithmetic::float32_candidate_v1).source.find("material = round(a_color1*255.0)")!=std::string::npos,
            "channel 0 reads color 1 when it is the only vertex color");
    auto dead=capture_transform(f.decoder.state(),*vertices.snapshot,0);ok(dead);
    require(dead.snapshot->pipeline.computed_colors==0,"dead channel must not be evaluated");
    ok(generate_transform_glsl(dead.snapshot->pipeline,TransformArithmetic::float32_candidate_v1));
    f.texgen(0,2,false,false,2);vertices=f.vertices({v});
    const auto texgen=capture_transform(f.decoder.state(),*vertices.snapshot,0);
    require(bool(texgen) && texgen.snapshot->pipeline.computed_colors==1,"a color texgen keeps its channel, which reads white");
}
int main(int argc,char** argv) {
    try {
        clip_disabled_domain(); unused_raster_colors(); projection_and_aliases(); texture_coordinates(); emboss(); instanced_variant(); latched_normals(); lighting(); prepared_lighting_channels(); failures_and_keys();
        if(argc==3 && std::string(argv[1])=="--fixtures") fixtures(argv[2]);
        else require(argc==1,"usage: starlet_gx_transform_tests [--fixtures EXISTING_DIRECTORY]");
        std::cout << "GX transform tests passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
