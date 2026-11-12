#include "starlet/gx/tev.hpp"
#include "starlet/gx/decoder.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace starlet::gx;
namespace {
constexpr auto model = TevArithmeticModel::fixed8_candidate_v1;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void ok(Status status) { require(static_cast<bool>(status), status.message.c_str()); }
struct Memory final : MemoryReader { bool copy(std::uint32_t, std::uint8_t*, std::size_t) override { return false; } };
struct Sink final : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
void bp(Decoder& d, unsigned address, std::uint32_t word) {
    ok(d.write_u8(0x61)); ok(d.write_u32((address << 24) | word));
}
void reg(Decoder& d, unsigned index, const TevSignedColor& color, bool konst = false) {
    const auto tag = konst ? 0x800000u : 0u;
    bp(d, 0xe0 + index * 2, tag | (unsigned(color[0]) & 2047) | ((unsigned(color[3]) & 2047) << 12));
    bp(d, 0xe1 + index * 2, tag | (unsigned(color[2]) & 2047) | ((unsigned(color[1]) & 2047) << 12));
}
TevPipeline blank() {
    TevPipeline p;
    for (auto& stage : p.stages) {
        stage.color.inputs.fill(TevColorInput::zero);
        stage.alpha.inputs.fill(TevAlphaInput::zero);
        stage.color.clamp = stage.alpha.clamp = true;
    }
    return p;
}
TevEvaluation eval(const TevPipeline& p, const TevUniforms& u = {}, const TevInputs& in = {}) {
    auto result = evaluate_tev(p, u, in, model);
    require(static_cast<bool>(result), result.status.message.c_str());
    return result;
}

void decode_capture_and_keys() {
    static_assert(std::is_const_v<decltype(TevMaterial::pipeline)>);
    static_assert(std::is_const_v<decltype(TevMaterial::uniforms)>);
    static_assert(sizeof(TevMaterial) < 2048);
    Memory memory; Sink sink; Decoder d(memory, sink);
    // Stage0: reg1 RGB = (reg0 RGB + prev RGB + half bias)*4, signed.
    // Alpha compares color R, then writes reg2; stage1 passes those results to PREV.
    bp(d, 0x00, 1u << 10);
    bp(d, 0xc0, (2u << 12) | (15u << 8) | (15u << 4) | (1u << 16) | (2u << 20) | (2u << 22));
    bp(d, 0xc1, (7u << 13) | (7u << 10) | (6u << 7) | (2u << 4) | (3u << 16) | (1u << 18) | (3u << 22) | 9);
    bp(d, 0xc2, 0x8fff4); // zero A/B/C, D=reg1 RGB, clamped.
    bp(d, 0xc3, 0x8ffb0); // zero A/B/C, D=reg2 alpha, clamped.
    bp(d, 0x28, 0x380u | (0x38au << 12)); // no lookup; null raster. Odd stage map2/coord1.
    bp(d, 0xf6, (0x10u << 9) | 4); // stage0 alpha K0.R, swap0 RG=(R,G).
    bp(d, 0xf7, 14); // swap0 BA=(B,A).
    bp(d, 0xf8, 11); bp(d, 0xf9, 1); // swap1 RG=(A,B), BA=(G,R).
    bp(d, 0xfa, 0); bp(d, 0xfb, 15); // swap2 RG=(R,R), BA=(A,A).
    bp(d, 0xf3, 0x31u | (0x92u << 8) | (4u << 16) | (1u << 19) | (2u << 22));
    for (unsigned i = 0; i < 4; ++i) {
        reg(d, i, {{static_cast<short>(-1024 + int(i)), static_cast<short>(1023 - int(i)), -1, 256}});
        reg(d, i, {{static_cast<short>(10 + i), static_cast<short>(20 + i), static_cast<short>(30 + i), static_cast<short>(40 + i)}}, true);
    }
    auto captured = capture_tev(d.state());
    require(static_cast<bool>(captured), captured.status.message.c_str());
    const auto& p = captured.material->pipeline;
    const auto& u = captured.material->uniforms;
    require(p.stage_count == 2 && p.stages[0].color.inputs[0] == TevColorInput::reg0_rgb &&
            p.stages[0].color.inputs[3] == TevColorInput::prev_rgb, "color selector decode");
    require(p.stages[0].color.bias == TevBias::add_half && p.stages[0].color.scale == TevScale::four &&
            !p.stages[0].color.clamp && p.stages[0].color.destination == TevRegister::reg1, "color arithmetic decode");
    require(p.stages[0].alpha.operation == TevOperation::r8_eq && p.stages[0].alpha.destination == TevRegister::reg2 &&
            p.stages[0].alpha.scale == TevScale::one && p.stages[0].alpha.bias == TevBias::zero, "compare encoding decodes without arithmetic bias/scale");
    require(p.stages[0].raster_swap == std::array<std::uint8_t, 4>{{3,2,1,0}} &&
            p.stages[0].texture_swap == std::array<std::uint8_t, 4>{{0,0,3,3}}, "independent swap tables");
    require(p.stages[1].texture_map == 2 && p.stages[1].texture_coord == 1 && !p.stages[1].texture_enabled, "odd TREF fields");
    require(u.registers[2] == TevSignedColor{{-1022,1021,-1,256}} && u.konst[2] == TevColor{{12,22,32,42}}, "both bank RGBA ordering and sign extension");
    require(u.alpha_reference == std::array<std::uint8_t,2>{{0x31,0x92}} && p.alpha_compare[0] == TevCompare::greater &&
            p.alpha_compare[1] == TevCompare::less && p.alpha_logic == TevAlphaLogic::xor_, "alpha compare fields");
    auto shader = generate_tev_shader(p, model); require(static_cast<bool>(shader), "captured shader generation");
    const auto revision = captured.material->state_revision;
    reg(d, 2, {{1,2,3,4}}); reg(d, 2, {{5,6,7,8}}, true);
    bp(d, 0xf3, (d.state().bp(0xf3) & 0xff0000) | 0xabcd);
    const auto newer = capture_tev(d.state()); require(static_cast<bool>(newer), "recapture");
    require(captured.material->state_revision == revision && u.registers[2][0] == -1022 && u.konst[2][0] == 12,
            "immutable capture survives state mutation");
    require(generate_tev_shader(newer.material->pipeline, model).key == shader.key, "register/konst/ref values do not specialize shader");
    auto changed = p; changed.stages[15].color.bias = TevBias::subtract_half;
    require(generate_tev_shader(changed, model).key == shader.key, "inactive stage excluded from key");
    changed.stages[0].color.bias = TevBias::subtract_half;
    require(generate_tev_shader(changed, model).key != shader.key, "active equation included in key");
    changed = p; changed.stages[1].texture_map ^= 1;
    require(generate_tev_shader(changed, model).key != shader.key, "texture routing included in pipeline key");
    // Same-bank masks update only selected bits, then material decode reads the retained bank.
    bp(d, 0xfe, 0xff); bp(d, 0xe4, 99);
    auto masked = capture_tev(d.state());
    require(masked.material->uniforms.konst[2] == TevColor{{99,6,7,8}} && masked.material->uniforms.registers[2] == TevSignedColor{{1,2,3,4}},
            "same-bank partial mask survives material decoding");
}

void konst_selector_decode() {
    for (unsigned odd = 0; odd < 2; ++odd) for (unsigned alpha = 0; alpha < 2; ++alpha) for (unsigned selector = 0; selector < 32; ++selector) {
        Memory memory; Sink sink; Decoder d(memory, sink);
        bp(d, 0, odd << 10);bp(d, 0xc0, 0x8ffff);bp(d, 0xc1, 0x8fff0);
        bp(d, 0xc0 + odd * 2, alpha ? 0x8ffff : 0x8fffe);
        bp(d, 0xc1 + odd * 2, alpha ? 0x8ffe0 : 0x8fff0);
        bp(d, 0xf6, selector << ((alpha ? 9 : 4) + odd * 10));
        const auto result = capture_tev(d.state());
        const bool valid = selector < 8 || selector >= (alpha ? 16u : 12u);
        require(static_cast<bool>(result) == valid, "all raw konst selector encodings");
        if (!valid) continue;
        const auto& k = alpha ? result.material->pipeline.stages[odd].alpha_konst : result.material->pipeline.stages[odd].color_konst;
        if (selector < 8) require(k.kind == TevKonstKind::fraction && k.index == selector, "konst fraction decode");
        else if (selector < 16) require(k.kind == TevKonstKind::rgb && k.index == selector - 12, "konst RGB register decode");
        else require(k.kind == TevKonstKind::component && k.index == (selector & 3) && k.component == (selector - 16) / 4, "konst scalar register/channel decode");
    }
}

void every_selector_and_swaps() {
    TevUniforms u; TevInputs in;
    u.registers = {{{{1,2,3,4}}, {{11,12,13,14}}, {{21,22,23,24}}, {{31,32,33,34}}}};
    u.konst[2] = {{101,102,103,104}};
    in.texture[0] = {{41,42,43,44}}; in.raster[1] = {{51,52,53,54}};
    in.texture_valid = 1; in.raster_valid = 2;
    const std::array<TevColor,16> expected{{
        {{1,2,3,0}},{{4,4,4,0}},{{11,12,13,0}},{{14,14,14,0}},{{21,22,23,0}},{{24,24,24,0}},{{31,32,33,0}},{{34,34,34,0}},
        {{41,42,43,0}},{{44,44,44,0}},{{51,52,53,0}},{{54,54,54,0}},{{255,255,255,0}},{{128,128,128,0}},{{101,102,103,0}},{{0,0,0,0}}
    }};
    for (unsigned selector = 0; selector < 16; ++selector) {
        auto p = blank(); auto& stage = p.stages[0]; stage.texture_enabled = true; stage.raster = TevRaster::color1;
        stage.color.inputs[3] = static_cast<TevColorInput>(selector); stage.color_konst = {TevKonstKind::rgb,2,0};
        require(eval(p,u,in).color == expected[selector], "all color selectors");
    }
    const unsigned expected_alpha[] = {4,14,24,34,44,54,102,0};
    for (unsigned selector = 0; selector < 8; ++selector) {
        auto p = blank(); auto& stage = p.stages[0]; stage.texture_enabled = true; stage.raster = TevRaster::color1;
        stage.alpha.inputs[3] = static_cast<TevAlphaInput>(selector); stage.alpha_konst = {TevKonstKind::component,2,1};
        require(eval(p,u,in).color[3] == expected_alpha[selector], "all alpha selectors");
    }
    auto p = blank(); auto& s = p.stages[0];s.texture_enabled = true;s.color.inputs[3] = TevColorInput::texture_rgb;s.alpha.inputs[3] = TevAlphaInput::texture;
    s.texture_swap = {{3,0,1,2}};
    require(eval(p,u,in).color == TevColor{{44,41,42,43}}, "texture swap includes alpha");
    s.raster = TevRaster::color1;s.color.inputs[3] = TevColorInput::raster_rgb;s.alpha.inputs[3] = TevAlphaInput::raster;s.raster_swap = {{2,2,2,1}};
    require(eval(p,u,in).color == TevColor{{53,53,53,52}}, "raster replication and alpha swap");
    s.raster = TevRaster::zero; in.raster_valid = 0;
    require(eval(p,u,in).color == TevColor{}, "explicit zero raster channel");
    s.color.inputs[3] = TevColorInput::konst;s.alpha.inputs[3] = TevAlphaInput::konst;
    for(unsigned index=0;index<8;++index) {
        s.color_konst = s.alpha_konst = {TevKonstKind::fraction,static_cast<std::uint8_t>(index),0};
        const unsigned n=index?256-32*index:255;const auto result=eval(p,u,in);
        require(result.color == TevColor{{static_cast<std::uint8_t>(n),static_cast<std::uint8_t>(n),static_cast<std::uint8_t>(n),static_cast<std::uint8_t>(n)}},"all candidate konst fractions");
    }
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c) {
        u.konst[r][c]=static_cast<std::uint8_t>(100+r*4+c);
        s.color_konst=s.alpha_konst={TevKonstKind::component,static_cast<std::uint8_t>(r),static_cast<std::uint8_t>(c)};
        const auto result=eval(p,u,in);require(result.color[0]==u.konst[r][c] && result.color[3]==u.konst[r][c],"all konst components");
    }
}

