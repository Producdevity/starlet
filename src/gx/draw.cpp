#include "starlet/gx/draw.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <limits>

namespace starlet::gx {
namespace {
DrawCapture failure(DrawError error, const std::string& message) { return {{error, message}, {}}; }
void key_part(std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& part) {
    const auto size = static_cast<std::uint32_t>(part.size());
    for (unsigned i = 0; i < 4; ++i) key.push_back(static_cast<std::uint8_t>(size >> (i * 8)));
    key.insert(key.end(), part.begin(), part.end());
}

bool outside_viewport(const DrawPacket& packet) {
    const auto& indices = packet.vertices->indices();
    if (indices.empty()) return false;
    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3) {
        unsigned outside = 15;
        int w_sign = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const auto value = evaluate_transform(packet.transform->pipeline, packet.transform->uniforms,
                packet.vertices->vertices()[indices[triangle + i]], TransformArithmetic::float32_candidate_v1);
            if (!value) return false;
            const auto& clip = value.output.gles_clip;
            if (!std::isfinite(clip[3]) || clip[3] == 0) return false;
            const int sign = clip[3] > 0 ? 1 : -1;
            if (w_sign && sign != w_sign) return false;
            w_sign = sign;
            const double x = double(clip[0]) / clip[3], y = double(clip[1]) / clip[3];
            if (!std::isfinite(x) || !std::isfinite(y)) return false;
            outside &= (x < -1 ? 1u : 0u) | (x > 1 ? 2u : 0u) |
                       (y < -1 ? 4u : 0u) | (y > 1 ? 8u : 0u);
        }
        if (!outside) return false;
    }
    return true;
}

// Clip-disabled draws still clip depth, as GLES does for every draw: a draw whose vertices all
// lie behind the eye, or all beyond the same depth plane, writes nothing.
bool beyond_depth(const DrawPacket& packet, const TransformEvaluator& evaluator) {
    if (!packet.transform->pipeline.clipping_disabled) return false;
    bool any = false, behind = true, near = true, far = true, clipped = false;
    TransformOutput first{};
    for (const auto& v : packet.vertices->vertices()) {
        if (v.disabled) continue;
        const auto value = evaluator.evaluate(v);
        if (!value) return false;
        const auto& c = value.output.gles_clip;
        const bool before = !(c[2] >= -c[3]), after = !(c[2] <= c[3]);
        behind = behind && c[3] <= 0; near = near && before; far = far && after;
        if ((before || after) && !clipped) { clipped = true; first = value.output; }
        any = true;
    }
    if (clipped) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "starlet: gx: depth-clipped a clip-disabled draw (view z=%g, w=%g)\n",
                         first.eye_position[2], first.gles_clip[3]);
    }
    return any && (behind || near || far);
}

