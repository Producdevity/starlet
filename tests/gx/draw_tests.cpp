#include "draw_fixture.hpp"
#include "starlet/gx/direct.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <iostream>
#include <stdexcept>

using namespace starlet::gx;
using namespace starlet::gx::test;
namespace {
void clipped_texture_projection() {
    for (unsigned axis=0;axis<3;++axis) for (float sign:{-1.f,1.f}) {
        Fixture plane;
        plane.f(0x1020,1); plane.f(0x1021,0); plane.f(0x1022,1); plane.f(0x1023,0);
        plane.f(0x1024,1); plane.f(0x1025,0); plane.xf(0x1041,6);
        for (unsigned c=0;c<4;++c) plane.f(62u*4+c,0);
        plane.f(62u*4+axis,-sign); plane.f(62u*4+3,2);
        for (unsigned i=0;i<4;++i) {
            std::array<float,3> position{{i==1||i==2?.5f:-.5f,i>=2?.5f:-.5f,-.5f}};
            if (i<2) position[axis]=2*sign;
            std::vector<std::uint8_t> bytes;
            for(auto value:position) append_float(bytes,value);
            std::copy(bytes.begin(),bytes.end(),plane.vertices.begin()+i*32);
        }
        const auto clipped = plane.capture();
        check(bool(plan_gles_draw(packet(clipped))), "each frustum-plane pair clips offscreen zero-Q vertices safely");
    }
    Fixture f;
    f.xf(0x1026,0); f.f(0x1020,1); f.f(0x1021,0); f.f(0x1022,1); f.f(0x1023,0);
    f.f(0x1024,-1.0f/3); f.f(0x1025,-4.0f/3);
    f.xf(0x1041,6); f.f(62u*4+2,-1);
    for (unsigned i=0;i<4;++i) {
        std::vector<std::uint8_t> position;
        append_float(position,i==1||i==2?.5f:-.5f); append_float(position,i>=2?.5f:-.5f);
        append_float(position,i<2?0.f:-2.f);
        std::copy(position.begin(),position.end(),f.vertices.begin()+i*32);
    }
    const auto clipped = f.capture();
    check(bool(plan_gles_draw(packet(clipped))) && bool(plan_gles_draw(packet(clipped),FramebufferFetch::none,false)),
          "zero Q on camera-clipped vertices cannot reject a nonsingular visible polygon");
    f.f(62u*4+2,0); f.f(62u*4+3,-1);
    const auto negative = f.capture();
    check(bool(plan_gles_draw(packet(negative))), "consistent negative visible Q remains nonsingular");
    f.f(62u*4,1); f.f(62u*4+3,0);
    const auto crossing = f.capture();
    check(bool(plan_gles_draw(packet(crossing))), "ordinary texture projection permits opposite-sign visible Q");
    f.f(62u*4,0); f.f(62u*4+3,1e-21f);
    const auto tiny = f.capture();
    check(bool(plan_gles_draw(packet(tiny))), "finite projected coordinates are not rejected by an arbitrary Q threshold");
    f.f(62u*4+3,std::numeric_limits<float>::denorm_min());
    check(!plan_gles_draw(packet(f.capture())), "projective division overflow still rejects");
    f.f(62u*4+3,0);
    check(!plan_gles_draw(packet(f.capture())), "a fully zero-Q visible lookup is degenerate");
    for (unsigned i=0;i<4;++i) std::fill_n(f.vertices.begin()+i*32+8,4,0);
    const auto invisible = f.capture();
    check(bool(plan_gles_draw(packet(invisible))), "fully clipped geometry has no texture projection to divide");
}
void clip_disabled_draws() {
    Fixture f;
    const auto before = f.capture(); const auto original = plan_gles_draw(packet(before));
    f.xf(0x1005,1);
    const auto captured = f.capture(); const auto& draw = packet(captured);
    check(draw.transform->pipeline.clipping_disabled, "clip-disable register retained");
    const auto inside = plan_gles_draw(draw); const auto deferred = plan_gles_draw(draw,FramebufferFetch::none,false);
    check(pipeline(inside).key == pipeline(original).key && pipeline(deferred).key == pipeline(inside).key &&
          inside.pipeline.vertex == original.pipeline.vertex && inside.pipeline.fragment == original.pipeline.fragment,
          "unclipped clip-disabled draw uses identical shader and program identity");
    auto uniforms = draw.transform->uniforms; uniforms.position[0][3] = 1280;
    auto outside = draw;
    outside.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{draw.transform->pipeline,uniforms,draw.transform->state_revision});
    check(bool(plan_gles_draw(outside)) && bool(plan_gles_draw(outside,FramebufferFetch::none,false)),
          "full and warm plans admit clip-disabled XY outside the scissored viewport");
    check(bool(validate_transform_vertices(*outside.transform,*outside.vertices,TransformArithmetic::float32_candidate_v1)),
          "diagnostic validator admits finite clip-disabled XY");
    uniforms.position[0][3] = 0;
    uniforms.position[2][3] = 1280;
    outside.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{draw.transform->pipeline,uniforms,draw.transform->state_revision});
    check(!plan_gles_draw(outside) && !plan_gles_draw(outside,FramebufferFetch::none,false),
          "clip-disabled near/far crossing still rejects");
}
void offscreen_clip_disabled_draws() {
    Fixture f;
    auto base = packet(f.capture());
    auto transform = base.transform->pipeline;
    transform.clipping_disabled = true; transform.orthographic = false;
    auto uniforms = base.transform->uniforms;
    uniforms.projection[0] = {1,0,1,0}; uniforms.projection[1] = {0,0,0,0};
    uniforms.position[2][3] = 10; uniforms.position[0][3] = 1000;
    auto outside = base;
    outside.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    const auto full = plan_gles_draw(outside), warm = plan_gles_draw(outside,FramebufferFetch::none,false);
    check(full && warm && full.pipeline.discarded && warm.pipeline.discarded,
          "same-sign negative-W triangles wholly outside viewport have no fragments");
    uniforms.position[0] = {.001f,0,0,-.2f}; uniforms.position[1] = {0,.001f,0,-.15f};
    outside.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    {const auto a = plan_gles_draw(outside), b = plan_gles_draw(outside,FramebufferFetch::none,false);
     check(a && b && a.pipeline.discarded && b.pipeline.discarded, "negative-W geometry inside the viewport lies behind the eye and has no fragments");}
    uniforms.position[0] = {1,0,0,1000}; uniforms.position[1] = {0,1,0,0};
    uniforms.position[2] = {0,0,0,0};
    outside.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    {const auto a = plan_gles_draw(outside);
     check(a && a.pipeline.discarded, "W=0 at every vertex has no fragments");}
    f.vertices.clear();
    for (unsigned i=0;i<4;++i) {
        append_float(f.vertices, i==1||i==2 ? 300 : 100); append_float(f.vertices,i>=2 ? 200 : 100);
        append_float(f.vertices,i<2 ? -2 : 2);
        for(unsigned j=0;j<4;++j)f.vertices.push_back(255);
        for(unsigned j=0;j<4;++j)append_float(f.vertices,1);
    }
    auto crossing = packet(f.capture());uniforms.position[2] = {0,0,1,0};
    crossing.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,crossing.transform->state_revision});
    check(!plan_gles_draw(crossing), "triangles crossing W=0 cannot use divided-coordinate culling");
}
void clip_disabled_behind_eye() {
    Fixture f;
    auto base = packet(f.capture());
    auto transform = base.transform->pipeline;
    transform.clipping_disabled = true; transform.orthographic = false;
    auto uniforms = base.transform->uniforms;
    uniforms.position[2][3] = 10;
    auto behind = base;
    behind.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    const auto full = plan_gles_draw(behind), warm = plan_gles_draw(behind,FramebufferFetch::none,false);
    check(full && warm && full.pipeline.discarded && warm.pipeline.discarded,
          "clip-disabled draws with negative W at every vertex are discarded");
    // View z = 0.1y - 15.5 runs from -5.5 to 4.5 across the quad: W changes sign.
    uniforms.position[2] = {0,.1f,1,-15};
    auto crossing = base;
    crossing.transform = std::make_shared<TransformSnapshot>(TransformSnapshot{transform,uniforms,base.transform->state_revision});
    check(!plan_gles_draw(crossing) && !plan_gles_draw(crossing,FramebufferFetch::none,false),
          "clip-disabled draws with W of both signs still reject");
}
void deferred_shader_source() {
    for (unsigned variant = 0; variant < 6; ++variant) {
        Fixture f;
        if (variant == 1) { f.memory.bytes.resize(96); f.bp(0x83,0x1e1d5); f.bp(0x87,0x2000); }
        if (variant == 2) f.full_opaque_quad();
        if (variant >= 3) { f.bp(0x43,1); f.bp(0x41,variant == 3 ? 0x18 : 0xf4a9); }
        const auto captured = f.capture();
        const auto& draw = packet(captured);
        const auto fetch = variant == 5 ? FramebufferFetch::ext : FramebufferFetch::arm;
        const auto full = plan_gles_draw(draw, fetch);
        const auto deferred = plan_gles_draw(draw, fetch, false);
        const auto& a = pipeline(full); const auto& b = pipeline(deferred);
        check(!a.vertex.empty() && !a.fragment.empty() && b.vertex.empty() && b.fragment.empty(),
              "deferred plan does not build GLSL source");
        check(a.key == b.key && a.coordinate_scale == b.coordinate_scale && a.image_size == b.image_size &&
              a.indirect_scale == b.indirect_scale && a.sampler_lod == b.sampler_lod &&
              a.pixel_controls == b.pixel_controls && a.rgba6_copy == b.rgba6_copy &&
              a.full_replacement == b.full_replacement && a.alpha_write == b.alpha_write,
              "deferred and full plans retain identical identity, uniforms and replacement semantics");
        auto bad = draw; bad.texture_mask = 0;
        const auto bad_full = plan_gles_draw(bad, fetch);
        const auto bad_deferred = plan_gles_draw(bad, fetch, false);
        check(bool(bad_full) == bool(bad_deferred) && bad_full.status.error == bad_deferred.status.error &&
              bad_full.status.message == bad_deferred.status.message, "deferred validation rejects the same invalid packet");
    }
}
void mipmap_lod_controls() {
    TextureSampler sampler; sampler.max_lod_sixteenths=160;
    check(std::abs(evaluate_texture_lod({2,0},{0,2},sampler)-1)<1e-6f,"edge distance LOD");
    sampler.edge_lod=false;
    check(std::abs(evaluate_texture_lod({2,0},{0,2},sampler)-1.5f)<1e-6f,"diagonal distance LOD");
    sampler.lod_bias_thirtyseconds=-16;
    check(std::abs(evaluate_texture_lod({2,0},{0,2},sampler)-1)<1e-6f,"actual game compensating half-level bias");
    sampler.min_lod_sixteenths=24; sampler.max_lod_sixteenths=32;
    check(evaluate_texture_lod({0,0},{0,0},sampler)==1.5f &&
          evaluate_texture_lod({1024,0},{0,1024},sampler)==2,"min/max LOD clamps");
    Fixture f; f.memory.bytes.resize(96); f.bp(0x83,0x1e1d5); f.bp(0x87,0x2000);
    const auto captured=f.capture(); const auto& draw=packet(captured);
    check(draw.textures[3]->levels().size()==3,"complete requested mip chain captured");
    const auto plan=plan_gles_draw(draw);const auto& gpu=pipeline(plan);
    check(gpu.fragment.find("gxSampleMip(uGxTexture3")!=std::string::npos &&
          gpu.sampler_lod[3]==std::array<float,4>{{-.5f,0,2,0}},"real diagonal/bias/LOD controls reach shader");
    f.bp(0x83,0x2d5); const auto changed=f.capture();
    check(pipeline(plan_gles_draw(packet(changed))).key==gpu.key,"LOD bias and metric are uniforms");
    Fixture planar; planar.bp(0x87,0x3000); planar.bp(0x83,1u<<9);
    pipeline(plan_gles_draw(packet(planar.capture())));
}
void perspective_integration() {
    Fixture f;
    const auto ortho=f.capture(); const auto ortho_plan=plan_gles_draw(packet(ortho));
    f.xf(0x1026,0); f.f(0x1020,1); f.f(0x1021,0); f.f(0x1022,1); f.f(0x1023,0);
    f.f(0x1024,-1.0f/3); f.f(0x1025,-4.0f/3);
    const auto perspective=f.capture(); const auto& draw=packet(perspective);
    const auto gpu=plan_gles_draw(draw);
    check(bool(gpu) && gpu.pipeline.key!=pipeline(ortho_plan).key,"projection form selects the existing homogeneous transform program");
    auto vertex=draw.vertices->vertices()[0]; vertex.position={0,0,-2};
    const auto transformed=evaluate_transform(draw.transform->pipeline,draw.transform->uniforms,
                                               vertex,TransformArithmetic::float32_candidate_v1);
    check(bool(transformed) && transformed.output.gles_clip[3]==2 &&
          std::abs(transformed.output.gles_clip[2]/2-1.0f/3)<1e-6f,"perspective W and GX depth interval preserved");
}
void inactive_indirect_inputs() {
    Fixture f; const auto reference = plan_gles_draw(packet(f.capture()));
    f.bp(0x27,0xffffff);
    for(unsigned word=0;word<128;++word) {
        f.bp(0x10,word);
        const auto captured=f.capture();const auto& draw=packet(captured);
        check(draw.indirect->lookup_mask==0 && draw.indirect->stages[0].matrix==0 &&
            draw.indirect->stages[0].bias==0,"inactive lookup inputs require no source or matrix");
        const auto planned=plan_gles_draw(draw);
        check(pipeline(planned).key==pipeline(reference).key && planned.pipeline.fragment==reference.pipeline.fragment &&
            planned.pipeline.vertex==reference.pipeline.vertex,"inactive format, lookup and bias preserve direct shader identity");
    }
    for(unsigned effect:{128u,256u,1u<<19,1u<<20}) {
        f.bp(0x10,0x70|effect);
        check(!f.capture(),"matrix-off does not bypass active unsupported feedback, LOD or bump alpha");
    }
}