void arithmetic_candidate() {
    const int samples[] = {0,1,127,128,254,255};
    const int accumulators[] = {-1024,-511,-1,0,1,511,1023};
    auto p = blank();auto& s=p.stages[0];
    s.color.inputs = {{TevColorInput::reg0_rgb,TevColorInput::reg1_rgb,TevColorInput::reg2_rgb,TevColorInput::prev_rgb}};
    s.alpha.inputs = {{TevAlphaInput::reg0,TevAlphaInput::reg1,TevAlphaInput::reg2,TevAlphaInput::prev}};
    TevUniforms u;
    for(int a:samples)for(int b:samples)for(int c:samples)for(int d:accumulators)
    for(unsigned bias=0;bias<3;++bias)for(unsigned scale=0;scale<4;++scale)for(unsigned sub=0;sub<2;++sub)for(unsigned clamp=0;clamp<2;++clamp) {
        u.registers[0].fill(static_cast<short>(d));u.registers[1].fill(static_cast<short>(a));u.registers[2].fill(static_cast<short>(b));u.registers[3].fill(static_cast<short>(c));
        s.color.bias=s.alpha.bias=static_cast<TevBias>(bias);s.color.scale=s.alpha.scale=static_cast<TevScale>(scale);
        s.color.operation=s.alpha.operation=static_cast<TevOperation>(sub);s.color.clamp=s.alpha.clamp=clamp!=0;
        const long double factor=(c+c/128)/256.0L;
        const long double offset=bias==1?128:(bias==2?-128:0);
        const long double multiplier=scale==3?0.5L:static_cast<long double>(1u<<scale);
        const long double sum=(d+offset+(sub?-1:1)*((1-factor)*a+factor*b))*multiplier;
        int expected=static_cast<int>(std::floor(sum+0.5L));expected=std::clamp(expected,clamp?0:-1024,clamp?255:1023);
        const auto result=eval(p,u);
        require(result.registers[0][0]==expected && result.registers[0][3]==expected,"declared candidate arithmetic against rational oracle");
        require(result.color[0]==static_cast<std::uint8_t>(unsigned(expected)&255),"declared candidate signed final conversion");
    }
    // Signed registers are low8 only on A/B/C; D retains its signed11 value.
    s.color.bias=s.alpha.bias=TevBias::zero;s.color.scale=s.alpha.scale=TevScale::one;s.color.operation=s.alpha.operation=TevOperation::add;
    s.color.clamp=s.alpha.clamp=false;u.registers[0].fill(-100);u.registers[1].fill(-1);u.registers[2].fill(1023);u.registers[3].fill(256);
    auto result=eval(p,u);require(result.registers[0][0]==155 && result.registers[0][3]==155,"signed11 ABC truncation and signed D accumulation");
}