struct ProjectionVertex {
    std::array<double,4> clip{};
    std::array<std::array<double,3>,8> texcoords{};
};
std::string validate_visible_projection(const DrawPacket& packet, const GlesDrawPipeline& pipeline) {
    const auto& vertices = packet.vertices->vertices();
    const auto& indices = packet.vertices->indices();
    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3) {
        std::array<ProjectionVertex,12> first{}, second{};
        auto* input = first.data(); auto* output = second.data();
        std::size_t count = 3;
        for (unsigned i = 0; i < 3; ++i) {
            const auto evaluated = evaluate_transform(packet.transform->pipeline, packet.transform->uniforms,
                vertices[indices[triangle+i]], TransformArithmetic::float32_candidate_v1);
            if (!evaluated) return evaluated.status.message;
            for (unsigned c = 0; c < 4; ++c) input[i].clip[c] = evaluated.output.gles_clip[c];
            for (unsigned t = 0; t < 8; ++t)
                for (unsigned c = 0; c < 3; ++c) input[i].texcoords[t][c] = evaluated.output.texcoords[t][c];
        }
        for (unsigned plane = 0; plane < 6 && count; ++plane) {
            const auto distance = [plane](const ProjectionVertex& v) {
                return v.clip[3] + (plane & 1 ? -v.clip[plane/2] : v.clip[plane/2]);
            };
            auto previous = input[count-1];
            auto previous_distance = distance(previous);
            std::size_t written = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& current = input[i];
                const auto current_distance = distance(current);
                if ((previous_distance >= 0) != (current_distance >= 0)) {
                    if (written == first.size()) return "projective clipping polygon exceeds bound";
                    auto& intersection = output[written++];
                    const auto t = previous_distance / (previous_distance-current_distance);
                    for (unsigned c = 0; c < 4; ++c)
                        intersection.clip[c] = previous.clip[c] + t*(current.clip[c]-previous.clip[c]);
                    for (unsigned coordinate = 0; coordinate < 8; ++coordinate)
                        for (unsigned c = 0; c < 3; ++c)
                            intersection.texcoords[coordinate][c] = previous.texcoords[coordinate][c] +
                                t*(current.texcoords[coordinate][c]-previous.texcoords[coordinate][c]);
                }
                if (current_distance >= 0) {
                    if (written == first.size()) return "projective clipping polygon exceeds bound";
                    output[written++] = current;
                }
                previous = current; previous_distance = current_distance;
            }
            count = written; std::swap(input,output);
        }
        if (count < 3) continue;
        for (std::size_t i = 0; i < count; ++i)
            if (!(input[i].clip[3] > 0)) return "visible projective vertex requires positive W";
        const auto coordinate_valid = [&](unsigned coordinate, double scale_s, double scale_t, bool indirect) {
            int sign = 0;
            bool nonzero = false;
            constexpr double maximum = std::numeric_limits<float>::max();
            for (std::size_t i = 0; i < count; ++i) {
                const auto& stq = input[i].texcoords[coordinate];
                if (!std::isfinite(stq[2])) return false;
                if (stq[2] == 0) {
                    if (indirect) return false;
                    continue;
                }
                if (indirect && std::abs(stq[2]) <= 1e-20) return false;
                nonzero = true;
                const int current_sign = stq[2] > 0 ? 1 : -1;
                if (indirect && sign && sign != current_sign) return false;
                sign = current_sign;
                const auto s = stq[0]/stq[2], t = stq[1]/stq[2];
                if (!std::isfinite(s) || !std::isfinite(t) || std::abs(s) > maximum || std::abs(t) > maximum ||
                    std::abs(s*scale_s) > maximum || std::abs(t*scale_t) > maximum ||
                    (indirect && (std::abs(s*scale_s) >= 65536 || std::abs(t*scale_t) >= 65536))) return false;
            }
            return nonzero;
        };
        for (unsigned i = 0; i < packet.tev->pipeline.stage_count; ++i) {
            const auto& stage = packet.tev->pipeline.stages[i];
            if (tev_stage_samples(packet.tev->pipeline, i) && !coordinate_valid(stage.texture_coord,
                pipeline.coordinate_scale[i][0], pipeline.coordinate_scale[i][1], false))
                return "visible texture coordinate exceeds the floating-point domain";
        }
        if (packet.indirect->lookup_mask)
            for (unsigned i = 0; i < packet.transform->pipeline.texgen_count; ++i) {
                const auto& scale = packet.raster->coordinates[i];
                if (!coordinate_valid(i,scale.s,scale.t,true))
                    return "visible indirect coordinates exceed the candidate S17.7 domain";
            }
    }
    return {};
}
}

