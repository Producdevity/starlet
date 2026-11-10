#include "starlet/gx/transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>

namespace starlet::gx {
namespace {
using V3 = std::array<float, 3>;
TransformStatus fail(TransformError error, const char* message) { return {error, message}; }
bool present(const Attribute& a) { return a.mode != AttributeMode::none; }
float as_float(std::uint32_t value) { float f; std::memcpy(&f, &value, sizeof f); return f; }
TransformVector color(std::uint32_t w) {
    return {float((w >> 24) & 255), float((w >> 16) & 255), float((w >> 8) & 255), float(w & 255)};
}
float dot(const V3& a, const V3& b) { return (a[0]*b[0] + a[1]*b[1]) + a[2]*b[2]; }
float dot(const TransformVector& a, const TransformVector& b) {
    return ((a[0]*b[0] + a[1]*b[1]) + a[2]*b[2]) + a[3]*b[3];
}
V3 xyz(const TransformVector& a) { return {a[0], a[1], a[2]}; }
template<class T> bool finite(const T& a) {
    return std::all_of(a.begin(), a.end(), [](float x) { return std::isfinite(x); });
}
bool normalize(V3& v) {
    const float d = dot(v, v);
    if (!std::isfinite(d) || d <= 1e-30f) return false;
    const float length = std::sqrt(d);
    for (auto& c : v) c /= length;
    return true;
}
// GX scales each term by its coefficient, so without x terms the input never matters:
// games leave the direction of a distance-only light uninitialized (actor light 2).
float polynomial(const TransformVector& c, float x) { return !c[1] && !c[2] ? c[0] : (c[2]*x + c[1])*x + c[0]; }
// Whether a light's direction reaches the attenuation: through the angle terms, or for
// specular attenuation also through the distance terms, which take the same angle.
bool direction_matters(const TransformLight& l, TransformAttenuation attenuation) {
    return l.angle[1] || l.angle[2] || (attenuation == TransformAttenuation::specular && (l.distance[1] || l.distance[2]));
}
float coefficient(const TransformUniforms& u, unsigned i) { return u.projection[i/4][i%4]; }
bool needs_normal(const TransformChannel& c) {
    return c.enabled && c.light_mask &&
        (c.diffuse != TransformDiffuse::none || c.attenuation == TransformAttenuation::specular);
}
bool uses_lighting_normal(const TransformPipeline& p) {
    for (unsigned i = 0; i < 4; ++i) {
        if (!(p.computed_colors & (1u << (i%2))) || (i >= 2 && i%2 >= p.color_count)) continue;
        if (needs_normal(p.channels[i])) return true;
    }
    return false;
}
bool uses_emboss(const TransformPipeline& p) {
    for (unsigned i = 0; i < p.texgen_count; ++i) if (p.texgens[i].type == 1) return true;
    return false;
}
TransformStatus validate(const TransformPipeline& p) {
    if (p.color_count > 2 || p.texgen_count > 8 || p.computed_colors > 3 || p.raster_colors > 3 || p.vertex_colors > 3 ||
        (p.normal_components != 0 && p.normal_components != 3 && p.normal_components != 9))
        return fail(TransformError::unsupported_state, "invalid transform pipeline counts");
    unsigned expected = ((1u << p.color_count) - 1u) & p.raster_colors;
    unsigned color_texgens = 0;
    for (unsigned i = 0; i < p.texgen_count; ++i) {
        const auto& t = p.texgens[i];
        if (t.type == 1) {
            if (t.emboss_source >= i || p.texgens[t.emboss_source].type != 0)
                return fail(TransformError::unsupported_state, "emboss texgen needs an earlier regular texgen as its source");
            if (p.normal_components != 9) return fail(TransformError::missing_input, "emboss texgen requires NBT");
            continue;
        }
        if (t.type > 3) return fail(TransformError::unsupported_state, "reserved texgen type");
        if (t.type >= 2) {
            if (color_texgens >= 2 || t.type != 2 + color_texgens++)
                return fail(TransformError::unsupported_state, "SRTG requires color0 then color1, at most two generators");
            if (t.source_row != 2 || t.input_abc1 || t.projected)
                return fail(TransformError::unsupported_state, "non-SDK color texgen encoding");
            expected |= 1u << (t.type - 2);
        } else {
            const unsigned row = t.source_row;
            if (row == 1 && !p.normal_components)
                return fail(TransformError::missing_input, "normal texgen requires a normal attribute");
            if ((row == 3 || row == 4) && p.normal_components != 9)
                return fail(TransformError::missing_input, "binormal/tangent texgen requires NBT");
            if (row >= 5 && row <= 12) {
                // An absent ST attribute reads (0, 0, 1, 1); see draw.md.
                if (t.input_abc1) return fail(TransformError::unsupported_state, "ST texgen cannot supply ABC input");
            } else if (row != 0 && row != 1 && row != 3 && row != 4) {
                return fail(TransformError::unsupported_state, "reserved regular texgen source row");
            } else if (!t.input_abc1) {
                return fail(TransformError::unsupported_state, "vector texgen requires ABC1 input");
            }
            if (p.dual_texture && t.post_row > 61)
                return fail(TransformError::invalid_matrix, "post matrix crosses row 63");
        }
    }
    if (p.computed_colors != expected)
        return fail(TransformError::unsupported_state, "computed color mask does not match channel/texgen use");
    for (unsigned i = 0; i < 4; ++i) {
        if (!(expected & (1u << (i%2))) || (i >= 2 && i%2 >= p.color_count)) continue;
        const auto& c = p.channels[i];
        if (unsigned(c.diffuse) > 2 || unsigned(c.attenuation) > 2)
            return fail(TransformError::unsupported_state, "reserved lighting function");
        if (c.enabled && c.attenuation == TransformAttenuation::specular && c.diffuse != TransformDiffuse::none)
            return fail(TransformError::unsupported_state, "specular channel has non-SDK diffuse mode");
        if (needs_normal(c) && !p.normal_components)
            return fail(TransformError::missing_input, "lighting requires a normal attribute");
    }
    return {};
}
// Games leave unused lights enabled with every attenuation coefficient zero; such a light
// adds nothing. A zero divisor under a positive numerator saturates the light instead of
// producing an infinity that a black light would turn into NaN.
float attenuation_quotient(float numerator, float denominator) {
    if (denominator == 0.0f) return numerator == 0.0f ? 0.0f : std::numeric_limits<float>::max();
    return numerator / denominator;
}
TransformStatus lighting_factor(const TransformChannel& c, const TransformLight& l,
                               const V3& eye, const V3& normal, float& factor) {
    factor = 1;
    if (c.diffuse == TransformDiffuse::none && c.attenuation == TransformAttenuation::none) return {};
    V3 ray = xyz(l.position);
    if (c.attenuation != TransformAttenuation::specular)
        for (unsigned k = 0; k < 3; ++k) ray[k] -= eye[k];
    const float squared = dot(ray, ray);
    if (!normalize(ray)) return fail(TransformError::unsupported_arithmetic, "degenerate light direction");
    const float ndotl = dot(normal, ray);
    if (c.diffuse == TransformDiffuse::signed_) factor = ndotl;
    if (c.diffuse == TransformDiffuse::clamped) factor = std::max(0.0f, ndotl);
    if (c.attenuation != TransformAttenuation::none) {
        float angle, distance;
        if (c.attenuation == TransformAttenuation::spot) {
            angle = std::max(0.0f, dot(ray, xyz(l.direction)));
            distance = std::sqrt(squared);
        } else {
            angle = ndotl > 0 ? std::max(0.0f, dot(normal, xyz(l.direction))) : 0;
            distance = angle;
        }
        const float numerator = std::max(0.0f, polynomial(l.angle, angle));
        const float denominator = polynomial(l.distance, distance);
        if (!finite(l.angle) || !finite(l.distance) || (direction_matters(l, c.attenuation) && !finite(l.direction)) ||
            !std::isfinite(numerator) || !std::isfinite(denominator))
            return fail(TransformError::unsupported_arithmetic, "invalid light attenuation division");
        factor *= attenuation_quotient(numerator, denominator);
    }
    if (!std::isfinite(factor)) return fail(TransformError::unsupported_arithmetic, "non-finite light factor");
    return {};
}
std::string index(unsigned i) { return std::to_string(i); }
// The XF numbers vertex colors in the order present: channel 0 reads color 1 when color 0
// is absent, and channel 1 reads color 1 only beside color 0. A channel without a vertex
// color reads white, as Dolphin models it. Returns the CP color index, or 2 for white.
unsigned vertex_color_source(const TransformPipeline& p, unsigned channel) {
    if (channel == 0) return (p.vertex_colors & 1) ? 0 : (p.vertex_colors & 2) ? 1 : 2;
    return p.vertex_colors == 3 ? 1 : 2;
}
std::string vertex_color_glsl(const TransformPipeline& p, unsigned channel) {
    const auto source = vertex_color_source(p, channel);
    return source < 2 ? "round(a_color" + index(source) + "*255.0)" : "vec4(255.0)";
}
} // namespace

static_assert(sizeof(TransformLight) == 80 && sizeof(TransformUniforms) == 3376, "std140 uniform layout");
static_assert(std::is_standard_layout<TransformUniforms>::value, "uniforms must be standard layout");

namespace {
TransformEvaluation evaluate_prepared(const TransformPipeline& p, const TransformUniforms& u,
                                      const ExpandedVertex& v, bool transform_normal) {
    TransformEvaluation r;
    if (v.disabled || !finite(v.position)) {
        r.status = fail(TransformError::invalid_input, "disabled or non-finite vertex"); return r;
    }
    const unsigned row = v.matrix_indices[0];
    if (row > 61 || (transform_normal && (row%32) > 29)) {
        r.status = fail(TransformError::invalid_matrix, "position/normal matrix crosses memory end"); return r;
    }
    const TransformVector source{v.position[0], v.position[1], v.position[2], 1};
    std::array<float, 9> latched{};
    if (p.latched_normal)
        for (unsigned k = 0; k < 9; ++k) latched[k] = u.latched_normal[k/3][k%3];
    const auto& vn = p.latched_normal ? latched : v.normal;
    auto& o = r.output;
    for (unsigned k = 0; k < 3; ++k) o.eye_position[k] = dot(u.position[row+k], source);
    if (transform_normal) {
        const V3 n{vn[0], vn[1], vn[2]};
        for (unsigned k = 0; k < 3; ++k) o.normal[k] = dot(xyz(u.normal[row%32+k]), n);
        if (!normalize(o.normal)) {
            r.status = fail(TransformError::unsupported_arithmetic, "degenerate transformed normal"); return r;
        }
    }
    const auto& e = o.eye_position;
    o.gx_clip = {e[0]*coefficient(u,0) + (p.orthographic ? coefficient(u,1) : e[2]*coefficient(u,1)),
                 e[1]*coefficient(u,2) + (p.orthographic ? coefficient(u,3) : e[2]*coefficient(u,3)),
                 e[2]*coefficient(u,4) + coefficient(u,5), p.orthographic ? 1.0f : -e[2]};
    o.gles_clip = o.gx_clip;
    o.gles_clip[2] = 2*o.gx_clip[2] + o.gx_clip[3];
    if (!finite(o.eye_position) || !finite(o.gx_clip) || !finite(o.gles_clip)) {
        r.status = fail(TransformError::unsupported_arithmetic, "non-finite transformed position"); return r;
    }
    std::array<std::array<float,8>,9> factors;
    std::array<std::uint8_t,9> evaluated_lights{};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned channel = i%2;
        if (!(p.computed_colors & (1u << channel)) || (i >= 2 && channel >= p.color_count)) continue;
        const auto& c = p.channels[i];
        TransformVector sum{};
        if (c.enabled) for (unsigned light = 0; light < 8; ++light) {
            if (!(c.light_mask & (1u << light))) continue;
            const unsigned function = unsigned(c.diffuse)*3 + unsigned(c.attenuation);
            if (!(evaluated_lights[function] & (1u << light))) {
                r.status = lighting_factor(c, u.lights[light], e, o.normal, factors[function][light]);
                if (!r.status) return r;
                evaluated_lights[function] |= 1u << light;
            }
            const float factor = factors[function][light];
            // A saturated light (see attenuation_quotient) stays finite until the clamp to 255.
            for (unsigned k = 0; k < 4; ++k)
                sum[k] = std::min(sum[k] + factor*u.lights[light].color[k], std::numeric_limits<float>::max());
        }
        for (unsigned k = (i < 2 ? 0 : 3); k < (i < 2 ? 3 : 4); ++k) {
            const auto source = vertex_color_source(p, channel);
            const float vertex = source < 2 ? float(v.colors[source][k]) : 255.0f;
            const float material = c.material_vertex ? vertex : u.material[channel][k];
            const float ambient = c.ambient_vertex ? vertex : u.ambient[channel][k];
            if (!std::isfinite(sum[k]) || !std::isfinite(material) || !std::isfinite(ambient) ||
                material < 0 || material > 255 || ambient < 0 || ambient > 255) {
                r.status = fail(TransformError::unsupported_arithmetic, "invalid lighting color arithmetic"); return r;
            }
            const float illumination = c.enabled ? std::clamp(ambient + std::trunc(sum[k]), 0.0f, 255.0f)/255.0f : 1;
            o.colors[channel][k] = (material/255.0f)*illumination;
        }
    }
    for (unsigned i = 0; i < p.texgen_count; ++i) {
        const auto& t = p.texgens[i];
        auto& out = o.texcoords[i];
        if (t.type >= 2) { out = {o.colors[t.type-2][0], o.colors[t.type-2][1], 1}; continue; }
        if (t.type == 1) {
            // The light direction projected on the row-3 and row-4 NBT vectors in eye space
            // offsets s and t of the source texgen (US 6,980,218); q passes through.
            const unsigned nr = row % 32;
            if (nr > 29) { r.status = fail(TransformError::invalid_matrix, "normal matrix crosses memory end"); return r; }
            V3 ray = xyz(u.lights[t.emboss_light].position);
            for (unsigned k = 0; k < 3; ++k) ray[k] -= e[k];
            if (!normalize(ray)) { r.status = fail(TransformError::unsupported_arithmetic, "degenerate emboss light direction"); return r; }
            const V3 b{vn[3], vn[4], vn[5]}, tangent{vn[6], vn[7], vn[8]};
            V3 s_axis, t_axis;
            for (unsigned k = 0; k < 3; ++k) {
                s_axis[k] = dot(xyz(u.normal[nr+k]), b);
                t_axis[k] = dot(xyz(u.normal[nr+k]), tangent);
            }
            out = o.texcoords[t.emboss_source];
            out[0] += dot(s_axis, ray); out[1] += dot(t_axis, ray);
            if (!finite(out)) { r.status = fail(TransformError::unsupported_arithmetic, "non-finite texture coordinate"); return r; }
            continue;
        }
        TransformVector input{};
        if (t.source_row == 0) input = source;
        else if (t.source_row == 1 || t.source_row == 3 || t.source_row == 4) {
            const unsigned n = t.source_row == 1 ? 0 : (t.source_row == 3 ? 3 : 6);
            input = {vn[n], vn[n+1], vn[n+2], 1};
        } else if (p.vertex_texcoords & (1u << (t.source_row-5))) {
            const auto& st = v.texcoords[t.source_row-5]; input = {st[0], st[1], 1, 1};
        } else {
            input = {0, 0, 1, 1};
        }
        const unsigned texture_row = v.matrix_indices[i+1];
        if (texture_row > (t.projected ? 61u : 62u)) {
            r.status = fail(TransformError::invalid_matrix, "texture matrix crosses row 63"); return r;
        }
        out = {dot(u.position[texture_row], input), dot(u.position[texture_row+1], input),
               t.projected ? dot(u.position[texture_row+2], input) : 1};
        if (p.dual_texture) {
            if (t.normalize && !normalize(out)) {
                r.status = fail(TransformError::unsupported_arithmetic, "degenerate texgen normalization"); return r;
            }
            const TransformVector post_input{out[0], out[1], out[2], 1};
            for (unsigned k = 0; k < 3; ++k) out[k] = dot(u.post[t.post_row+k], post_input);
        }
        if (!finite(input) || !finite(out)) {
            r.status = fail(TransformError::unsupported_arithmetic, "non-finite texture coordinate"); return r;
        }
    }
    return r;
}


} // namespace