void comparisons_and_stage_order() {
    auto p=blank();auto& s=p.stages[0];TevUniforms u;
    s.color.inputs={{TevColorInput::reg0_rgb,TevColorInput::reg1_rgb,TevColorInput::reg2_rgb,TevColorInput::prev_rgb}};
    s.alpha.inputs={{TevAlphaInput::reg0,TevAlphaInput::reg1,TevAlphaInput::reg2,TevAlphaInput::prev}};
    s.color.clamp=s.alpha.clamp=false;
    for(unsigned op=8;op<16;++op) {
        const bool equal=op&1;const unsigned width=(op-8)/2;
        u.registers[0]={{-100,1000,10,-100}};
        u.registers[1]=equal?TevSignedColor{{7,4,2,8}}:TevSignedColor{{5,4,3,8}};
        u.registers[2]=equal?TevSignedColor{{7,4,3,8}}:TevSignedColor{{4,5,2,7}};
        u.registers[3]={{10,20,30,100}};s.color.operation=s.alpha.operation=static_cast<TevOperation>(op);
        const std::array<bool,3> pass=width==3?(equal?std::array<bool,3>{{true,true,false}}:std::array<bool,3>{{true,false,true}})
            :std::array<bool,3>{{equal?width<2:width!=1,equal?width<2:width!=1,equal?width<2:width!=1}};
        const auto result=eval(p,u);
        for(unsigned c=0;c<3;++c)require(result.registers[0][c]==u.registers[0][c]+(pass[c]?u.registers[3][c]:0),"all RGB compare widths/order");
        const bool alpha_pass=width==3?true:pass[0];
        require(result.registers[0][3]==(alpha_pass?0:-100),"alpha RGB compares use color operands; A8 uses alpha operands");
    }
    // Same-stage color writes cannot alter alpha's color-input comparison.
    p=blank();p.stage_count=2;auto& a=p.stages[0];auto& b=p.stages[1];
    a.color.inputs[0]=TevColorInput::reg0_rgb;a.color.inputs[2]=TevColorInput::one;a.color.destination=TevRegister::reg0;
    a.alpha.inputs[2]=TevAlphaInput::konst;a.alpha.operation=TevOperation::r8_gt;
    b.color.inputs[3]=TevColorInput::reg0_rgb;b.alpha.inputs[3]=TevAlphaInput::prev;
    u={};u.registers[1]={{3,4,5,0}};
    require(eval(p,u).color==TevColor{{0,0,0,255}},"simultaneous color/alpha stage commit");
    p=blank();p.stage_count=16;
    for(unsigned i=0;i<16;++i) {
        p.stages[i].color.inputs[0]=TevColorInput::one;p.stages[i].color.inputs[3]=TevColorInput::prev_rgb;
        p.stages[i].color.clamp=i==15;
        p.stages[i].alpha.inputs[0]=TevAlphaInput::konst;p.stages[i].alpha.inputs[3]=TevAlphaInput::prev;
        p.stages[i].alpha_konst={TevKonstKind::fraction,7,0};p.stages[i].alpha.clamp=false;
    }
    auto result=eval(p);require(result.color==TevColor{{255,255,255,0}} && result.registers[0][3]==512,"all sixteen stages and signed accumulation");
}