void indirect_lookup_and_offsets() {
    IndirectMatrix matrix;
    matrix.rows = {{{{512,0,0,-2}},{{0,512,0,-2}}}};
    check(evaluate_indirect_offset(matrix,{17,201,255,0},7) == std::array<std::int32_t,2>{{-2048,2032}},
          "ABG routing, signed bias and fractional texel offsets");
    check(evaluate_indirect_offset(matrix,{255,0,127,129},7) == std::array<std::int32_t,2>{{16,-16}},
          "one eighth texel survives S17.7 quantization");
    check(evaluate_indirect_offset(matrix,{0,0,0,0},0) == std::array<std::int32_t,2>{{0,0}},"unbiased zero");
    matrix.rows = {{{{0,0,1,0}},{{-1,0,0,1}}}};
    check(evaluate_indirect_offset(matrix,{200,7,99,1},0) == std::array<std::int32_t,2>{{0,-2}},
          "green supplies U; negative products floor before exponent scaling");
    matrix.rows = {{{{1023,1023,1023,46}},{{-1024,-1024,-1024,-17}}}};
    check(evaluate_indirect_offset(matrix,{255,255,255,255},0) == std::array<std::int32_t,2>{{0,-1}},
          "full exponent range without undefined shifts or signed overflow");
    Fixture f;
    f.bp(0,0x10012); f.bp(0x10,0x270); f.bp(0x27,1);
    f.bp(6,0xc00200); f.bp(7,0xd00000); f.bp(8,0);
    f.bp(0x81,0); f.bp(0x85,0); f.bp(0x89,0x100c07); f.bp(0x95,0x8000>>5);
    const auto captured=f.capture(); const auto& draw=packet(captured);
    check(draw.indirect->lookup_mask==1 && draw.texture_mask==10 && f.memory.reads==2,
          "indirect-only image is owned and counted exactly once");
    check(draw.indirect->matrices[0].rows[0][3]==-2 && draw.indirect->matrices[0].rows[1][3]==-2,"two-bit columns build the exponent");
    f.bp(8,0x800000);
    check(packet(f.capture()).indirect->matrices[0].rows[0][3]==-2,"the third column's upper scale bit has no effect");
    f.bp(8,0x400000);
    check(packet(f.capture()).indirect->matrices[0].rows[0][3]==14,"the third column's lower scale bit is the exponent's fifth bit");
    check(draw.indirect->matrices[0].rows==IndirectMatrix{{{{{512,0,0,-2}},{{0,512,0,-2}}}}}.rows,
          "actual game's packed signed11 matrix and exponent decode");
    const auto plan=plan_gles_draw(draw); const auto& gpu=pipeline(plan);
    check(gpu.fragment.find("indirectSample0=gxQuantize(texture(uGxTexture1")!=std::string::npos &&
          gpu.fragment.find("gxIndirectOffset(indirectSample0")!=std::string::npos,"second lookup modifies regular coordinates");
    f.bp(6,0xc001ff); const auto changed=f.capture(); const auto newer=plan_gles_draw(packet(changed));
    check(pipeline(newer).key==gpu.key && draw.indirect->matrices[0].rows[0][0]==512 &&
          changed.packet->indirect->matrices[0].rows[0][0]==511,"matrix animation is owned uniform state, not a new shader");
    f.bp(0x25,0x32); const auto scaled=f.capture();
    check(pipeline(plan_gles_draw(packet(scaled))).key!=gpu.key,"coordinate downshift changes sampling program");
    f.bp(0x10,0x200); const auto unbiased=f.capture();
    check(pipeline(plan_gles_draw(packet(unbiased))).key!=newer.pipeline.key,"bias is part of lookup identity");
    f.bp(0x27,3 | (1u<<3)); const auto shared=f.capture();
    check(shared && packet(shared).texture_mask==8 &&
          pipeline(plan_gles_draw(packet(shared))).fragment.find("indirectSample0=gxQuantize(texture(uGxTexture3")!=std::string::npos,
          "one map serves indirect and regular lookups and is captured once");
    f.bp(0x27,1 | (7u<<3)); check(!f.capture(),"missing indirect coordinate rejected before source lookup");
    // A stage reading a disabled lookup gets no offset (undefined on the console); its wraps still apply.
    f.bp(0x27,1); f.bp(0,0x12); const auto disabled=f.capture();
    check(disabled && packet(disabled).indirect->lookup_mask==0 && packet(disabled).indirect->stages[0].matrix==0,
          "a disabled indirect lookup adds no offset");
    f.bp(0x10,0x270|(6u<<13)); const auto wrap_only=f.capture();
    check(wrap_only && packet(wrap_only).indirect->stages[0].wrap_s==6 &&
          pipeline(plan_gles_draw(packet(wrap_only))).fragment.find("offset0=ivec2(0)")!=std::string::npos,
          "wrapping applies without an enabled lookup");
    f.bp(0x10,0x270);
    f.bp(0,0x50012); check(!f.capture(),"more than four indirect lookups rejected");
    f.bp(0,0x10012);
    for (const auto extra : {4u,128u,7u<<13,7u<<16,1u<<19,1u<<20}) {
        f.bp(0x10,0x270|extra); check(!f.capture(),"unimplemented indirect effects fail explicitly");
    }
    // Wrapping keeps the regular coordinate modulo 256..16 texels, or drops it (GX_ITW_0).
    f.bp(0x10,0x270|(6u<<13)|(1u<<16)); const auto wrapped=f.capture();
    check(wrapped && packet(wrapped).indirect->stages[0].wrap_s==6 && packet(wrapped).indirect->stages[0].wrap_t==1 &&
          pipeline(plan_gles_draw(packet(wrapped))).fragment.find("fixed0=(ivec2(0,(base0.y&32767))+offset0)")!=std::string::npos,
          "indirect wrap modes apply to the regular coordinate");
    f.bp(0x10,0xa70); check(!f.capture(),"dynamic matrices remain explicit");
    f.bp(0x10,0x670); const auto matrix2=f.capture();
    pipeline(plan_gles_draw(packet(matrix2)));
    f.xf(0x1041,(6u<<7)|2u);
    f.f(62u*4,1); f.f(62u*4+1,0); f.f(62u*4+2,0); f.f(62u*4+3,-.5f);
    check(!plan_gles_draw(packet(f.capture())), "indirect integer coordinates still reject a visible Q sign crossing");
    f.xf(0x1041,6u<<7);
    f.bp(0x10,0x270); f.memory.fail=true; check(!f.capture(),"indirect resource failures publish no draw");
}
void rgb8_full_replacement() {
    Fixture f; f.full_opaque_quad();
    check(pipeline(plan_gles_draw(packet(f.capture()))).full_replacement, "opaque RGB8 full overwrite");
    f.vertices[15]=7;
    check(!pipeline(plan_gles_draw(packet(f.capture()))).full_replacement, "varying colors cannot overwrite format");
    for(unsigned i=0;i<4;++i)f.vertices[i*32+15]=7;
    check(!pipeline(plan_gles_draw(packet(f.capture()))).full_replacement, "nonopaque output cannot restore RGB8 backing alpha");
    f.full_opaque_quad();f.bp(0x41,0xf4b9);
    check(!pipeline(plan_gles_draw(packet(f.capture()))).full_replacement, "blend depends on previous framebuffer");
    f.full_opaque_quad();f.bp(0x21, (980u<<12)|797u);
    check(!pipeline(plan_gles_draw(packet(f.capture()))).full_replacement, "partial scissor cannot replace EFB format");
}
void rgba6_fetch_boundary() {
    Fixture f; f.full_opaque_quad(); f.bp(0x43,1); f.bp(0x41,0xf7d9);
    auto captured = f.capture();const auto& draw = packet(captured);
    check(!plan_gles_draw(draw),"destination-alpha blend requires coherent fetch");
    const auto arm = plan_gles_draw(draw,FramebufferFetch::arm);
    const auto ext = plan_gles_draw(draw,FramebufferFetch::ext);
    check(bool(arm) && bool(ext),"coherent ARM and EXT paths admitted");
    check(!arm.pipeline.raster.blend && !arm.pipeline.full_replacement,"shader blend never enables GL blend or discards previous EFB");
    check(arm.pipeline.key!=ext.pipeline.key,"fetch dialect is part of shader identity");
    check(arm.pipeline.fragment.find("gl_LastFragColorARM")!=std::string::npos &&
        ext.pipeline.fragment.find("inout highp vec4 gxFragmentColor")!=std::string::npos,"destination is read coherently");
}
void depth_shader_order() {
    Fixture f;f.full_opaque_quad();f.bp(0x40,31);
    auto captured=f.capture();auto late=plan_gles_draw(packet(captured));
    check(bool(late) && !late.pipeline.full_replacement && late.pipeline.fragment.find("early_fragment_tests")==std::string::npos,"late Z preserves alpha-discard ordering");
    f.bp(0x43,64);captured=f.capture();auto early=plan_gles_draw(packet(captured));
    check(bool(early) && early.pipeline.fragment.find("layout(early_fragment_tests) in;")!=std::string::npos && early.pipeline.key!=late.pipeline.key,"early Z is explicit and part of shader identity");
}
void rgb8_alpha_update() {
    Fixture f;
    const auto base = plan_gles_draw(packet(f.capture()));
    f.bp(0x41, 0x00f4bd);
    const auto enabled = plan_gles_draw(packet(f.capture()));
    check(bool(base) && bool(enabled), "RGB8 alpha-update/dither on/off draws admitted");
    check(base.pipeline.key == enabled.pipeline.key && !enabled.pipeline.alpha_write,
          "RGB8 alpha update does not change shader or write backing alpha");
}
void routing_ownership_and_keys() {
    Fixture f;
    const auto captured = f.capture(); const auto& p = packet(captured);
    check(p.texture_mask == 8 && !p.textures[0] && p.textures[3] && f.memory.reads == 1, "stage/coord/map routing");
    check(p.vertices->indices().size() == 6 && p.vertices->vertices()[1].colors[0][1] == 255, "owned quad and gradient");
    const auto g = plan_gles_draw(p); const auto& s = pipeline(g);
    check(s.coordinate_scale[0] == std::array<float,2>{{1,1}}, "SU and image dimensions cancel once");
    check(s.fragment.find("texture(uGxTexture3,(gx_tex1.xy/gx_tex1.z)") != std::string::npos, "sampler uses actual routing");
    check(s.fragment.find("uGxTexture0") == std::string::npos, "unused map not declared");
    std::fill(f.memory.bytes.begin(), f.memory.bytes.end(), 0x42);
    f.bp(0x32, 15); f.bp(0xf3, 0x3f1234);
    const auto changed = f.capture(); const auto& q = packet(changed);
    check(p.textures[3]->levels()[0].rgba[0] == 0xa5 && q.textures[3]->levels()[0].rgba[0] == 0x42,
          "same address mutation cannot alter or reuse earlier texture bytes");
    const auto newer = plan_gles_draw(q); const auto& ns = pipeline(newer);
    check(ns.coordinate_scale[0][0] == 2 && ns.key == s.key, "dimensions/scales/alpha refs are uniforms");
    std::fill(f.vertices.begin(), f.vertices.end(), 0);
    check(p.vertices->vertices()[0].position[0] == 100, "source vertex mutation cannot alter packet");
    auto mixed = p; mixed.textures[3] = q.textures[3];
    check(!plan_gles_draw(mixed), "mixed snapshot revisions rejected");
    mixed = p; mixed.texture_mask = 0; check(!plan_gles_draw(mixed), "resource mask mismatch rejected");
    mixed = p; mixed.tev.reset(); check(!plan_gles_draw(mixed), "incomplete packet rejected");
}
void automatic_repeat_coordinates() {
    Fixture f;
    f.bp(0x83, 1u | (1u << 2));
    f.bp(0x32, 0x10007); f.bp(0x33, 0x10003);
    const auto repeat = f.capture();
    const auto plan = plan_gles_draw(packet(repeat));
    check(pipeline(plan).coordinate_scale[0] == std::array<float,2>{{1,1}},
          "automatic repeat bias keeps normalized coordinates");
    f.bp(0x32, 0x1000f);
    check(!plan_gles_draw(packet(f.capture())), "manually scaled biased coordinates remain rejected");
    f.bp(0x32, 0x30007);
    check(!plan_gles_draw(packet(f.capture())), "cylindrical wrap is distinct from repeat");
}
void shared_maps_and_dead_inputs() {
    Fixture f;
    f.bp(0x00, 0x412);
    f.bp(0xc2, 0x8f8af); f.bp(0xc3, 0x80000 | (7u << 13) | (4u << 10) | (5u << 7) | (7u << 4));
    f.bp(0x28, 0x4b | (0x4bu << 12));
    const auto same = f.capture(); check(packet(same).texture_mask == 8 && f.memory.reads == 1, "two stages share one owned map");
    f.bp(0xc2, 0x8fff0); f.bp(0xc3, 0x8ff80); f.bp(0x28, 0x4b | (0x3ffu << 12));
    const auto dead = f.capture(); packet(dead); pipeline(plan_gles_draw(*dead.packet));
    check(dead.packet->texture_mask == 8 && f.memory.reads == 2, "disabled dead second lookup needs no map7");
    TevStage stage;
    stage.color.inputs.fill(TevColorInput::zero); stage.alpha.inputs.fill(TevAlphaInput::zero);
    stage.alpha.operation = TevOperation::r8_gt;
    stage.alpha.inputs[0] = TevAlphaInput::texture; stage.alpha.inputs[1] = TevAlphaInput::raster;
    check(!tev_input_use(stage).texture && !tev_input_use(stage).raster_mask, "color comparison replaces alpha A/B inputs");
    stage.alpha.inputs[2] = TevAlphaInput::texture;
    check(!tev_input_use(stage).texture && tev_input_use(stage).constant_texture, "alpha C reads the disabled lookup's constant");
    stage.texture_enabled = true; check(tev_input_use(stage).texture, "alpha C still consumed");
}
void rejection_and_limits() {
    Fixture f;
    const auto good = f.capture(); const auto bytes = packet(good).retained_bytes;
    DrawLimits limits; limits.max_retained_bytes = bytes;
    check(bool(f.capture(limits)), "exact retained boundary accepted");
    limits.max_retained_bytes = bytes - 1;
    auto bad = f.capture(limits); check(!bad && !bad.packet && bad.status.error == DrawError::storage_limit, "one byte below retained limit fails");
    limits.max_retained_bytes = 0; bad = f.capture(limits); check(!bad && !bad.packet, "zero storage limit atomic");
    f.memory.fail = true; bad = f.capture(); check(!bad && !bad.packet && bad.status.error == DrawError::texture, "source read failure atomic");
    f.memory.fail = false; f.xf(0x103f, 1); bad = f.capture();
    check(!bad && bad.status.error == DrawError::missing_input, "missing coordinate rejected");
    f.xf(0x103f, 2); f.xf(0x1009, 0); bad = f.capture();
    check(!bad && bad.status.error == DrawError::missing_input, "missing raster channel rejected");
    f.xf(0x1009, 1);
    const std::pair<unsigned,unsigned> unsupported[] = {
        {0x83,4u<<5},{0x83,1u<<19},{0x83,1u<<21},
        {0x87,1},{0x32,0x10007},{0x33,0x20003}
    };
    for (auto [reg,value] : unsupported) {
        Fixture t; t.bp(reg,value); const auto c = t.capture();
        if (c) check(!plan_gles_draw(*c.packet), "unverified sampler/coordinate state rejected");
        else check(c.status.error == DrawError::texture, "unsupported sampler must fail in texture or GLES layer");
    }
    Fixture linear; linear.bp(0x83, (4u<<5)|16);
    const auto c = linear.capture(); pipeline(plan_gles_draw(packet(c)));
}
}
void direct_parity(Fixture& f, FramebufferFetch fetch, const char* name) {
    const auto captured = f.capture(); const auto& draw = packet(captured);
    const auto planned = plan_gles_draw(draw, fetch); const auto& gpu = pipeline(planned);
    const auto command = f.command();
    const auto program = plan_direct_program(f.decoder.state(), command.layout, fetch);
    check(bool(program), program.status.message.c_str());
    check(program.program->vertex == gpu.vertex && program.program->fragment == gpu.fragment, (std::string(name)+" #1").c_str());
    check(program.program->maps == draw.texture_mask && program.program->rgba6 == gpu.rgba6_copy, (std::string(name)+" #2").c_str());
    DirectDrawState state;
    const auto status = plan_direct_state(f.decoder.state(), *program.program, TextureCmprModel::reject_unverified, state);
    check(bool(status), status.message.c_str());
    const auto& a = state.raster; const auto& b = gpu.raster;
    check(a.viewport.x == b.viewport.x && a.viewport.y == b.viewport.y && a.viewport.width == b.viewport.width &&
          a.viewport.height == b.viewport.height && a.scissor.x == b.scissor.x && a.scissor.y == b.scissor.y &&
          a.scissor.width == b.scissor.width && a.scissor.height == b.scissor.height && a.blend == b.blend &&
          a.color_write == b.color_write && a.depth_test == b.depth_test && a.depth_write == b.depth_write &&
          a.early_depth == b.early_depth && a.depth_range == b.depth_range && a.depth_compare == b.depth_compare &&
          a.source_factor == b.source_factor && a.destination_factor == b.destination_factor && a.cull == b.cull, (std::string(name)+" #3").c_str());
    check(state.coordinate_scale == gpu.coordinate_scale && state.image_size == gpu.image_size &&
          state.indirect_scale == gpu.indirect_scale && state.pixel_controls == gpu.pixel_controls &&
          state.alpha_write == gpu.alpha_write, (std::string(name)+" #4").c_str());
    for (unsigned map = 0; map < 8; ++map)
        if (draw.textures[map]) check(state.sampler_lod[map] == gpu.sampler_lod[map], (std::string(name)+" #5").c_str());
    check(state.tev.registers == draw.tev->uniforms.registers && state.tev.konst == draw.tev->uniforms.konst &&
          state.tev.alpha_reference == draw.tev->uniforms.alpha_reference, (std::string(name)+" #6").c_str());
    if (draw.indirect->lookup_mask) for (unsigned i = 0; i < 6; ++i) check(state.indirect_rows[i] == draw.indirect->matrices[i / 2].rows[i % 2], (std::string(name)+" #7").c_str());
    std::vector<std::uint32_t> words, changed;
    direct_program_words(f.decoder.state(), command.layout, fetch, DirectShading::exact, words);
    TransformUniforms uniforms; direct_transform_uniforms(f.decoder.state(), uniforms);
    TransformUniforms reused; std::memset(&reused, 0xab, sizeof reused); direct_transform_uniforms(f.decoder.state(), reused);
    check(!std::memcmp(&reused, &uniforms, sizeof reused), "direct uniforms write every byte of a reused block");
    for (const auto& v : draw.vertices->vertices()) {
        const unsigned row = v.matrix_indices[0];
        for (unsigned k = 0; k < 3; ++k)
            check(uniforms.position[row + k] == draw.transform->uniforms.position[row + k], "direct position rows");
    }
    check(uniforms.projection == draw.transform->uniforms.projection && uniforms.viewport == draw.transform->uniforms.viewport,
          "direct projection and viewport");
    f.bp(0xf3, (f.decoder.state().bp(0xf3) & 0xff0000) | 0x1234);
    f.xf(0, 0x3f800000);
    direct_program_words(f.decoder.state(), command.layout, fetch, DirectShading::exact, changed);
    check(words == changed, "alpha references and matrices are not program words");
}
void direct_parity_cases() {
    { Fixture f; direct_parity(f, FramebufferFetch::none, "direct default orthographic"); }
    { Fixture f; f.memory.bytes.resize(96); f.bp(0x83,0x1e1d5); f.bp(0x87,0x2000); direct_parity(f, FramebufferFetch::none, "direct mipmap"); }
    { Fixture f; f.xf(0x1026,0); f.f(0x1020,1); f.f(0x1021,0); f.f(0x1022,1); f.f(0x1023,0);
      f.f(0x1024,-1.0f/3); f.f(0x1025,-4.0f/3); direct_parity(f, FramebufferFetch::none, "direct perspective"); }
    { Fixture f; f.bp(0,0x10012); f.bp(0x10,0x270); f.bp(0x27,1);
      f.bp(6,0xc00200); f.bp(7,0xd00000); f.bp(8,0);
      f.bp(0x81,0); f.bp(0x85,0); f.bp(0x89,0x100c07); f.bp(0x95,0x8000>>5);
      direct_parity(f, FramebufferFetch::none, "direct indirect"); }
    { Fixture f; f.bp(0,0x10012); f.bp(0x10,0x270); f.bp(0x27,3 | (1u<<3));
      f.bp(6,0xc00200); f.bp(7,0xd00000); f.bp(8,0);
      direct_parity(f, FramebufferFetch::none, "direct indirect sharing the regular map"); }
    { Fixture f; f.bp(0,0x10012); f.bp(0x10,0x270|(6u<<13)|(6u<<16)); f.bp(0x27,1);
      f.bp(6,0xc00200); f.bp(7,0xd00000); f.bp(8,0);
      f.bp(0x81,0); f.bp(0x85,0); f.bp(0x89,0x100c07); f.bp(0x95,0x8000>>5);
      direct_parity(f, FramebufferFetch::none, "direct indirect replacing the regular coordinate"); }
    { Fixture f; f.full_opaque_quad(); f.bp(0x43,1); f.bp(0x41,0xf7d9); direct_parity(f, FramebufferFetch::arm, "direct rgba6 fetch"); }
    { Fixture f; f.full_opaque_quad(); direct_parity(f, FramebufferFetch::none, "direct full quad"); }
    Fixture f; f.bp(0x41, 0xf4a9 & ~1u);
    std::vector<std::uint32_t> before, after;
    direct_program_words(f.decoder.state(), f.command().layout, FramebufferFetch::none, DirectShading::exact, before);
    f.bp(0x41, 0xf4a9);
    direct_program_words(f.decoder.state(), f.command().layout, FramebufferFetch::none, DirectShading::exact, after);
    check(before == after, "RGB8 blending is raster state, not program identity");
    f.bp(0x43, 1);
    direct_program_words(f.decoder.state(), f.command().layout, FramebufferFetch::none, DirectShading::exact, after);
    check(before != after, "pixel format is program identity");
}