TransformEvaluator::TransformEvaluator(const TransformSnapshot& snapshot, TransformArithmetic model)
    : snapshot_(snapshot) {
    if (model != TransformArithmetic::float32_candidate_v1)
        status_ = fail(TransformError::unsupported_arithmetic, "unsupported transform arithmetic model");
    else
        status_ = validate(snapshot.pipeline);
    if (status_) lighting_normal_ = uses_lighting_normal(snapshot.pipeline);
}
TransformEvaluation TransformEvaluator::evaluate(const ExpandedVertex& vertex) const {
    if (!status_) return {status_, {}};
    return evaluate_prepared(snapshot_.pipeline, snapshot_.uniforms, vertex, lighting_normal_);
}
TransformEvaluation evaluate_transform(const TransformPipeline& p, const TransformUniforms& u,
                                       const ExpandedVertex& v, TransformArithmetic model) {
    if (model != TransformArithmetic::float32_candidate_v1)
        return {fail(TransformError::unsupported_arithmetic, "unsupported transform arithmetic model"), {}};
    const auto status = validate(p);
    if (!status) return {status, {}};
    return evaluate_prepared(p, u, v, uses_lighting_normal(p));
}

TransformStatus capture_transform_pipeline(const State& s, const VertexLayout& layout, std::uint8_t raster_colors,
                                           TransformPipeline& p, std::uint8_t latched_components) {
    p = {};
    if (s.xf(0x1005) > 1 || s.xf(0x1009) > 2 || s.xf(0x103f) > 8 || s.xf(0x1026) > 1 || s.xf(0x1012) > 1)
        return fail(TransformError::unsupported_state, "unsupported clipping/projection/channel/texgen state");
    p.color_count = static_cast<std::uint8_t>(s.xf(0x1009));
    p.texgen_count = static_cast<std::uint8_t>(s.xf(0x103f));
    p.raster_colors = raster_colors;
    p.computed_colors = static_cast<std::uint8_t>(((1u << p.color_count)-1u) & raster_colors);
    p.orthographic = s.xf(0x1026) != 0;
    p.dual_texture = s.xf(0x1012) != 0;
    p.clipping_disabled = s.xf(0x1005) != 0;
    p.normal_components = present(layout.normal) ? layout.normal.components : 0;
    for (unsigned i = 0; i < 2; ++i) if (present(layout.colors[i])) p.vertex_colors |= 1u << i;
    for (unsigned i = 0; i < 8; ++i) if (present(layout.texcoords[i])) p.vertex_texcoords |= 1u << i;
    for (unsigned i = 0; i < p.texgen_count; ++i) {
        const auto t = s.texgen(static_cast<std::uint8_t>(i));
        const auto post = s.xf(static_cast<std::uint16_t>(0x1050+i));
        // The texgen register holds 18 bits and the post-transform register 9; the SDK's
        // unmasked field writes can set bits above them, which the hardware never sees.
        if ((s.xf(static_cast<std::uint16_t>(0x1040+i)) & 0x3ffffu & ~0x3fff6u) || (post & 0x1ffu & ~0x13fu))
            return fail(TransformError::unsupported_state, "reserved texgen bits");
        p.texgens[i] = {t.type, t.source_row, t.input_abc1, t.projected,
                       (post & 256u) != 0, static_cast<std::uint8_t>(post & 63u)};
        if (t.type == 1) { p.texgens[i].emboss_source = t.bump_source; p.texgens[i].emboss_light = t.bump_light; }
        if (t.type == 2 || t.type == 3) p.computed_colors |= 1u << (t.type-2);
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!(p.computed_colors & (1u << (i%2))) || (i >= 2 && i%2 >= p.color_count)) continue;
        const auto bits = s.xf(static_cast<std::uint16_t>(0x100e + i));
        if (bits & ~0x7fffu) return fail(TransformError::unsupported_state, "reserved channel bits");
        auto& c = p.channels[i];
        c.material_vertex = bits & 1; c.enabled = bits & 2; c.ambient_vertex = bits & 64;
        c.light_mask = static_cast<std::uint8_t>(((bits >> 2)&15) | ((bits >> 7)&240));
        c.diffuse = static_cast<TransformDiffuse>((bits >> 7)&3);
        c.attenuation = !(bits & 512) ? TransformAttenuation::none :
            (bits & 1024 ? TransformAttenuation::spot : TransformAttenuation::specular);
    }
    if (!p.normal_components && (latched_components == 3 || latched_components == 9)) {
        bool needs = uses_lighting_normal(p) || uses_emboss(p);
        for (unsigned i = 0; i < p.texgen_count; ++i)
            needs |= p.texgens[i].type == 0 && (p.texgens[i].source_row == 1 || p.texgens[i].source_row == 3 || p.texgens[i].source_row == 4);
        if (needs) { p.normal_components = latched_components; p.latched_normal = true; }
    }
    return validate(p);
}