void alpha_tests_and_errors() {
    auto p=blank();p.stages[0].alpha.inputs[3]=TevAlphaInput::prev;TevUniforms u;
    auto compare=[](unsigned op,int a,int b) { switch(op) {case 0:return false;case 1:return a<b;case 2:return a==b;case 3:return a<=b;case 4:return a>b;case 5:return a!=b;case 6:return a>=b;default:return true;} };
    for(unsigned a=0;a<256;++a)for(unsigned c0=0;c0<8;++c0)for(unsigned c1=0;c1<8;++c1)for(unsigned logic=0;logic<4;++logic) {
        u.registers[0][3]=static_cast<short>(a);u.alpha_reference={{37,128}};
        p.alpha_compare={{static_cast<TevCompare>(c0),static_cast<TevCompare>(c1)}};p.alpha_logic=static_cast<TevAlphaLogic>(logic);
        const bool left=compare(c0,a,37),right=compare(c1,a,128);
        const bool expected=logic==0?(left&&right):logic==1?(left||right):logic==2?(left!=right):(left==right);
        require(eval(p,u).alpha_pass==expected,"all alpha-test combinations and u8 inputs");
    }
    p=blank();p.stage_count=0;require(evaluate_tev(p,{}, {},model).status.error==TevError::invalid_pipeline,"zero stage count");
    p.stage_count=17;require(!generate_tev_shader(p,model),"stage bound in generator");
    // A last stage writing other registers still outputs its own results; PREV keeps its value.
    p=blank();p.stages[0].color.inputs[3]=TevColorInput::reg1_rgb;p.stages[0].alpha.inputs[3]=TevAlphaInput::reg2;
    p.stages[0].color.destination=TevRegister::reg0;p.stages[0].alpha.destination=TevRegister::reg2;
    u={};u.registers[0]={{200,200,200,200}};u.registers[2]={{11,12,13,14}};u.registers[3]={{1,2,3,77}};
    {const auto out=eval(p,u);
     require(out.color[0]==11&&out.color[1]==12&&out.color[2]==13&&out.color[3]==77&&out.registers[0][0]==200,"last stage output leaves the TEV");}
    require(static_cast<bool>(generate_tev_shader(p,model)),"last stage may write any register");
    p=blank();require(evaluate_tev(p,{}, {},static_cast<TevArithmeticModel>(0)).status.error==TevError::unsupported_arithmetic,"no implicit arithmetic default");
    p.stages[0].color.inputs[3]=TevColorInput::texture_rgb;
    require(eval(p).color==TevColor{{255,255,255,0}},"a disabled lookup needs no texture sample");
    p.stages[0].texture_enabled=true;require(evaluate_tev(p,{}, {},model).status.error==TevError::missing_input,"missing texture sample rejected");
    p=blank();p.stages[0].raster=TevRaster::color0;p.stages[0].alpha.inputs[3]=TevAlphaInput::raster;
    require(evaluate_tev(p,{}, {},model).status.error==TevError::missing_input,"missing raster sample rejected");
    p=blank();u={};u.registers[1][0]=1024;require(evaluate_tev(p,u,{},model).status.error==TevError::invalid_uniform,"invalid signed uniform range");
    p.stages[0].texture_swap[1]=4;require(!generate_tev_shader(p,model),"invalid swizzle rejected before indexing");
    p=blank();p.stages[0].color.operation=TevOperation::r8_eq;p.stages[0].color.scale=TevScale::two;
    require(!generate_tev_shader(p,model),"compare with arithmetic scaling rejected");
    Memory memory;Sink sink;Decoder d(memory,sink);
    bp(d,0xf5,4);require(capture_tev(d.state()).status.message=="a Z texture needs a lookup in the last TEV stage","Z texture without its lookup");bp(d,0xf5,0);
    bp(d,0x00,1u<<16);require(bool(capture_tev(d.state())),"coordinate lookup is external to TEV arithmetic");
    bp(d,0x00,0);bp(d,0x10,0x200);require(bool(capture_tev(d.state())),"indirect lookup is external to TEV arithmetic");
    bp(d,0x10,0);bp(d,0x28,5u<<7);require(!capture_tev(d.state()),"bump raster explicit");
    bp(d,0x28,7u<<7);bp(d,0xc0,0x8fffe);bp(d,0xf6,8u<<4);require(!capture_tev(d.state()),"reserved active konst selector");
    bp(d,0xc0,0x8ffff);require(static_cast<bool>(capture_tev(d.state())),"unused reserved konst field is irrelevant");
}

