#include "starlet/gx/sampling.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace starlet::gx {
float evaluate_texture_lod(std::array<float,2> dx, std::array<float,2> dy, const TextureSampler& sampler) {
    if (sampler.anisotropy != 1 || sampler.bias_clamp ||
        sampler.min_lod_sixteenths > sampler.max_lod_sixteenths)
        throw std::invalid_argument("unsupported candidate texture LOD controls");
    for (unsigned i=0;i<2;++i) if (!std::isfinite(dx[i]) || !std::isfinite(dy[i]))
        throw std::invalid_argument("non-finite texture derivatives");
    const float footprint = sampler.edge_lod ? std::max(std::hypot(dx[0],dx[1]),std::hypot(dy[0],dy[1])) :
        std::max(std::hypot(dx[0]+dy[0],dx[1]+dy[1]),std::hypot(dx[0]-dy[0],dx[1]-dy[1]));
    const float value=std::log2(std::max(footprint,1e-20f))+sampler.lod_bias_thirtyseconds/32.0f;
    return std::clamp(value,sampler.min_lod_sixteenths/16.0f,sampler.max_lod_sixteenths/16.0f);
}
const char* texture_sampling_glsl() {
    return R"(
uniform vec4 uGxSamplerLod[8];
float gxTextureLod(vec2 dx, vec2 dy, vec4 controls) {
    float footprint = controls.w != 0.0 ? max(length(dx),length(dy)) :
        max(length(dx+dy),length(dx-dy));
    return clamp(log2(max(footprint,1e-20))+controls.x,controls.y,controls.z);
}
vec4 gxSampleMip(highp sampler2D image, vec2 uv, vec4 controls) {
    vec2 texels=uv*vec2(textureSize(image,0));
    return textureLod(image,uv,gxTextureLod(dFdx(texels),dFdy(texels),controls));
}
)";
}
} // namespace starlet::gx