void direct_transform_uniforms(const State& s, TransformUniforms& u) {
    const auto* xf = s.xf_data();
    auto row = [xf](std::uint16_t base, unsigned width, TransformVector& v) {
        std::memcpy(v.data(), xf + base, width * 4);
        std::fill(v.begin() + width, v.end(), 0.0f);
    };
    static_assert(sizeof(TransformUniforms) == 211 * sizeof(TransformVector), "every uniform field is written");
    u.latched_normal = {};   // The XF latch is not State; the direct renderer fills it.
    static_assert(sizeof(u.position) == 256 * 4 && sizeof(u.post) == 256 * 4, "XF matrix memory layout");
    std::memcpy(u.position.data(), xf, sizeof u.position);
    std::memcpy(u.post.data(), xf + 0x500, sizeof u.post);
    for (unsigned i = 0; i < 32; ++i) row(static_cast<std::uint16_t>(0x400+i*3), 3, u.normal[i]);
    for (unsigned i = 0; i < 8; ++i) {
        u.projection[i/4][i%4] = i < 6 ? as_float(s.xf(static_cast<std::uint16_t>(0x1020+i))) : 0.0f;
        u.viewport[i/4][i%4] = i < 6 ? as_float(s.xf(static_cast<std::uint16_t>(0x101a+i))) : 0.0f;
    }
    for (unsigned i = 0; i < 2; ++i) {
        u.ambient[i] = color(s.xf(static_cast<std::uint16_t>(0x100a+i)));
        u.material[i] = color(s.xf(static_cast<std::uint16_t>(0x100c+i)));
    }
    for (unsigned i = 0; i < 8; ++i) {
        auto& l = u.lights[i]; const auto base = static_cast<std::uint16_t>(0x600+i*16);
        l.color = color(s.xf(base+3));
        row(base+4, 3, l.angle); row(base+7, 3, l.distance);
        row(base+10, 3, l.position); row(base+13, 3, l.direction);
    }
}