void z_texture_stages() {
    Memory memory;Sink sink;Decoder d(memory,sink);
    bp(d,0x28,(1u<<6)|(7u<<7)); // Stage0 looks up map0/coord0 and reads no raster colour.
    bp(d,0xf4,0x123456);
    auto base=capture_tev(d.state());require(static_cast<bool>(base),"base capture");
    require(base.material->uniforms.z_texture_bias==0 && !tev_stage_samples(base.material->pipeline,0),"inactive Z texture: no bias, no lookup");
    std::vector<std::vector<std::uint8_t>> keys{generate_tev_shader(base.material->pipeline,model,false).key};
    for(unsigned op=1;op<3;++op)for(unsigned type=0;type<3;++type) {
        bp(d,0xf5,(op<<2)|type);
        const auto c=capture_tev(d.state());require(static_cast<bool>(c),c.status.message.c_str());
        const auto& p=c.material->pipeline;
        require(p.z_texture_op==op && p.z_texture_type==type && c.material->uniforms.z_texture_bias==0x123456,"Z-texture op, type and bias");
        require(tev_stage_samples(p,0) && !tev_input_use(p.stages[0]).texture,"the Z texel is the last stage's lookup");
        const auto key=generate_tev_shader(p,model,false).key;
        require(std::find(keys.begin(),keys.end(),key)==keys.end(),"op and type are shader identity");
        keys.push_back(key);
    }
    bp(d,0xf5,2);
    require(capture_tev(d.state()).material->pipeline.z_texture_type==0,"type of an inactive Z texture is ignored");
    for(unsigned word:{12u,4u|3u,8u|16u}) {bp(d,0xf5,word);require(!capture_tev(d.state()),"reserved Z-texture state");}
    bp(d,0xf5,10);bp(d,0xc0,0x8fff8); // Stage0 colour D reads the texture.
    require(capture_tev(d.state()).status.message=="the Z texture occupies the last TEV stage's texture input","Z texel is no TEV input");
    // Two stages: stage0 colours with its texture; the last stage's lookup is the Z texel.
    bp(d,0x00,1u<<10);bp(d,0x28,(1u<<6)|(7u<<7)|(((1u<<6)|(7u<<7)|1u)<<12));bp(d,0xc2,0x8fff0);
    auto two=capture_tev(d.state());require(static_cast<bool>(two),two.status.message.c_str());
    require(tev_stage_samples(two.material->pipeline,0) && tev_stage_samples(two.material->pipeline,1) &&
            !tev_stage_samples(two.material->pipeline,2),"TEV and Z lookups");
    auto p=two.material->pipeline;p.z_texture_op=3;require(generate_tev_shader(p,model).status.error==TevError::invalid_pipeline,"invalid op");
    p.z_texture_op=0;p.z_texture_type=2;require(!generate_tev_shader(p,model),"type without op");
}