DrawCapture capture_draw(const State& state, const Draw& draw, MemoryReader& memory,
                         DrawLimits limits, TextureCmprModel cmpr, TextureCache* cache) {
    return capture_draw(state,draw,memory,limits,cmpr,cache,nullptr);
}
DrawCapture capture_draw(const State& state, const Draw& draw, MemoryReader& memory,
                         DrawLimits limits, TextureCmprModel cmpr, TextureCache* cache, VertexCache* vertex_cache) {
    auto packet = std::make_shared<DrawPacket>();
    packet->retained_bytes = sizeof(DrawPacket) + sizeof(TransformSnapshot) + sizeof(TevMaterial) + sizeof(RasterSnapshot) + sizeof(IndirectSnapshot);
    auto add_bytes = [&](std::size_t bytes) {
        if (packet->retained_bytes > limits.max_retained_bytes || bytes > limits.max_retained_bytes - packet->retained_bytes)
            return false;
        packet->retained_bytes += bytes;
        return true;
    };
    if (!add_bytes(0)) return failure(DrawError::storage_limit, "draw fixed-state storage exceeds limit");
    const auto vertices = capture_vertices(state, draw, memory, limits.vertices, vertex_cache);
    if (!vertices) return failure(DrawError::vertices, vertices.message);
    if (!add_bytes(vertices.snapshot->storage_bytes())) return failure(DrawError::storage_limit, "draw vertices exceed retained limit");
    packet->vertices = vertices.snapshot;
    const auto tev = capture_tev(state);
    if (!tev) return failure(DrawError::tev, tev.status.message);
    packet->tev = tev.material;
    const auto indirect = capture_indirect(state, tev.material->pipeline);
    if (!indirect) return failure(DrawError::tev, indirect.status.message);
    packet->indirect = indirect.snapshot;
    std::uint8_t raster_colors = 0;
    for (unsigned i = 0; i < tev.material->pipeline.stage_count; ++i)
        raster_colors |= tev_input_use(tev.material->pipeline.stages[i]).raster_mask;
    const auto transform = capture_transform(state, *vertices.snapshot, raster_colors);
    if (!transform) return failure(DrawError::transform, transform.status.message);
    packet->transform = transform.snapshot;
    const auto raster = capture_raster(state);
    if (!raster) return failure(DrawError::raster, raster.status.message);
    packet->raster = raster.snapshot;
    if ((state.bp(0x43)&7)==1) {
        const auto pixel = capture_pixel(state,PixelPolicy::candidate_v1);
        if (!pixel) return failure(DrawError::raster,pixel.status.message);
        if (!add_bytes(sizeof(PixelSnapshot))) return failure(DrawError::storage_limit,"pixel state exceeds retained limit");
        packet->pixel = pixel.snapshot;
    }
    auto capture_map = [&](unsigned map) -> DrawStatus {
        if (packet->textures[map]) return {};
        const auto texture = capture_texture(state, map, memory, cmpr, limits.textures, cache);
        if (!texture) return {DrawError::texture, texture.message};
        if (!add_bytes(texture.snapshot->storage_bytes()))
            return {DrawError::storage_limit, "draw textures exceed retained limit"};
        packet->textures[map] = texture.snapshot;
        packet->texture_mask |= 1u << map;
        return {};
    };
    for (unsigned i = 0; i < tev.material->pipeline.stage_count; ++i) {
        const auto& stage = tev.material->pipeline.stages[i];
        const auto use = tev_input_use(stage);
        if (use.raster_mask & ~transform.snapshot->pipeline.computed_colors)
            return failure(DrawError::missing_input, "TEV consumes an unavailable raster color");
        if (!tev_stage_samples(tev.material->pipeline, i)) continue;
        if (stage.texture_coord >= transform.snapshot->pipeline.texgen_count)
            return failure(DrawError::missing_input, "TEV consumes an unavailable texture coordinate");
        const auto status = capture_map(stage.texture_map);
        if (!status) return failure(status.error, status.message);
    }
    for (unsigned i = 0; i < 4; ++i) if (packet->indirect->lookup_mask & (1u << i)) {
        const auto& lookup = packet->indirect->lookups[i];
        if (lookup.coordinate >= transform.snapshot->pipeline.texgen_count)
            return failure(DrawError::missing_input, "indirect lookup consumes an unavailable coordinate");
        // A map may serve regular and indirect lookups alike; it is captured once.
        const auto status = capture_map(lookup.map);
        if (!status) return failure(status.error, status.message);
    }
    return {{}, std::move(packet)};
}