TransformCapture capture_transform(const State& s, const VertexSnapshot& vertices, std::uint8_t raster_colors) {
    TransformCapture r;
    if (vertices.state_revision() != s.revision()) {
        r.status = fail(TransformError::state_mismatch, "vertex and transform state revisions differ"); return r;
    }
    TransformPipeline p;
    TransformUniforms u;
    r.status = capture_transform_pipeline(s, vertices.layout(), raster_colors, p);
    if (!r.status) return r;
    unsigned lights = 0, light_positions = 0, light_attenuation = 0, light_specular = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto& c = p.channels[i];
        if (!c.enabled) continue;
        lights |= c.light_mask;
        if (c.diffuse != TransformDiffuse::none || c.attenuation != TransformAttenuation::none)
            light_positions |= c.light_mask;
        if (c.attenuation != TransformAttenuation::none) light_attenuation |= c.light_mask;
        if (c.attenuation == TransformAttenuation::specular) light_specular |= c.light_mask;
    }
    for (unsigned i = 0; i < p.texgen_count; ++i)
        if (p.texgens[i].type == 1) { lights |= 1u << p.texgens[i].emboss_light; light_positions |= 1u << p.texgens[i].emboss_light; }
    auto read_row = [&s](std::uint16_t base, unsigned row, unsigned width) {
        TransformVector v{};
        for (unsigned k = 0; k < width; ++k) v[k] = as_float(s.xf(static_cast<std::uint16_t>(base+row*width+k)));
        return v;
    };
    const bool transform_normal = uses_lighting_normal(p);
    std::array<bool, 64> pos{}, post{};
    std::array<bool, 32> normal{};
    for (const auto& v : vertices.vertices()) {
        if (v.disabled) continue;
        if (!finite(v.position) || (transform_normal && !finite(V3{v.normal[0],v.normal[1],v.normal[2]}))) {
            r.status = fail(TransformError::invalid_input, "non-finite live position/normal input"); return r;
        }
        const unsigned row = v.matrix_indices[0];
        if (row > 61 || (transform_normal && row%32 > 29)) {
            r.status = fail(TransformError::invalid_matrix, "position/normal matrix crosses memory end"); return r;
        }
        for (unsigned k = 0; k < 3; ++k) { pos[row+k] = true; if (transform_normal) normal[row%32+k] = true; }
        for (unsigned i = 0; i < p.texgen_count; ++i) {
            const auto& t = p.texgens[i];
            if (t.type == 1) {
                if (!finite(v.normal)) { r.status = fail(TransformError::invalid_input, "non-finite live texgen input"); return r; }
                if (row%32 > 29) { r.status = fail(TransformError::invalid_matrix, "position/normal matrix crosses memory end"); return r; }
                for (unsigned k = 0; k < 3; ++k) normal[row%32+k] = true;
                continue;
            }
            if (t.type != 0) continue;
            bool source_finite = true;
            if (t.source_row == 1 || t.source_row == 3 || t.source_row == 4) {
                const unsigned n = t.source_row == 1 ? 0 : (t.source_row == 3 ? 3 : 6);
                source_finite = finite(V3{v.normal[n],v.normal[n+1],v.normal[n+2]});
            } else if (t.source_row >= 5 && (p.vertex_texcoords & (1u << (t.source_row-5)))) source_finite = finite(v.texcoords[t.source_row-5]);
            if (!source_finite) {
                r.status = fail(TransformError::invalid_input, "non-finite live texgen input"); return r;
            }
            const unsigned tex_row = v.matrix_indices[i+1], count = t.projected ? 3 : 2;
            if (tex_row+count > 64) {
                r.status = fail(TransformError::invalid_matrix, "texture matrix crosses row 63"); return r;
            }
            for (unsigned k = 0; k < count; ++k) pos[tex_row+k] = true;
            if (p.dual_texture) for (unsigned k = 0; k < 3; ++k) post[t.post_row+k] = true;
        }
    }
    for (unsigned i = 0; i < 64; ++i) {
        if (pos[i]) u.position[i] = read_row(0, i, 4);
        if (post[i]) u.post[i] = read_row(0x500, i, 4);
        if (i < 32 && normal[i]) u.normal[i] = read_row(0x400, i, 3);
        if ((pos[i] && !finite(u.position[i])) || (post[i] && !finite(u.post[i])) ||
            (i < 32 && normal[i] && !finite(u.normal[i]))) {
            r.status = fail(TransformError::invalid_input, "non-finite live XF matrix row"); return r;
        }
    }
    for (unsigned i = 0; i < 6; ++i) {
        u.projection[i/4][i%4] = as_float(s.xf(static_cast<std::uint16_t>(0x1020+i)));
        u.viewport[i/4][i%4] = as_float(s.xf(static_cast<std::uint16_t>(0x101a+i)));
        if (!std::isfinite(u.projection[i/4][i%4]) || !std::isfinite(u.viewport[i/4][i%4])) {
            r.status = fail(TransformError::invalid_input, "non-finite projection/viewport state"); return r;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!(p.computed_colors & (1u << i))) continue;
        u.ambient[i] = color(s.xf(static_cast<std::uint16_t>(0x100a+i)));
        u.material[i] = color(s.xf(static_cast<std::uint16_t>(0x100c+i)));
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (!(lights & (1u << i))) continue;
        auto& l = u.lights[i]; const auto base = static_cast<std::uint16_t>(0x600+i*16);
        l.color = color(s.xf(base+3));
        l.angle = read_row(base+4, 0, 3); l.distance = read_row(base+7, 0, 3);
        l.position = read_row(base+10, 0, 3); l.direction = read_row(base+13, 0, 3);
        if (((light_positions & (1u << i)) && !finite(l.position)) ||
            ((light_attenuation & (1u << i)) && (!finite(l.angle) || !finite(l.distance) ||
                (direction_matters(l, light_specular & (1u << i) ? TransformAttenuation::specular : TransformAttenuation::spot) &&
                 !finite(l.direction))))) {
            r.status = fail(TransformError::invalid_input, "non-finite live light parameters"); return r;
        }
    }
    r.snapshot = std::make_shared<const TransformSnapshot>(TransformSnapshot{p,u,s.revision()});
    return r;
}

