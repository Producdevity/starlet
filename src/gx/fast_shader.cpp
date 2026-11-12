#include "starlet/gx/fast_shader.hpp"

#include <array>
#include <sstream>

namespace starlet::gx {
namespace {
template<class E> unsigned code(E value) { return static_cast<unsigned>(value); }
bool comparison(TevOperation op) { return code(op) >= 8; }
unsigned compare_width(TevOperation op) { return (code(op) >> 1) & 3; }
bool alpha_uses_color(const TevStage& stage) { return comparison(stage.alpha.operation) && compare_width(stage.alpha.operation) < 3; }
std::string swizzle(const std::array<std::uint8_t, 4>& swap) {
    std::string out;
    for (auto component : swap) out += "rgba"[component];
    return out;
}
std::string number(float value) {
    std::ostringstream out; out.precision(9); out << value;
    auto s = out.str();
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}
// GX fractional constants: 1, 7/8 ... 1/8 in 8-bit codes.
float fraction(unsigned index) { return float(index ? 256 - index * 32 : 255) / 255.0f; }
std::string konst(const TevKonstSelection& k, bool alpha) {
    if (k.kind == TevKonstKind::fraction) return alpha ? number(fraction(k.index)) : "vec3(" + number(fraction(k.index)) + ")";
    const auto reg = "uGxTevKonst[" + std::to_string(k.index) + "]";
    if (k.kind == TevKonstKind::rgb) return reg + ".rgb";
    const auto scalar = reg + "." + "rgba"[k.component];
    return alpha ? scalar : "vec3(" + scalar + ")";
}
std::string color_input(TevColorInput input) {
    const unsigned value = code(input);
    if (value < 8) return "r" + std::to_string(value / 2) + ((value & 1) ? ".aaa" : ".rgb");
    switch (input) {
    case TevColorInput::texture_rgb: return "tex.rgb";
    case TevColorInput::texture_alpha: return "tex.aaa";
    case TevColorInput::raster_rgb: return "ras.rgb";
    case TevColorInput::raster_alpha: return "ras.aaa";
    case TevColorInput::one: return "vec3(1.0)";
    case TevColorInput::half: return "vec3(" + number(128.0f / 255.0f) + ")";
    case TevColorInput::konst: return "kc";
    default: return "vec3(0.0)";
    }
}
std::string alpha_input(TevAlphaInput input) {
    if (code(input) < 4) return "r" + std::to_string(code(input)) + ".a";
    switch (input) {
    case TevAlphaInput::texture: return "tex.a";
    case TevAlphaInput::raster: return "ras.a";
    case TevAlphaInput::konst: return "ka";
    default: return "0.0";
    }
}
// a, b and c are 8-bit operands; d keeps the signed 11-bit register range.
std::string linear(const std::string& a, const std::string& b, const std::string& c, const std::string& d,
                   TevOperation op, TevBias bias, TevScale scale) {
    std::string mix = "mix(" + a + "," + b + "," + c + ")";
    std::string sum = d + (op == TevOperation::subtract ? "-" : "+") + mix;
    if (bias == TevBias::add_half) sum += "+" + number(128.0f / 255.0f);
    else if (bias == TevBias::subtract_half) sum += "-" + number(128.0f / 255.0f);
    const char* factors[] = {"", "*2.0", "*4.0", "*0.5"};
    return "(" + sum + ")" + factors[code(scale)];
}
std::string quantized(const std::string& v) { return "ivec3(" + v + "*255.0+0.5)"; }
std::string packed(const std::string& q, unsigned width) {
    std::string out = q + ".r";
    if (width >= 1) out += "+" + q + ".g*256";
    if (width >= 2) out += "+" + q + ".b*65536";
    return "(" + out + ")";
}
std::string alpha_test(TevCompare op, unsigned index) {
    if (op == TevCompare::always) return "true";
    if (op == TevCompare::never) return "false";
    const char* operators[] = {"", "<", "==", "<=", ">", "!=", ">=", ""};
    return "(alpha " + std::string(operators[code(op)]) + " uGxTevAlphaRef[" + std::to_string(index) + "])";
}
}

FastFogGlsl fast_fog_glsl(std::uint8_t fog, const std::string& z) {
    const unsigned fog_type = fog & 7;
    std::ostringstream f;
    // Depths behind the B plane are fully fogged, as the hardware's divide saturates.
    f << "    highp float fogDepth=" << (fog & 8 ? "uGxFog.x*" + z
                                           : "uGxFog.x/max(uGxFog.y-" + z + "*uGxFog.z,1e-9)") << ";\n";
    // Range adjustment: sample i is the factor (i+1)*32 pixels from the center (GXInitFogAdjTable);
    // the hardware interpolates two samples, and the factor is 1 at the center itself.
    if (fog & 16)
        f << "    highp float fogStep=min(abs(gl_FragCoord.x-uGxFogCenter)/32.0,10.0);\n    int fogIndex=int(fogStep);\n"
             "    highp float fogBelow=fogIndex==0?1.0:uGxFogTable[min(fogIndex-1,9)];\n"
             "    fogDepth*=mix(fogBelow,uGxFogTable[min(fogIndex,9)],fogStep-float(fogIndex));\n";
    f << "    highp float fogX=clamp(fogDepth-uGxFog.w,0.0,1.0);\n";
    const char* factor = fog_type == 2 ? "fogX" : fog_type == 4 ? "1.0-exp2(-8.0*fogX)" : fog_type == 5 ? "1.0-exp2(-8.0*fogX*fogX)"
                         : fog_type == 6 ? "exp2(-8.0*(1.0-fogX))" : "exp2(-8.0*(1.0-fogX)*(1.0-fogX))";
    return {f.str(), factor};
}

bool fast_alpha_test(const TevPipeline& tev) {
    return !(tev.alpha_compare[0] == TevCompare::always && tev.alpha_compare[1] == TevCompare::always &&
             tev.alpha_logic == TevAlphaLogic::and_);
}

std::string fast_fragment_source(const TevPipeline& tev, const IndirectSnapshot& indirect, std::uint8_t maps,
                                 bool early_depth, bool constant_alpha, bool batched, std::uint8_t affine,
                                 std::uint8_t fog, std::uint8_t wide_registers, const ZTextureProgram& z_texture,
                                 bool zero_alpha, bool vertex_fog) {
    std::ostringstream f;
    f << "#version 310 es\n";
    if (early_depth) f << "layout(early_fragment_tests) in;\n";
    f << "precision mediump float;\nprecision highp int;\n"
      << "in mediump vec4 gx_color0;\nin mediump vec4 gx_color1;\n";
    std::uint8_t coordinates = 0;
    for (unsigned i = 0; i < tev.stage_count; ++i)
        if (tev_stage_samples(tev, i)) coordinates |= 1u << tev.stages[i].texture_coord;
    for (unsigned i = 0; i < 4; ++i) if (indirect.lookup_mask & (1u << i)) coordinates |= 1u << indirect.lookups[i].coordinate;
    for (unsigned i = 0; i < 8; ++i)
        if (coordinates & (1u << i)) f << "in highp " << ((affine >> i) & 1 ? "vec2" : "vec3") << " gx_tex" << i << ";\n";
    // The coordinate in texel-independent units: divided by q unless it is affine.
    const auto uv_of = [&](unsigned c) {
        const auto t = "gx_tex" + std::to_string(c);
        return (affine >> c) & 1 ? t : "(" + t + ".xy/" + t + ".z)";
    };
    for (unsigned map = 0; map < 8; ++map) if (maps & (1u << map)) f << "uniform mediump sampler2D uGxTexture" << map << ";\n";
    if (batched) for (unsigned i = 0; i < 4; ++i) f << "flat in mediump vec4 gx_register" << i << ";\n";
    else f << "uniform vec4 uGxTevRegisters[4];\n";
    f << "uniform vec4 uGxTevKonst[4];\nuniform highp ivec2 uGxTevAlphaRef;\n"
         "uniform highp vec2 uGxCoordinateScale[16];\nuniform float uGxSamplerBias[8];\n";
    // x: the constant, y: keep TEV alpha, z: blend source factor applied here (1: a, 2: 1-a).
    if (constant_alpha) f << "uniform vec3 uGxConstantAlpha;\n";
    const unsigned fog_type = fog & 7;
    // The Z texture's Z feeds the depth test and fog. Its texel is the last stage's raw lookup.
    const bool z_code = z_texture.op && (z_texture.depth || fog_type);
    const auto z_glsl = z_code ? z_texture_glsl(z_texture, "gxZLookup") : ZTextureGlsl{};
    f << z_glsl.globals;
    if (fog_type && vertex_fog) f << "flat in highp float gx_fog;\nuniform mediump vec3 uGxFogColor;\n";
    else if (fog_type) f << "uniform highp vec4 uGxFog;\nuniform mediump vec3 uGxFogColor;\n";
    if (fog_type && (fog & 16) && !vertex_fog) f << "uniform highp float uGxFogCenter;\nuniform highp float uGxFogTable[10];\n";
    if (indirect_coordinates(indirect)) f << "uniform highp vec2 uGxImageSize[16];\n";
    if (indirect.lookup_mask) f << "uniform highp vec4 uGxIndirectScale[4];\n"
                                   "uniform highp ivec4 uGxIndirectRows[6];\n" << indirect_offset_glsl();
    // GX reads a register's low 8 bits as an a, b or c operand and as the final output, so
    // k/255 becomes (k&255)/255 (Super Mario Galaxy cuts Mario's eye holes with wrapped alphas). Exact on
    // [0,1]; the offset absorbs mediump rounding of k/255.
    f << "layout(location=0) out vec4 gxFragmentColor;\n"
         "vec4 gxLow8(vec4 v) { return v-floor(v*(255.0/256.0)+(0.5/256.0))*(256.0/255.0); }\nvoid main() {\n";
    f << z_glsl.reference;
    if (z_code) f << "    highp ivec4 gxZLookup;\n";
    auto sample = [&](unsigned map, const std::string& uv) {
        return "texture(uGxTexture" + std::to_string(map) + "," + uv + ",uGxSamplerBias[" + std::to_string(map) + "])";
    };
    for (unsigned i = 0; i < 4; ++i) if (indirect.lookup_mask & (1u << i)) {
        const auto& lookup = indirect.lookups[i];
        const auto c = std::to_string(unsigned(lookup.coordinate));
        f << "    ivec2 indirectUV" << i << "=ivec2(" << uv_of(lookup.coordinate) << "*uGxIndirectScale[" << i << "].xy)>>ivec2("
          << unsigned(lookup.shift_s) << "," << unsigned(lookup.shift_t) << ");\n"
          << "    ivec4 indirectSample" << i << "=ivec4(" << sample(lookup.map, "vec2(indirectUV" + std::to_string(i) + ")*uGxIndirectScale[" +
             std::to_string(i) + "].zw") << "*255.0+0.5);\n";
    }
    for (unsigned i = 0; i < 4; ++i)
        f << "    vec4 r" << i << "=" << (batched ? "gx_register" + std::to_string(i) : "uGxTevRegisters[" + std::to_string(i) + "]") << ";\n";
    f << "    vec4 l0,l1,l2,l3;\n";
    // Whether lN holds the low 8 bits of rN's current value.
    std::array<bool, 4> low_current{};
    // Registers loaded within 0..255 or written by a clamping stage hold [0,1], as textures,
    // rasterized colours and constants always do; only other operands need their low 8 bits.
    std::array<bool, 4> unit_color{}, unit_alpha{};
    for (unsigned i = 0; i < 4; ++i) {
        unit_color[i] = !(wide_registers & (1u << (2 * i)));
        unit_alpha[i] = !(wide_registers & (1u << (2 * i + 1)));
    }
    for (unsigned i = 0; i < tev.stage_count; ++i) {
        const auto& stage = tev.stages[i];
        f << "    { // stage " << i << "\n";
        const bool z_lookup = z_code && i + 1u == tev.stage_count;
        const auto use = tev_input_use(stage);
        if (use.texture || z_lookup) {
            const auto c = std::to_string(unsigned(stage.texture_coord));
            const auto& s = indirect.stages[i];
            std::string uv;
            if (indirect_coordinate(s)) {
                f << "        ivec2 base=ivec2(" << uv_of(stage.texture_coord) << "*uGxCoordinateScale[" << i << "]*uGxImageSize[" << i << "]*128.0);\n";
                if (s.matrix)
                    f << "        ivec2 offset=gxIndirectOffset(indirectSample" << unsigned(s.lookup) << ",uGxIndirectRows[" << (s.matrix - 1) * 2
                      << "],uGxIndirectRows[" << (s.matrix - 1) * 2 + 1 << "],ivec3(" << ((s.bias & 1) ? 128 : 0) << ","
                      << ((s.bias & 2) ? 128 : 0) << "," << ((s.bias & 4) ? 128 : 0) << "));\n";
                else f << "        ivec2 offset=ivec2(0);\n";
                f << "        ivec2 fixedUV=(" << indirect_wrap_glsl(s, "base") << "+offset)&ivec2(0xffffff);\n"
                  << "        fixedUV=(fixedUV&ivec2(0x7fffff))-(fixedUV&ivec2(0x800000));\n";
                uv = "vec2(fixedUV)/(128.0*uGxImageSize[" + std::to_string(i) + "])";
            } else uv = uv_of(stage.texture_coord) + "*uGxCoordinateScale[" + std::to_string(i) + "]";
            if (z_lookup) f << "        gxZLookup=ivec4(" << sample(stage.texture_map, uv) << "*255.0+0.5);\n        vec4 tex=vec4(0.0);\n";
            else f << "        vec4 tex=" << sample(stage.texture_map, uv) << "." << swizzle(stage.texture_swap) << ";\n";
        } else f << "        vec4 tex=vec4(" << (use.constant_texture && tev.disabled_texture ? "1.0" : "0.0") << ");\n";
        f << "        vec4 ras=" << (stage.raster == TevRaster::zero ? std::string("vec4(0.0)")
                                     : "gx_color" + std::to_string(code(stage.raster)) + "." + swizzle(stage.raster_swap)) << ";\n"
          << "        vec3 kc=" << konst(stage.color_konst, false) << ";\n"
          << "        float ka=" << konst(stage.alpha_konst, true) << ";\n";
        auto low = [&](unsigned reg) {
            if (!low_current[reg]) f << "        l" << reg << "=gxLow8(r" << reg << ");\n";
            low_current[reg] = true;
            return "l" + std::to_string(reg);
        };
        for (unsigned input = 0; input < 4; ++input) {
            const char n = "abcd"[input];
            auto ce = color_input(stage.color.inputs[input]);
            auto ae = alpha_input(stage.alpha.inputs[input]);
            const unsigned cv = code(stage.color.inputs[input]), av = code(stage.alpha.inputs[input]);
            const bool color_unit = cv >= 8 || ((cv & 1) ? unit_alpha[cv / 2] : unit_color[cv / 2]);
            const bool alpha_unit = av >= 4 || unit_alpha[av];
            if (input < 3 && !color_unit) ce = low(cv / 2) + ((cv & 1) ? ".aaa" : ".rgb");
            if (input < 3 && !alpha_unit) ae = low(av) + ".a";
            f << "        vec3 c" << n << "=" << ce << ";\n        float a" << n << "=" << ae << ";\n";
        }
        const auto& ce = stage.color;
        const std::string cmin = ce.clamp ? "0.0" : number(-1024.0f / 255.0f), cmax = ce.clamp ? "1.0" : number(1023.0f / 255.0f);
        if (comparison(ce.operation)) {
            const unsigned width = compare_width(ce.operation);
            const bool eq = code(ce.operation) & 1;
            f << "        ivec3 qa=" << quantized("ca") << ",qb=" << quantized("cb") << ";\n";
            if (width == 3) f << "        vec3 nextColor=cd+mix(vec3(0.0),cc,vec3(" << (eq ? "equal" : "greaterThan") << "(qa,qb)));\n";
            else f << "        vec3 nextColor=cd+(" << packed("qa", width) << (eq ? "==" : ">") << packed("qb", width) << "?cc:vec3(0.0));\n";
            f << "        nextColor=clamp(nextColor," << cmin << "," << cmax << ");\n";
        } else
            f << "        vec3 nextColor=clamp(" << linear("ca", "cb", "cc", "cd", ce.operation, ce.bias, ce.scale) << "," << cmin << "," << cmax << ");\n";
        const auto& ae = stage.alpha;
        const std::string amin = ae.clamp ? "0.0" : number(-1024.0f / 255.0f), amax = ae.clamp ? "1.0" : number(1023.0f / 255.0f);
        if (comparison(ae.operation)) {
            const bool eq = code(ae.operation) & 1;
            std::string condition;
            if (alpha_uses_color(stage)) {
                const unsigned width = compare_width(ae.operation);
                f << "        ivec3 pa=" << quantized("ca") << ",pb=" << quantized("cb") << ";\n";
                condition = packed("pa", width) + (eq ? "==" : ">") + packed("pb", width);
            } else condition = std::string("int(aa*255.0+0.5)") + (eq ? "==" : ">") + "int(ab*255.0+0.5)";
            f << "        float nextAlpha=clamp(ad+(" << condition << "?ac:0.0)," << amin << "," << amax << ");\n";
        } else
            f << "        float nextAlpha=clamp(" << linear("aa", "ab", "ac", "ad", ae.operation, ae.bias, ae.scale) << "," << amin << "," << amax << ");\n";
        f << "        r" << code(ce.destination) << ".rgb=nextColor;\n        r" << code(ae.destination) << ".a=nextAlpha;\n    }\n";
        unit_color[code(ce.destination)] = ce.clamp;
        unit_alpha[code(ae.destination)] = ae.clamp;
        low_current[code(ce.destination)] = low_current[code(ae.destination)] = false;
    }
    const char* logic[] = {" && ", " || ", " != ", " == "};
    const bool test = fast_alpha_test(tev);
    // The last stage's results leave the TEV, whichever registers it wrote.
    const auto& last = tev.stages[tev.stage_count - 1];
    const unsigned rgb = code(last.color.destination), alpha = code(last.alpha.destination);
    const auto output = [&](unsigned reg, bool unit) { return unit ? "r" + std::to_string(reg) : "gxLow8(r" + std::to_string(reg) + ")"; };
    if (rgb == alpha) f << "    vec4 color=" << output(rgb, unit_color[rgb] && unit_alpha[rgb]) << ";\n";
    else f << "    vec4 color=vec4(" << output(rgb, unit_color[rgb]) << ".rgb," << output(alpha, unit_alpha[alpha]) << ".a);\n";
    if (test)
        f << "    int alpha=int(color.a*255.0+0.5);\n    if (!(" << alpha_test(tev.alpha_compare[0], 0) << logic[code(tev.alpha_logic)]
          << alpha_test(tev.alpha_compare[1], 1) << ")) " << (zero_alpha ? "color.a=0.0" : "discard") << ";\n";
    if (constant_alpha) f << "    float blendAlpha=color.a;\n    color.a=uGxConstantAlpha.y>0.5?blendAlpha:uGxConstantAlpha.x;\n";
    f << z_glsl.depth;
    if (fog_type && vertex_fog) f << "    color.rgb=mix(color.rgb,uGxFogColor,gx_fog);\n";
    else if (fog_type) {
        const std::string z = z_code ? "gxDepth" : "gl_FragCoord.z";
        if (z_code) f << "    highp float gxDepth=float(gxZ)/16777216.0;\n";
        const auto fog_glsl = fast_fog_glsl(fog, z);
        f << fog_glsl.statements << "    color.rgb=mix(color.rgb,uGxFogColor," << fog_glsl.factor << ");\n";
    }
    if (constant_alpha) f << "    color.rgb*=uGxConstantAlpha.z>1.5?1.0-blendAlpha:(uGxConstantAlpha.z>0.5?blendAlpha:1.0);\n";
    f << "    gxFragmentColor=color;\n}\n";
    return f.str();
}

} // namespace starlet::gx