GlesDrawResult plan_gles_draw(const DrawPacket& packet, FramebufferFetch fetch, bool include_source) {
    auto reject = [](const std::string& reason) { return GlesDrawResult{{DrawError::unsupported_state, reason}, {}}; };
    if (!packet.vertices || !packet.transform || !packet.tev || !packet.raster || !packet.indirect)
        return reject("incomplete draw packet");
    const auto revision = packet.vertices->state_revision();
    if (packet.transform->state_revision != revision || packet.tev->state_revision != revision ||
        packet.raster->state_revision != revision || packet.indirect->state_revision != revision)
        return reject("mixed draw-state revisions");
    if (packet.vertices->topology() != Topology::triangles)
        return reject("GLES checkpoint admits triangle topology only");
    const bool rgba6 = (packet.raster->pixel_control&7)==1;
    if (rgba6 && (!packet.pixel || packet.pixel->state_revision()!=revision ||
        packet.pixel->plan().format!=PixelFormat::rgba6 || packet.pixel->plan().operation==PixelOperation::logic))
        return reject("RGBA6 renderer requires owned copy/blend/subtract pixel state");
    const bool needs_fetch = rgba6 && packet.pixel->plan().operation!=PixelOperation::copy;
    if (needs_fetch && fetch!=FramebufferFetch::arm && fetch!=FramebufferFetch::ext)
        return reject("RGBA6 blending requires coherent framebuffer fetch");
    const auto raster = plan_gles_raster(*packet.raster,rgba6);
    if (!raster) return reject(raster.status.message);
    const auto vertex = generate_transform_glsl(packet.transform->pipeline, TransformArithmetic::float32_candidate_v1, false);
    if (!vertex) return reject(vertex.status.message);
    const auto tev = generate_tev_shader(packet.tev->pipeline, TevArithmeticModel::fixed8_candidate_v1, false);
    if (!tev) return reject(tev.status.message);
    GlesDrawPipeline p;
    p.raster = raster.plan;
    p.rgba6_copy = rgba6;
    if (rgba6) { p.pixel_controls=packet.pixel->uniforms();p.alpha_write=packet.pixel->plan().alpha_write; }
    p.key = {'G','D','R','W',6,static_cast<std::uint8_t>(rgba6),
        static_cast<std::uint8_t>(needs_fetch ? fetch : FramebufferFetch::none),static_cast<std::uint8_t>(p.raster.early_depth)};
    if (rgba6) {
        const auto& plan = packet.pixel->plan();
        p.key.insert(p.key.end(), {static_cast<std::uint8_t>(plan.operation), plan.source_factor, plan.destination_factor,
            static_cast<std::uint8_t>(plan.color_write | (plan.alpha_write << 1) | (plan.dither << 2) | (plan.force_alpha << 3))});
    }
    key_part(p.key, vertex.key); key_part(p.key, tev.key);
    std::uint8_t maps = 0, mip_maps = 0;
    auto sampling = [&](unsigned map, unsigned coordinate) -> std::string {
        if (map >= 8 || coordinate >= packet.transform->pipeline.texgen_count || !packet.textures[map])
            return "texture sampling input is absent from packet";
        const auto& texture = *packet.textures[map];
        if (texture.state_revision() != revision) return "mixed texture-state revisions";
        const auto& desc = texture.descriptor();
        const auto& sampler = desc.sampler;
        const auto filter = static_cast<unsigned>(sampler.min_filter);
        if (sampler.linear_magnification != ((filter & 4) != 0) || sampler.bias_clamp || sampler.anisotropy != 1)
            return "GLES isotropic sampler requires matching min/mag filters without bias clamp";
        if (filter != 0 && filter != 4) mip_maps |= 1u << map;
        p.sampler_lod[map] = {{sampler.lod_bias_thirtyseconds/32.0f, sampler.min_lod_sixteenths/16.0f,
                               sampler.max_lod_sixteenths/16.0f, sampler.edge_lod ? 1.0f : 0.0f}};
        const auto& scale = packet.raster->coordinates[coordinate];
        if ((scale.words[0] | scale.words[1]) & 0x20000)
            return "GX cylindrical coordinate wrapping is not implemented";
        const auto automatic_repeat = [](std::uint32_t word, TextureWrap wrap, unsigned scale, unsigned size) {
            return !(word & 0x10000) ||
                (wrap == TextureWrap::repeat && scale == size && (size & (size - 1)) == 0);
        };
        if (!automatic_repeat(scale.words[0], sampler.wrap_s, scale.s, desc.width) ||
            !automatic_repeat(scale.words[1], sampler.wrap_t, scale.t, desc.height))
            return "GX range bias requires the SDK automatic power-of-two repeat mapping";
        maps |= 1u << map;
        return {};
    };
    for (unsigned i = 0; i < packet.tev->pipeline.stage_count; ++i) {
        const auto& stage = packet.tev->pipeline.stages[i];
        const auto use = tev_input_use(stage);
        if (use.raster_mask & ~packet.transform->pipeline.computed_colors)
            return reject("TEV raster input is absent from the transform");
        if (!tev_stage_samples(packet.tev->pipeline, i)) continue;
        const auto error = sampling(stage.texture_map, stage.texture_coord);
        if (!error.empty()) return reject(error);
        const auto& desc = packet.textures[stage.texture_map]->descriptor();
        const auto& scale = packet.raster->coordinates[stage.texture_coord];
        p.coordinate_scale[i] = {{float(scale.s) / desc.width, float(scale.t) / desc.height}};
        p.image_size[i] = {{float(desc.width), float(desc.height)}};
        const auto& indirect = packet.indirect->stages[i];
        if (indirect.matrix > 3 || indirect.lookup > 3 || indirect.bias > 7 ||
            (indirect.matrix && !(packet.indirect->lookup_mask & (1u << indirect.lookup))))
            return reject("invalid captured indirect stage");
    }
    p.key.push_back(packet.indirect->lookup_mask);
    for (unsigned i = 0; i < packet.tev->pipeline.stage_count; ++i) {
        const auto& stage = packet.indirect->stages[i];
        p.key.insert(p.key.end(), {stage.matrix, stage.lookup, stage.bias});
    }
    if (packet.indirect->lookup_mask & ~15u) return reject("invalid indirect lookup mask");
    for (unsigned i = 0; i < 4; ++i) if (packet.indirect->lookup_mask & (1u << i)) {
        const auto& lookup = packet.indirect->lookups[i];
        if (lookup.shift_s > 15 || lookup.shift_t > 15) return reject("invalid indirect coordinate shift");
        const auto error = sampling(lookup.map, lookup.coordinate);
        if (!error.empty()) return reject(error);
        const auto& scale = packet.raster->coordinates[lookup.coordinate];
        const auto& desc = packet.textures[lookup.map]->descriptor();
        p.indirect_scale[i] = {{float(scale.s)*128, float(scale.t)*128,
                               1.0f/(128*desc.width), 1.0f/(128*desc.height)}};
        p.key.insert(p.key.end(), {lookup.map, lookup.coordinate, lookup.shift_s, lookup.shift_t});
    }
    if (maps != packet.texture_mask) return reject("packet texture mask does not match consumed maps");
    // Only indirect integer conversion requires continuous, bounded projected coordinates.
    bool projection_proven = true;
    const TransformEvaluator evaluator(*packet.transform, TransformArithmetic::float32_candidate_v1);
    if (!evaluator.status()) return reject(evaluator.status().message);
    if (beyond_depth(packet, evaluator)) {
        p.discarded = true;
        return {{}, std::move(p)};
    }
    for (const auto& v : packet.vertices->vertices()) {
        if (v.disabled) continue;
        const auto evaluated = evaluator.evaluate(v);
        if (!evaluated) return reject(evaluated.status.message);
        const auto clipping = validate_transform_clipping(packet.transform->pipeline, evaluated.output);
        if (!clipping) {
            // The raster plan already restricts the scissor to the viewport.
            if (outside_viewport(packet)) {
                p.discarded = true;
                return {{}, std::move(p)};
            }
            return reject(clipping.message);
        }
        for (unsigned i = 0; i < packet.tev->pipeline.stage_count; ++i) {
            const auto& stage = packet.tev->pipeline.stages[i];
            if (!tev_stage_samples(packet.tev->pipeline, i)) continue;
            const auto& stq = evaluated.output.texcoords[stage.texture_coord];
            if (stq[2] == 0 || !std::isfinite(stq[0] / stq[2] * p.coordinate_scale[i][0]) ||
                !std::isfinite(stq[1] / stq[2] * p.coordinate_scale[i][1]))
                projection_proven = false;
        }
        if (packet.indirect->lookup_mask) {
            for (unsigned coordinate = 0; coordinate < packet.transform->pipeline.texgen_count; ++coordinate) {
                const auto& stq = evaluated.output.texcoords[coordinate];
                const auto& scale = packet.raster->coordinates[coordinate];
                if (stq[2] <= 1e-20f || !std::isfinite(stq[0]/stq[2]) || !std::isfinite(stq[1]/stq[2]) ||
                    std::abs(stq[0]/stq[2]*scale.s) >= 65536 || std::abs(stq[1]/stq[2]*scale.t) >= 65536)
                    projection_proven = false;
            }
        }
    }
    // Texture division follows homogeneous clipping; original vertices can be singular offscreen.
    if (!projection_proven) {
        const auto error = validate_visible_projection(packet,p);
        if (!error.empty()) return reject(error);
    }
    p.key.push_back(mip_maps);
    const ZTextureProgram z_texture{packet.tev->pipeline.z_texture_op, packet.tev->pipeline.z_texture_type, p.raster.depth_test};
    if (z_texture.op) p.key.insert(p.key.end(), {'Z', z_texture.op, z_texture.type, static_cast<std::uint8_t>(z_texture.depth)});
    if (include_source) {
        const auto vertex = generate_transform_glsl(packet.transform->pipeline, TransformArithmetic::float32_candidate_v1);
        const auto tev = generate_tev_shader(packet.tev->pipeline, TevArithmeticModel::fixed8_candidate_v1);
        p.vertex = vertex.source;
        p.fragment = gles_fragment_source(packet.tev->pipeline, tev.functions, *packet.indirect, maps, mip_maps,
                                          rgba6 ? &packet.pixel->plan() : nullptr, needs_fetch, fetch, p.raster.early_depth,
                                          packet.raster->efb.height, z_texture);
    }
    // Replacing every color component avoids reinterpreting the prior EFB format.
    if (!needs_fetch && !p.raster.cull && !p.raster.depth_test && p.raster.color_write && (rgba6 ? p.alpha_write : !p.raster.blend) && !packet.texture_mask &&
        p.raster.viewport.x==0 && p.raster.viewport.y==0 && p.raster.viewport.width==EfbLayout::width &&
        p.raster.viewport.height==packet.raster->efb.height && p.raster.scissor.x==0 && p.raster.scissor.y==0 &&
        p.raster.scissor.width==EfbLayout::width && p.raster.scissor.height==packet.raster->efb.height &&
        packet.vertices->vertices().size()==4 && packet.vertices->indices()==std::vector<std::uint16_t>{0,1,2,0,2,3}) {
        constexpr float corners[4][2]={{-1,1},{1,1},{1,-1},{-1,-1}};
        TevInputs inputs;inputs.raster_valid=packet.transform->pipeline.computed_colors;
        bool full=true;
        std::array<TransformVector,2> colors{};
        for (unsigned i=0;i<4;++i) {
            const auto v=evaluate_transform(packet.transform->pipeline,packet.transform->uniforms,
                packet.vertices->vertices()[i],TransformArithmetic::float32_candidate_v1);
            if (!v || packet.vertices->vertices()[i].disabled || v.output.gles_clip[3]!=1 ||
                std::abs(v.output.gles_clip[0]-corners[i][0])>1e-6f ||
                std::abs(v.output.gles_clip[1]-corners[i][1])>1e-6f ||
                v.output.gles_clip[2]<-1 || v.output.gles_clip[2]>1) { full=false;break; }
            if (!i) colors=v.output.colors;
            else if (colors!=v.output.colors) { full=false;break; }
        }
        if (full) {
            for(unsigned c=0;c<2;++c) for(unsigned k=0;k<4;++k)
                inputs.raster[c][k]=static_cast<std::uint8_t>(std::floor(std::clamp(colors[c][k],0.0f,1.0f)*255+0.5f));
            const auto output=evaluate_tev(packet.tev->pipeline,packet.tev->uniforms,inputs,TevArithmeticModel::fixed8_candidate_v1);
            p.full_replacement=output && output.alpha_pass && (rgba6 || output.color[3]==255);
        }
    }
    return {{}, std::move(p)};
}