TransformStatus validate_transform_clipping(const TransformPipeline& pipeline, const TransformOutput& output) {
    if (!pipeline.clipping_disabled) return {};
    const auto& c = output.gles_clip;
    if (!std::isfinite(c[3]) || c[3] <= 0)
        return fail(TransformError::unsupported_state, "clip-disabled vertex requires positive finite W");
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(c[i]))
            return fail(TransformError::unsupported_state, "non-finite clip-disabled vertex");
    if (std::abs(c[2]) > c[3])
        return fail(TransformError::unsupported_state, "clip-disabled vertex crosses the GLES depth volume");
    for (unsigned i = 0; i < 2; ++i)
        if (!std::isfinite(c[i] / c[3]))
            return fail(TransformError::unsupported_state, "clip-disabled viewport projection overflows");
    return {};
}

TransformStatus validate_transform_vertices(const TransformSnapshot& snapshot, const VertexSnapshot& vertices,
                                            TransformArithmetic model) {
    if (snapshot.state_revision != vertices.state_revision())
        return fail(TransformError::state_mismatch, "vertex and transform state revisions differ");
    if (model != TransformArithmetic::float32_candidate_v1)
        return fail(TransformError::unsupported_arithmetic, "unsupported transform arithmetic model");
    const TransformEvaluator evaluator(snapshot, model);
    if (!evaluator.status()) return evaluator.status();
    for (const auto& vertex : vertices.vertices()) {
        if (vertex.disabled) continue;
        const auto result = evaluator.evaluate(vertex);
        if (!result) return result.status;
        const auto clipping = validate_transform_clipping(snapshot.pipeline, result.output);
        if (!clipping) return clipping;
    }
    return {};
}