void fast_program_rebuild(Fixture& f, const char* name) {
    const auto command = f.command();
    for (bool batched : {false, true}) {
        const auto fast = plan_direct_program(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast, batched);
        check(batched || bool(fast), name);
        if (!fast) continue;
        std::vector<std::uint32_t> words;
        direct_program_words(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast, words);
        State state; VertexLayout layout;
        check(direct_program_state(words, state, layout), name);
        const auto rebuilt = plan_direct_program(state, layout, FramebufferFetch::none, DirectShading::fast, batched);
        check(rebuilt && rebuilt.program->vertex == fast.program->vertex && rebuilt.program->fragment == fast.program->fragment, name);
    }
}
// A lit draw without normals lights with the latched normal; program lists record the latch
// in the words, and a list entry must plan the same program the draw did.
void latched_normal_program_rebuild() {
    Fixture f; f.xf(0x1009, 1); f.xf(0x100e, (1u << 1) | (1u << 2) | (2u << 7));
    const auto command = f.command();
    const auto drawn = plan_direct_program(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast, false, 3);
    check(bool(drawn) && drawn.program->transform.latched_normal, "lit program with a latched normal plans");
    std::vector<std::uint32_t> words;
    direct_program_words(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast, words, 3);
    State state; VertexLayout layout; std::uint8_t latch = 0;
    check(!direct_program_state(words, state, layout), "latched words are refused to a caller that cannot take the latch");
    check(direct_program_state(words, state, layout, &latch) && latch == 3, "latched words rebuild with their latch");
    const auto rebuilt = plan_direct_program(state, layout, FramebufferFetch::none, DirectShading::fast, false, latch);
    check(rebuilt && drawn && rebuilt.program->vertex == drawn.program->vertex && rebuilt.program->fragment == drawn.program->fragment,
          "latched words plan the drawn program");
    for (std::uint32_t bad : {5u, 15u}) {
        auto wrong = words; wrong[0] = (wrong[0] & ~(15u << 21)) | (bad << 21);
        check(!direct_program_state(wrong, state, layout, &latch), "an impossible latch is refused");
    }
}
void fast_program_rebuilds() {
    { Fixture f; fast_program_rebuild(f, "fast program rebuilds from its words"); }
    { Fixture f; f.bp(0xe2, 0x7ffu | (300u << 12)); fast_program_rebuild(f, "fast program with wide registers rebuilds"); }
    { Fixture f; f.bp(0xf1, 4u << 21); f.bp(0xe8, 1u << 10); fast_program_rebuild(f, "fast program with fog rebuilds"); }
    {
        // Fog off with projection and range adjustment left set: they are not program identity,
        // and the state plan accepts the program.
        Fixture plain, f; f.bp(0xf1, 1u << 20); f.bp(0xe8, 1u << 10);
        const auto command = f.command();
        std::vector<std::uint32_t> a, b;
        direct_program_words(plain.decoder.state(), plain.command().layout, FramebufferFetch::none, DirectShading::fast, a);
        direct_program_words(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast, b);
        check(a == b, "fog projection and range adjustment are ignored while fog is off");
        const auto program = plan_direct_program(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast);
        check(bool(program), program.status.message.c_str());
        DirectDrawState state;
        const auto status = plan_direct_state(f.decoder.state(), *program.program, TextureCmprModel::reject_unverified, state);
        check(bool(status), status.message.c_str());
    }
    { Fixture f; f.bp(0,0x10012); f.bp(0x10,0x270); f.bp(0x27,1); f.bp(6,0xc00200); f.bp(7,0xd00000); f.bp(8,0);
      f.bp(0x81,0); f.bp(0x85,0); f.bp(0x89,0x100c07); f.bp(0x95,0x8000>>5);
      fast_program_rebuild(f, "fast indirect program rebuilds"); }
    std::vector<std::uint32_t> words(101);
    State state; VertexLayout layout;
    check(!direct_program_state(words, state, layout), "words without the fast flag are refused");
    latched_normal_program_rebuild();
}
// The default fixture's stage reads TEXC from map 3; with its lookup disabled the draw binds and
// samples no texture and reads the constant (draw.md).
void disabled_lookup_draws() {
    for (const bool texgens : {true, false}) {
        Fixture f; f.bp(0x28, 0x0b);
        if (!texgens) f.bp(0x00, 0x10);
        const auto captured = f.capture(); const auto& draw = packet(captured);
        check(draw.texture_mask == 0, "a disabled lookup captures no texture");
        const auto planned = plan_gles_draw(draw); const auto& gpu = pipeline(planned);
        check(gpu.fragment.find("uGxTexture") == std::string::npos &&
              gpu.fragment.find(texgens ? "ivec4 tex = ivec4(255);" : "ivec4 tex = ivec4(0);") != std::string::npos,
              "exact program reads the constant and declares no sampler");
        const auto command = f.command();
        const auto fast = plan_direct_program(f.decoder.state(), command.layout, FramebufferFetch::none, DirectShading::fast);
        check(bool(fast), fast.status.message.c_str());
        check(fast.program->maps == 0 && fast.program->fragment.find("uGxTexture") == std::string::npos &&
              fast.program->fragment.find(texgens ? "vec4 tex=vec4(1.0);" : "vec4 tex=vec4(0.0);") != std::string::npos,
              "fast program reads the constant and binds no map");
        fast_program_rebuild(f, "disabled-lookup program rebuilds from its words");
    }
}
int main(int argc, char** argv) {
    try {
        direct_parity_cases();
        fast_program_rebuilds();
        disabled_lookup_draws();
        clipped_texture_projection(); clip_disabled_draws(); offscreen_clip_disabled_draws(); clip_disabled_behind_eye(); deferred_shader_source(); mipmap_lod_controls(); perspective_integration(); inactive_indirect_inputs(); indirect_lookup_and_offsets(); rgb8_full_replacement(); rgb8_alpha_update(); rgba6_fetch_boundary(); depth_shader_order(); routing_ownership_and_keys(); automatic_repeat_coordinates(); shared_maps_and_dead_inputs(); rejection_and_limits();
        if (argc == 2) {
            Fixture f; const auto c = f.capture(); const auto p = plan_gles_draw(packet(c));
            std::ofstream(std::string(argv[1]) + ".vert") << pipeline(p).vertex;
            std::ofstream(std::string(argv[1]) + ".frag") << p.pipeline.fragment;
        }
        std::cout << "GX owned draw and GLES pipeline tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
