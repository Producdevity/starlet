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
DepthCapture capture_depth(const State& state) {
    const auto z = state.bp(0x40), control = state.bp(0x43);
    if ((z & ~0x1fu) || (control & ~0x7fu))
        return {{DepthError::unsupported_state, "reserved depth state bits"}, {}};
    if ((control & 7) > 1 || (control & 0x38) || (state.bp(0) & ((1u << 9) | (1u << 19))))
        return {{DepthError::unsupported_state, "depth requires non-AA linear Z24 without Z freeze"}, {}};
    if (state.bp(0xf5) & 0x0c)
        return {{DepthError::unsupported_state, "Z texture depth generation is not implemented"}, {}};
    return {{}, std::make_shared<const DepthSnapshot>(DepthSnapshot{z, control, state.revision()})};
}
DepthResult evaluate_depth(const DepthSnapshot& state, std::uint32_t incoming,
                           std::uint32_t stored, bool alpha_pass) {
    if (incoming > depth_code_max || stored > depth_code_max)
        return {{DepthError::invalid_input, "depth input is not a canonical Z24 code"}, {}};
    const bool enabled = state.z_mode & 1;
    const bool z_pass = !enabled || compare((state.z_mode >> 1) & 7, incoming, stored);
    const bool write = enabled && (state.z_mode & 16) && z_pass &&
                       ((state.pixel_control & 64) || alpha_pass);
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