TransformShader generate_transform_glsl(const TransformPipeline& p, TransformArithmetic model, bool include_source, bool batched,
                                        std::uint8_t affine, bool compact, bool instanced) {
    TransformShader r;
    if (model != TransformArithmetic::float32_candidate_v1) {
        r.status = fail(TransformError::unsupported_arithmetic, "unsupported transform arithmetic model"); return r;
    }
    r.status = validate(p); if (!r.status) return r;
    r.key = {'G','X','X','F',1,static_cast<std::uint8_t>(model),p.color_count,p.texgen_count,
             p.computed_colors,p.normal_components,static_cast<std::uint8_t>(p.orthographic),
             static_cast<std::uint8_t>(p.dual_texture), static_cast<std::uint8_t>(batched), affine, static_cast<std::uint8_t>(compact),
             static_cast<std::uint8_t>(p.latched_normal), static_cast<std::uint8_t>(instanced)};
    if (batched && instanced) { r.status = fail(TransformError::unsupported_state, "a transform is batched or instanced"); return r; }
    if (instanced)
        for (unsigned i = 0; i < p.texgen_count; ++i)
            if (p.texgens[i].type == 1) { r.status = fail(TransformError::unsupported_state, "instanced transform with an emboss texgen"); return r; }
    if (batched) {
        bool object_space = uses_lighting_normal(p);
        for (unsigned i = 0; i < p.texgen_count; ++i) object_space |= p.texgens[i].type == 1 || (p.texgens[i].type == 0 && p.texgens[i].source_row < 5);
        if (object_space) { r.status = fail(TransformError::unsupported_state, "batched transform consumes object-space inputs"); return r; }
    }
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned channel = i % 2;
        if (!(p.computed_colors & (1u << channel)) || (i >= 2 && channel >= p.color_count)) continue;
        const auto& c = p.channels[i];
        r.key.insert(r.key.end(), {std::uint8_t(c.material_vertex), std::uint8_t(c.enabled)});
        if (c.material_vertex || (c.enabled && c.ambient_vertex)) r.key.push_back(std::uint8_t(vertex_color_source(p, channel)));
        if (c.enabled) {
            r.key.insert(r.key.end(), {std::uint8_t(c.ambient_vertex), c.light_mask});
            if (c.light_mask) r.key.insert(r.key.end(), {std::uint8_t(c.diffuse), std::uint8_t(c.attenuation)});
        }
    }
    for (unsigned i = 0; i < p.texgen_count; ++i) {
        const auto& t = p.texgens[i];
        r.key.push_back(t.type);
        if (t.type == 1) { r.key.insert(r.key.end(), {t.emboss_source, t.emboss_light}); continue; }
        if (t.type >= 2) continue;
        r.key.insert(r.key.end(), {t.source_row, std::uint8_t(t.projected)});
        if (t.source_row >= 5 && !(p.vertex_texcoords & (1u << (t.source_row-5)))) r.key.push_back(0xff); // Absent ST.
        if (p.dual_texture) r.key.insert(r.key.end(), {std::uint8_t(t.normalize), t.post_row});
    }
    if (!include_source) return r;
    std::ostringstream g;
    g << R"GLSL(#version 310 es