struct ShaderCase { std::string name;TevPipeline pipeline;TevUniforms uniforms;TevInputs inputs; };
std::vector<ShaderCase> shader_cases() {
    std::vector<ShaderCase> cases;
    ShaderCase c{"selectors",blank(),{}, {}};
    c.pipeline.stages[0].texture_enabled=true;c.pipeline.stages[0].raster=TevRaster::color0;
    c.pipeline.stages[0].color.inputs={{TevColorInput::texture_rgb,TevColorInput::raster_rgb,TevColorInput::konst,TevColorInput::reg0_rgb}};
    c.pipeline.stages[0].alpha.inputs={{TevAlphaInput::texture,TevAlphaInput::raster,TevAlphaInput::konst,TevAlphaInput::reg0}};
    c.pipeline.stages[0].color_konst={TevKonstKind::rgb,2,0};c.pipeline.stages[0].alpha_konst={TevKonstKind::component,2,1};
    c.uniforms.konst[2]={{1,128,254,17}};c.uniforms.registers[1]={{-2,5,-10,5}};
    c.inputs.texture[0]={{250,12,35,20}};c.inputs.raster[0]={{2,240,51,230}};c.inputs.texture_valid=1;c.inputs.raster_valid=1;
    cases.push_back(c);
    for(unsigned op=8;op<16;++op) {auto cc=c;cc.name="compare_"+std::to_string(op);cc.pipeline.stages[0].color.operation=cc.pipeline.stages[0].alpha.operation=static_cast<TevOperation>(op);cases.push_back(cc);}
    for(unsigned scale=0;scale<4;++scale)for(unsigned sub=0;sub<2;++sub) {
        auto cc=c;cc.name="linear_"+std::to_string(scale)+"_"+std::to_string(sub);auto& s=cc.pipeline.stages[0];
        s.color.scale=s.alpha.scale=static_cast<TevScale>(scale);s.color.operation=s.alpha.operation=static_cast<TevOperation>(sub);
        s.color.bias=TevBias::subtract_half;s.alpha.bias=TevBias::add_half;s.color.clamp=s.alpha.clamp=false;cases.push_back(cc);
    }
    for(unsigned op=0;op<8;++op)for(unsigned logic=0;logic<4;++logic) {
        auto cc=c;cc.name="alpha_"+std::to_string(op)+"_"+std::to_string(logic);
        cc.pipeline.alpha_compare={{static_cast<TevCompare>(op),static_cast<TevCompare>((op+3)%8)}};
        cc.pipeline.alpha_logic=static_cast<TevAlphaLogic>(logic);cc.uniforms.alpha_reference={{64,128}};cases.push_back(cc);
    }
    auto cc=c;cc.name="swap_alpha_fail";cc.pipeline.stages[0].texture_swap={{3,0,1,2}};cc.pipeline.stages[0].raster_swap={{2,2,2,1}};
    cc.pipeline.alpha_compare={{TevCompare::greater,TevCompare::less}};cc.pipeline.alpha_logic=TevAlphaLogic::and_;cc.uniforms.alpha_reference={{200,100}};cases.push_back(cc);
    cc=c;cc.name="sixteen_stages";cc.pipeline.stage_count=16;
    for(unsigned i=1;i<16;++i) {auto& s=cc.pipeline.stages[i];s.color.inputs[3]=TevColorInput::prev_rgb;s.color.inputs[0]=TevColorInput::half;s.color.clamp=false;s.alpha.inputs[3]=TevAlphaInput::prev;s.alpha.inputs[0]=TevAlphaInput::konst;s.alpha_konst={TevKonstKind::fraction,7,0};s.alpha.clamp=false;}
    cases.push_back(cc);return cases;
}
template<class Array> void json_array(std::ostream& out,const Array& array) {
    out<<'[';bool first=true;for(const auto& value:array) {if(!first)out<<',';first=false;
        if constexpr(std::is_arithmetic_v<std::decay_t<decltype(value)>>)out<<int(value);else json_array(out,value);}
    out<<']';
}
void emit_cases(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);std::ofstream manifest(directory/"cases.json");
    manifest<<"{\"schema\":1,\"model\":\"fixed8_candidate_v1\",\"cases\":[";bool first=true;
    for(const auto& c:shader_cases()) {
        const auto shader=generate_tev_shader(c.pipeline,model);require(static_cast<bool>(shader),"fixture shader");
        const auto result=eval(c.pipeline,c.uniforms,c.inputs);
        std::ofstream source(directory/(c.name+".frag"));source<<shader.fragment;require(static_cast<bool>(source),"write shader fixture");
        if(!first)manifest<<',';first=false;
        manifest<<"{\"name\":\""<<c.name<<"\",\"shader\":\""<<c.name<<".frag\",\"registers\":";json_array(manifest,c.uniforms.registers);
        manifest<<",\"konst\":";json_array(manifest,c.uniforms.konst);manifest<<",\"alpha_reference\":";json_array(manifest,c.uniforms.alpha_reference);
        manifest<<",\"texture\":";json_array(manifest,c.inputs.texture);manifest<<",\"raster\":";json_array(manifest,c.inputs.raster);
        manifest<<",\"expected\":";json_array(manifest,result.color);manifest<<",\"alpha_pass\":"<<(result.alpha_pass?"true":"false")<<'}';
    }
    manifest<<"]}\n";require(static_cast<bool>(manifest),"write fixture manifest");
}
// A stage reading its texture with the lookup disabled reads a constant: 255 with texture
// coordinates enabled in BP genMode, 0 without (draw.md).
void disabled_lookup_constant() {
    Memory memory; Sink sink; Decoder d(memory, sink);
    bp(d, 0x00, 1); // One texgen, one stage.
    bp(d, 0x28, 3u | (7u << 7)); // Map 3, coordinate 0, lookup disabled, no raster.
    bp(d, 0xc0, 0x80000 | (15u << 12) | (15u << 8) | (15u << 4) | 8u); // D = TEXC.
    bp(d, 0xc1, 0x80000 | (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (2u << 2)); // D = TEXA, swap table 2.
    bp(d, 0xfa, 1); bp(d, 0xfb, 2); // Table 2 reorders components; a constant is unaffected.
    auto white = capture_tev(d.state());
    require(bool(white), white.status.message.c_str());
    const auto& p = white.material->pipeline;
    require(p.disabled_texture == 255, "texgens enabled: the disabled lookup reads 255");
    const auto use = tev_input_use(p.stages[0]);
    require(!use.texture && use.constant_texture && !tev_stage_samples(p, 0), "a disabled lookup samples nothing");
    TevInputs in; in.texture[0] = {{1, 2, 3, 4}};
    require(eval(p, {}, in).color == TevColor{{255, 255, 255, 255}}, "reference reads 255 without a sample");
    in.texture_valid = 1;
    require(eval(p, {}, in).color == TevColor{{255, 255, 255, 255}}, "a supplied sample is ignored");
    const auto white_shader = generate_tev_shader(p, model);
    require(bool(white_shader), white_shader.status.message.c_str());
    require(white_shader.functions.find("ivec4 tex = ivec4(255);") != std::string::npos &&
            white_shader.functions.find("textures[0]") == std::string::npos, "generated TEV reads the constant, not textures[0]");

    bp(d, 0x00, 0); // No texgens.
    auto black = capture_tev(d.state());
    require(bool(black) && black.material->pipeline.disabled_texture == 0, "no texgens: the disabled lookup reads 0");
    require(eval(black.material->pipeline, {}, in).color == TevColor{{0, 0, 0, 0}}, "reference reads 0");
    const auto black_shader = generate_tev_shader(black.material->pipeline, model);
    require(bool(black_shader) && black_shader.functions.find("ivec4 tex = ivec4(0);") != std::string::npos &&
            black_shader.functions.find("textures[0]") == std::string::npos, "generated TEV reads zero");
    require(black_shader.key != white_shader.key, "the constant is pipeline identity");

    // Stages not reading their disabled lookup keep their keys and text.
    auto unused = blank(), unused_black = blank(); unused_black.disabled_texture = 0;
    const auto a = generate_tev_shader(unused, model), b = generate_tev_shader(unused_black, model);
    require(a && b && a.key == b.key && a.fragment == b.fragment, "an unread disabled lookup is no identity");
    unused.disabled_texture = 7;
    require(!generate_tev_shader(unused, model), "only 0 and 255 are disabled-lookup values");
}
void generated_shader_contract() {
    for(const auto& c:shader_cases()) {
        const auto a=generate_tev_shader(c.pipeline,model),b=generate_tev_shader(c.pipeline,model);
        require(a && b && a.key==b.key && a.fragment==b.fragment,"deterministic specialization");
        require(a.key.size()<=640 && a.fragment.size()<64000,"bounded specialization output");
        require(a.fragment.find("#version 310 es\n")==0 && a.fragment.find("precision highp int;")!=std::string::npos,"GLES3.1 integer precision contract");
        require(a.functions.find("fixed8_candidate_v1")!=std::string::npos && a.fragment.find("discard;")!=std::string::npos,"explicit arithmetic model and alpha rejection");
    }
}
}
int main(int argc,char** argv) {
    try {
        decode_capture_and_keys();z_texture_stages();konst_selector_decode();every_selector_and_swaps();arithmetic_candidate();comparisons_and_stage_order();alpha_tests_and_errors();disabled_lookup_constant();generated_shader_contract();
        if(argc==3 && std::string(argv[1])=="--emit-shaders")emit_cases(argv[2]);
        else require(argc==1,"usage: starlet_gx_tev_tests [--emit-shaders directory]");
        std::cout<<"GX TEV tests passed (fixed8_candidate_v1; original-GX precision unverified)\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