std::string gles_fragment_source(const TevPipeline& tev_pipeline, const std::string& tev_functions,
                                 const IndirectSnapshot& indirect, std::uint8_t maps, std::uint8_t mip_maps,
                                 const PixelPlan* rgba6, bool needs_fetch, FramebufferFetch fetch, bool early_depth,
                                 int efb_height, const ZTextureProgram& z_texture) {
    std::ostringstream fragment;
    fragment << "#version 310 es\n";
    if (needs_fetch) fragment << (fetch==FramebufferFetch::arm ?
        "#extension GL_ARM_shader_framebuffer_fetch : require\n" :
        "#extension GL_EXT_shader_framebuffer_fetch : require\n");
    if (early_depth) fragment << "layout(early_fragment_tests) in;\n";
    fragment << "precision highp float;\nprecision highp int;\n"
             << "in vec4 gx_color0;\nin vec4 gx_color1;\n";
    std::uint8_t coordinates = 0;
    for (unsigned i = 0; i < tev_pipeline.stage_count; ++i)
        if (tev_stage_samples(tev_pipeline, i)) coordinates |= 1u << tev_pipeline.stages[i].texture_coord;
    for (unsigned i = 0; i < 4; ++i) if (indirect.lookup_mask & (1u << i)) coordinates |= 1u << indirect.lookups[i].coordinate;
    for (unsigned i = 0; i < 8; ++i) if (coordinates & (1u << i)) fragment << "in vec3 gx_tex" << i << ";\n";
    if (mip_maps) fragment << texture_sampling_glsl();
    for (unsigned map = 0; map < 8; ++map) if (maps & (1u << map))
        fragment << "uniform highp sampler2D uGxTexture" << map << ";\n";
    fragment << "uniform vec2 uGxCoordinateScale[16];\n";
    if (indirect_coordinates(indirect)) fragment << "uniform vec2 uGxImageSize[16];\n";
    if (indirect.lookup_mask) fragment <<
        "uniform vec4 uGxIndirectScale[4];\n"
        "uniform ivec4 uGxIndirectRows[6];\n" << indirect_offset_glsl();
    fragment << (needs_fetch && fetch==FramebufferFetch::ext ?
        "layout(location=0) inout highp vec4 gxFragmentColor;\n" :
        "layout(location=0) out vec4 gxFragmentColor;\n");
    if (needs_fetch && fetch==FramebufferFetch::arm) fragment << "highp vec4 gl_LastFragColorARM;\n";
    // The Z texel is the last stage's raw lookup, before its swap table.
    const bool z_code = z_texture.op && z_texture.depth;
    const auto z_glsl = z_code ? z_texture_glsl(z_texture, "samples[" + std::to_string(tev_pipeline.stage_count - 1) + "]")
                               : ZTextureGlsl{};
    fragment
             << "ivec4 gxQuantize(vec4 v) { return ivec4(floor(clamp(v,0.0,1.0)*255.0+0.5)); }\n"
             << (rgba6 ? pixel_stage_glsl(*rgba6) + "uniform uvec4 uGxPixel;\n" : "")
             << z_glsl.globals
             << tev_functions
             << "void main() {\n" << z_glsl.reference << "    ivec4 samples[16];\n    ivec4 raster[2];\n"
             << "    for (int i=0;i<16;++i) samples[i]=ivec4(0);\n"
             << "    raster[0]=gxQuantize(gx_color0); raster[1]=gxQuantize(gx_color1);\n";
    auto sample_expression = [&](unsigned map, const std::string& uv) {
        if (mip_maps & (1u << map)) return "gxSampleMip(uGxTexture" + std::to_string(map) + "," + uv +
            ",uGxSamplerLod[" + std::to_string(map) + "])";
        return "texture(uGxTexture" + std::to_string(map) + "," + uv + ")";
    };
    for (unsigned i = 0; i < 4; ++i) if (indirect.lookup_mask & (1u << i)) {
        const auto& lookup = indirect.lookups[i];
        fragment << "    ivec2 indirectUV" << i << "=ivec2((gx_tex" << unsigned(lookup.coordinate)
            << ".xy/gx_tex" << unsigned(lookup.coordinate) << ".z)*uGxIndirectScale[" << i << "].xy)>>ivec2("
            << unsigned(lookup.shift_s) << "," << unsigned(lookup.shift_t) << ");\n"
            << "    ivec4 indirectSample" << i << "=gxQuantize(" << sample_expression(lookup.map,
                "vec2(indirectUV" + std::to_string(i) + ")*uGxIndirectScale[" + std::to_string(i) + "].zw") << ");\n";
    }
    for (unsigned i = 0; i < tev_pipeline.stage_count; ++i) {
        const auto& stage = tev_pipeline.stages[i];
        if (!tev_stage_samples(tev_pipeline, i)) continue;
        const auto& stage_indirect = indirect.stages[i];
        if (indirect_coordinate(stage_indirect)) {
            fragment << "    ivec2 base" << i << "=ivec2((gx_tex" << unsigned(stage.texture_coord)
                << ".xy/gx_tex" << unsigned(stage.texture_coord) << ".z)*uGxCoordinateScale[" << i
                << "]*uGxImageSize[" << i << "]*128.0);\n";
            if (stage_indirect.matrix)
                fragment << "    ivec2 offset" << i << "=gxIndirectOffset(indirectSample" << unsigned(stage_indirect.lookup)
                    << ",uGxIndirectRows[" << (stage_indirect.matrix-1)*2 << "],uGxIndirectRows["
                    << (stage_indirect.matrix-1)*2+1 << "],ivec3(" << ((stage_indirect.bias&1)?128:0) << ","
                    << ((stage_indirect.bias&2)?128:0) << "," << ((stage_indirect.bias&4)?128:0) << "));\n";
            else fragment << "    ivec2 offset" << i << "=ivec2(0);\n";
            fragment << "    ivec2 fixed" << i << "=(" << indirect_wrap_glsl(stage_indirect, "base" + std::to_string(i)) << "+offset" << i << ")&ivec2(0xffffff);\n"
                << "    fixed" << i << "=(fixed" << i << "&ivec2(0x7fffff))-(fixed" << i << "&ivec2(0x800000));\n"
                << "    samples[" << i << "]=gxQuantize(" << sample_expression(stage.texture_map,
                    "vec2(fixed" + std::to_string(i) + ")/(128.0*uGxImageSize[" + std::to_string(i) + "])") << ");\n";
        } else {
            fragment << "    samples[" << i << "]=gxQuantize(" << sample_expression(stage.texture_map,
                "(gx_tex" + std::to_string(stage.texture_coord) + ".xy/gx_tex" + std::to_string(stage.texture_coord) +
                ".z)*uGxCoordinateScale[" + std::to_string(i) + "]") << ");\n";
        }
    }
    fragment << "    ivec4 color=gxTevEvaluate(samples,raster);\n"
             << "    if (!gxTevAlphaPass(color.a)) discard;\n" << z_glsl.depth;
    if (rgba6) {
        fragment << "    uvec4 destination=";
        if (needs_fetch) fragment << "uvec4(floor(" << (fetch==FramebufferFetch::arm ? "gl_LastFragColorARM" : "gxFragmentColor") << "*255.0+0.5))>>2u;\n";
        else fragment << "uvec4(0);\n";
        fragment << "    uvec4 q=gxPixelStage(uvec4(clamp(color,0,255)),destination,ivec2(int(gl_FragCoord.x)," << efb_height - 1 << "-int(gl_FragCoord.y)),uGxPixel.y&255u);\n"
                 << "    gxFragmentColor=vec4((q<<2)|(q>>4))/255.0;\n}\n";
    } else fragment << "    gxFragmentColor=vec4(color)/255.0;\n}\n";
    return fragment.str();
}

} // namespace starlet::gx