precision highp float;
precision highp int;
layout(location=0) in vec3 a_position;
)GLSL";
    if (batched) g << "layout(location=1) in vec4 a_register0;\nlayout(location=2) in vec4 a_register1;\n"
                      "layout(location=3) in vec4 a_register2;\nlayout(location=15) in vec4 a_register3;\n"
                      "flat out mediump vec4 gx_register0;\nflat out mediump vec4 gx_register1;\n"
                      "flat out mediump vec4 gx_register2;\nflat out mediump vec4 gx_register3;\n";
    else g << "layout(location=1) in vec3 a_normal;\nlayout(location=2) in vec3 a_binormal;\nlayout(location=3) in vec3 a_tangent;\n";
    g << R"GLSL(layout(location=4) in vec4 a_color0;
layout(location=5) in vec4 a_color1;
)GLSL";
    for (unsigned i = 0; i < 8; ++i) g << "layout(location=" << 6+i << ") in vec2 a_tex" << i << ";\n";
    g << R"GLSL(layout(location=14) in uvec3 a_matrix;
struct Light { vec4 color; vec4 angle; vec4 distance; vec4 position; vec4 direction; };
layout(std140, binding=0) uniform GXTransform {
    vec4 u_position[64];
    vec4 u_normal[32];
    vec4 u_post[64];
    vec4 u_projection[2];
    vec4 u_ambient[2];
    vec4 u_material[2];
    Light u_light[8];
    vec4 u_viewport[2];
    vec4 u_latched_normal[3];
};
out vec3 gx_eye;
out vec3 gx_normal;
out vec4 gx_clip;
out vec4 gx_gles_clip;
)GLSL";
    if (instanced)
        g << "layout(std140, binding=1) uniform GXInstances {\n    vec4 u_instance[" << transform_instance_limit * transform_instance_rows
          << "];\n};\n";
    g << (compact ? "out mediump vec4 gx_color0;\nout mediump vec4 gx_color1;\n" : "out vec4 gx_color0;\nout vec4 gx_color1;\n");
    for (unsigned i = 0; i < 8; ++i) g << "out " << ((affine >> i) & 1 ? "vec2" : "vec3") << " gx_tex" << i << ";\n";
    g << R"GLSL(int matrixIndex(int indexSlot) {
    return int((a_matrix[indexSlot/3] >> uint((indexSlot%3)*8)) & 63u);
}
vec3 unitVector(vec3 v) { return v / sqrt(dot(v,v)); }
float polynomial(vec3 c, float x) { return c.y == 0.0 && c.z == 0.0 ? c.x : (c.z*x+c.y)*x+c.x; }
float lightFactor(int light, int diffuse, int attenuation) {
    if (diffuse == 0 && attenuation == 0) return 1.0;
    vec3 ray = u_light[light].position.xyz;
    if (attenuation != 2) ray -= gx_eye;
    float distanceSquared = dot(ray, ray);
    ray = unitVector(ray);
    float ndotl = dot(gx_normal, ray);
    float factor = diffuse == 0 ? 1.0 : (diffuse == 1 ? ndotl : max(0.0,ndotl));
    if (attenuation != 0) {
        float angle;
        float distance;
        if (attenuation == 1) {
            angle = max(0.0, dot(ray,u_light[light].direction.xyz));
            distance = sqrt(distanceSquared);
        } else {
            angle = ndotl > 0.0 ? max(0.0,dot(gx_normal,u_light[light].direction.xyz)) : 0.0;
            distance = angle;
        }
        float numerator = max(0.0,polynomial(u_light[light].angle.xyz,angle));
        float denominator = polynomial(u_light[light].distance.xyz,distance);
        factor *= denominator == 0.0 ? (numerator == 0.0 ? 0.0 : 3.4e38) : numerator / denominator;
    }
    return factor;
}
void main() {
    int pn = matrixIndex(0);
    vec4 position = vec4(a_position,1.0);
)GLSL";
    if (batched) g << "    gx_eye = a_position;\n    gx_register0 = a_register0/255.0;\n    gx_register1 = a_register1/255.0;\n"
                      "    gx_register2 = a_register2/255.0;\n    gx_register3 = a_register3/255.0;\n";
    else if (instanced) {
        g << "    int ib = gl_InstanceID*" << transform_instance_rows << ";\n"
             "    gx_eye = vec3(dot(u_instance[ib],position),dot(u_instance[ib+1],position),dot(u_instance[ib+2],position));\n";
    } else {
        g << "    gx_eye = vec3(dot(u_position[pn],position),dot(u_position[pn+1],position),dot(u_position[pn+2],position));\n";
    }
    if (!batched) {
        if (p.latched_normal) g << "    vec3 normalIn = u_latched_normal[0].xyz;\n    vec3 binormalIn = u_latched_normal[1].xyz;\n"
                                   "    vec3 tangentIn = u_latched_normal[2].xyz;\n";
        else g << "    vec3 normalIn = a_normal;\n    vec3 binormalIn = a_binormal;\n    vec3 tangentIn = a_tangent;\n";
    }
    g << R"GLSL(    gx_normal = vec3(0.0);
    gx_color0 = vec4(0.0);
    gx_color1 = vec4(0.0);
)GLSL";
    for (unsigned i = 0; i < 8; ++i) g << "    gx_tex" << i << " = " << ((affine >> i) & 1 ? "vec2" : "vec3") << "(0.0);\n";
    if (uses_lighting_normal(p) && instanced) g << R"GLSL(    gx_normal = unitVector(vec3(dot(u_instance[ib+3].xyz,normalIn),dot(u_instance[ib+4].xyz,normalIn),dot(u_instance[ib+5].xyz,normalIn)));
)GLSL";
    else if (uses_lighting_normal(p)) g << R"GLSL(    int nr = pn % 32;
    gx_normal = unitVector(vec3(dot(u_normal[nr].xyz,normalIn),dot(u_normal[nr+1].xyz,normalIn),dot(u_normal[nr+2].xyz,normalIn)));
)GLSL";
    g << "    gx_clip = vec4(gx_eye.x*u_projection[0].x+";
    g << (p.orthographic ? "u_projection[0].y" : "gx_eye.z*u_projection[0].y");
    g << ",gx_eye.y*u_projection[0].z+";
    g << (p.orthographic ? "u_projection[0].w" : "gx_eye.z*u_projection[0].w");
    g << ",gx_eye.z*u_projection[1].x+u_projection[1].y," << (p.orthographic ? "1.0" : "-gx_eye.z") << ");\n";
    // No gl_PointSize: only triangles are rasterized, and Mali r20 makes every triangle
    // draw of a point-size-writing stage cost ~3x the CPU time in the driver.
    g << "    gx_gles_clip = vec4(gx_clip.xy,2.0*gx_clip.z+gx_clip.w,gx_clip.w);\n    gl_Position = gx_gles_clip;\n";
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned channel = i%2;
        if (!(p.computed_colors & (1u << channel)) || (i >= 2 && channel >= p.color_count)) continue;
        const auto& c = p.channels[i];
        g << "    {\n        vec4 material = " << (c.material_vertex ? vertex_color_glsl(p, channel) : "u_material["+index(channel)+"]") << ";\n";
        if (c.enabled) {
            g << "        vec4 sum = vec4(0.0);\n";
            for (unsigned light = 0; light < 8; ++light) if (c.light_mask & (1u << light))
                g << "        sum += lightFactor(" << light << ',' << unsigned(c.diffuse) << ',' << unsigned(c.attenuation) << ")*u_light[" << light << "].color;\n";
            g << "        vec4 ambient = " << (c.ambient_vertex ? vertex_color_glsl(p, channel) : "u_ambient["+index(channel)+"]") << ";\n";
            g << "        vec4 result = (material/255.0)*(clamp(ambient+trunc(sum),0.0,255.0)/255.0);\n";
        } else g << "        vec4 result = material/255.0;\n";
        g << "        gx_color" << channel << (i < 2 ? ".rgb = result.rgb;" : ".a = result.a;") << "\n    }\n";
    }
    for (unsigned i = 0; i < p.texgen_count; ++i) {
        const auto& t = p.texgens[i];
        const bool flat = (affine >> i) & 1;
        if (t.type >= 2) {
            g << "    gx_tex" << i << " = " << (flat ? "" : "vec3(") << "gx_color" << unsigned(t.type-2) << ".rg" << (flat ? "" : ",1.0)") << ";\n";
            continue;
        }
        if (t.type == 1) {
            const unsigned source = t.emboss_source;
            const bool source_flat = (affine >> source) & 1;
            g << "    {\n        int er = pn % 32;\n        vec3 ray = unitVector(u_light[" << unsigned(t.emboss_light) << "].position.xyz - gx_eye);\n"
                 "        vec3 sAxis = vec3(dot(u_normal[er].xyz,binormalIn),dot(u_normal[er+1].xyz,binormalIn),dot(u_normal[er+2].xyz,binormalIn));\n"
                 "        vec3 tAxis = vec3(dot(u_normal[er].xyz,tangentIn),dot(u_normal[er+1].xyz,tangentIn),dot(u_normal[er+2].xyz,tangentIn));\n"
                 "        vec2 offset = vec2(dot(sAxis,ray),dot(tAxis,ray));\n";
            if (flat) g << "        gx_tex" << i << " = gx_tex" << source << ".xy + offset;\n    }\n";
            else if (source_flat) g << "        gx_tex" << i << " = vec3(gx_tex" << source << " + offset,1.0);\n    }\n";
            else g << "        gx_tex" << i << " = gx_tex" << source << " + vec3(offset,0.0);\n    }\n";
            continue;
        }
        g << "    {\n        vec4 source = ";
        switch (t.source_row) {
        case 0: g << "position"; break;
        case 1: g << "vec4(normalIn,1.0)"; break;
        case 3: g << "vec4(binormalIn,1.0)"; break;
        case 4: g << "vec4(tangentIn,1.0)"; break;
        default:
            if (p.vertex_texcoords & (1u << (t.source_row-5))) g << "vec4(a_tex" << unsigned(t.source_row-5) << ",1.0,1.0)";
            else g << "vec4(0.0,0.0,1.0,1.0)";
            break;
        }
        g << ";\n        int tr = matrixIndex(" << i+1 << ");\n";
        if (flat) { g << "        gx_tex" << i << " = vec2(dot(u_position[tr],source),dot(u_position[tr+1],source));\n    }\n"; continue; }
        g << "        vec3 stq = vec3(dot(u_position[tr],source),dot(u_position[tr+1],source),";
        g << (t.projected ? "dot(u_position[tr+2],source)" : "1.0") << ");\n";
        if (p.dual_texture) {
            if (t.normalize) g << "        stq = unitVector(stq);\n";
            g << "        vec4 postSource = vec4(stq,1.0);\n        stq = vec3(dot(u_post[" << unsigned(t.post_row)
              << "],postSource),dot(u_post[" << unsigned(t.post_row+1) << "],postSource),dot(u_post[" << unsigned(t.post_row+2) << "],postSource));\n";
        }
        g << "        gx_tex" << i << " = stq;\n    }\n";
    }
    g << "}\n";
    r.source = g.str();
    return r;
}
} // namespace starlet::gx
