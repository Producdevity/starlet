#include "starlet/gx/depth.hpp"

namespace starlet::gx {
namespace {
bool compare(unsigned function, std::uint32_t incoming, std::uint32_t stored) {
    switch (function) {
    case 0: return false;
    case 1: return incoming < stored;
    case 2: return incoming == stored;
    case 3: return incoming <= stored;
    case 4: return incoming > stored;
    case 5: return incoming != stored;
    case 6: return incoming >= stored;
    case 7: return true;
    }
    return false;
}
}
bool decode_z_texture(std::uint32_t bp_f4, std::uint32_t bp_f5, ZTexture& out) {
    out = {};
    const auto op = static_cast<std::uint8_t>((bp_f5 >> 2) & 3), type = static_cast<std::uint8_t>(bp_f5 & 3);
    if (!op) return true;
    if (op == 3 || type == 3 || (bp_f5 & 0xfffff0u)) return false;
    out = {op, type, bp_f4 & depth_code_max};
    return true;
}
std::uint32_t z_texture_depth(const ZTexture& texture, const std::array<std::uint8_t, 4>& texel, std::uint32_t reference) {
    const std::uint32_t r = texel[0], g = texel[1], b = texel[2], a = texel[3];
    const std::uint32_t z = texture.type == 0 ? a : texture.type == 1 ? (a << 8 | r) : (r << 16 | g << 8 | b);
    return (z + texture.bias + (texture.op == 1 ? reference : 0)) & depth_code_max;
}
ZTextureGlsl z_texture_glsl(const ZTextureProgram& program, const std::string& texel) {
    ZTextureGlsl glsl;
    if (!program.op) return glsl;
    glsl.globals = "uniform highp uint uGxZBias;\n";
    // Z texels offset the reference Z at the centre of the 2x2 quad, without the per-pixel slopes
    // (GX manual 8.10). EFB quads are window quads, as target heights are even.
    if (program.op == 1)
        glsl.reference =
            "    highp float gxZQuad=gl_FragCoord.z+dFdx(gl_FragCoord.z)*(floor(gl_FragCoord.x*0.5)*2.0+1.0-gl_FragCoord.x)"
            "+dFdy(gl_FragCoord.z)*(floor(gl_FragCoord.y*0.5)*2.0+1.0-gl_FragCoord.y);\n"
            "    highp uint gxZReference=min(uint(clamp(gxZQuad,0.0,1.0)*16777216.0+0.5),16777215u);\n";
    const char* z = program.type == 0 ? "gxZTexel.a" : program.type == 1 ? "(gxZTexel.a<<8u|gxZTexel.r)"
                                                                       : "(gxZTexel.r<<16u|gxZTexel.g<<8u|gxZTexel.b)";
    glsl.depth = "    highp uvec4 gxZTexel=uvec4(" + texel + ");\n    highp uint gxZ=(" + z + "+uGxZBias" +
                 (program.op == 1 ? "+gxZReference" : "") + ")&16777215u;\n";
    if (program.depth) glsl.depth += "    gl_FragDepth=float(gxZ)/16777216.0;\n";
    return glsl;
}
DepthCapture capture_depth(const State& state) {
    const auto z = state.bp(0x40), control = state.bp(0x43);
    if ((z & ~0x1fu) || (control & ~0x7fu))
        return {{DepthError::unsupported_state, "reserved depth state bits"}, {}};
    if ((control & 7) > 1 || (control & 0x38) || (state.bp(0) & ((1u << 9) | (1u << 19))))
        return {{DepthError::unsupported_state, "depth requires non-AA linear Z24 without Z freeze"}, {}};
    ZTexture texture;
    if (!decode_z_texture(state.bp(0xf4), state.bp(0xf5), texture))
        return {{DepthError::unsupported_state, "reserved Z-texture state"}, {}};
    return {{}, std::make_shared<const DepthSnapshot>(DepthSnapshot{z, control, state.revision(), texture})};
}
DepthResult evaluate_depth(const DepthSnapshot& state, std::uint32_t incoming,
                           std::uint32_t stored, bool alpha_pass) {
    if (incoming > depth_code_max || stored > depth_code_max)
        return {{DepthError::invalid_input, "depth input is not a canonical Z24 code"}, {}};
    const bool enabled = state.z_mode & 1;
    const bool z_pass = !enabled || compare((state.z_mode >> 1) & 7, incoming, stored);
    const bool early = (state.pixel_control & 64) && !state.z_texture.active();
    const bool write = enabled && (state.z_mode & 16) && z_pass && (early || alpha_pass);
    return {{}, {z_pass && alpha_pass, write, write ? incoming : stored}};
}
std::string_view depth_glsl() {
    return R"GLSL(
bool gx_depth_compare(uint function, uint incoming, uint stored) {
    if (function==0u) return false;
    if (function==1u) return incoming<stored;
    if (function==2u) return incoming==stored;
    if (function==3u) return incoming<=stored;
    if (function==4u) return incoming>stored;
    if (function==5u) return incoming!=stored;
    if (function==6u) return incoming>=stored;
    return true;
}
uvec3 gx_depth(uint incoming, uint stored, bool alpha, uvec2 controls) {
    bool enabled=(controls.x&1u)!=0u;
    bool z_pass=!enabled || gx_depth_compare((controls.x>>1u)&7u,incoming,stored);
    bool write=enabled && (controls.x&16u)!=0u && z_pass && ((controls.y&64u)!=0u || alpha);
    return uvec3(z_pass && alpha ? 1u : 0u,write ? 1u : 0u,write ? incoming : stored);
}
)GLSL";
}
} // namespace starlet::gx
