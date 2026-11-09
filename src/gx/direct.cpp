#include "starlet/gx/direct.hpp"

#include <cstring>

namespace starlet::gx {
namespace {
DrawStatus reject(DrawError error, const std::string& message) { return {error, message}; }
unsigned texture_filter(const State& state, unsigned map) {
    return (state.bp(static_cast<std::uint8_t>((map < 4 ? 0x80 : 0xa0) + map % 4)) >> 5) & 7;
}
}

namespace {
// Bit 2i: a colour component of TEV register i lies outside 0..255; bit 2i+1: its alpha
// does. Only those registers need the fast program's 8-bit operand wrap.
std::uint8_t wide_registers(const State& state) {
    auto wide = [](std::uint32_t word, unsigned shift) {
        const auto value = static_cast<std::int32_t>((word >> shift) & 0x7ff) - (((word >> shift) & 0x400) ? 0x800 : 0);
        return value < 0 || value > 255;
    };
    std::uint8_t bits = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto ra = state.tev_register(false, static_cast<std::uint8_t>(i * 2));
        const auto bg = state.tev_register(false, static_cast<std::uint8_t>(i * 2 + 1));
        if (wide(ra, 0) || wide(bg, 0) || wide(bg, 12)) bits |= 1u << (2 * i);
        if (wide(ra, 12)) bits |= 1u << (2 * i + 1);
    }
    return bits;
}
// GX_PTIDENTITY (post-transform rows 61-63) holds the identity, as GXInit loads it.
bool identity_post(const State& state) {
    static constexpr std::uint32_t identity[12] = {0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0};
    return !std::memcmp(state.xf_data() + 0x5f4, identity, sizeof identity);
}
// Texgens whose q stays 1: 2x4 or colour sources whose post transform, if any, is the identity.
std::uint8_t affine_coordinates(const State& state, const TransformPipeline& p) {
    const bool identity = identity_post(state);
    std::uint8_t mask = 0;
    for (unsigned i = 0; i < p.texgen_count; ++i) {
        const auto& t = p.texgens[i];
        if (t.type >= 2 || (!t.projected && (!p.dual_texture || (identity && t.post_row == 61 && !t.normalize)))) mask |= 1u << i;
    }
    return mask;
}
// Fog type, projection and range-adjustment enable select the fast fragment program.
std::uint8_t fast_fog(const State& state) {
    return static_cast<std::uint8_t>(((state.bp(0xf1) >> 21) & 7) | ((state.bp(0xf1) >> 20) & 1) << 3 |
                                     ((state.bp(0xe8) >> 10) & 1) << 4);
}
}

void direct_program_words(const State& state, const VertexLayout& layout, FramebufferFetch fetch, DirectShading shading,
                          std::vector<std::uint32_t>& words) {
    words.clear();
    const auto format = stream_format(layout);
    const bool fast = shading == DirectShading::fast;
    words.push_back(std::uint32_t{format.normal_vectors} | (std::uint32_t{format.colors} << 4) |
                    (std::uint32_t{format.texcoords} << 8) | (static_cast<std::uint32_t>(fetch) << 16) | (std::uint32_t{fast} << 20));
    for (std::uint16_t a : {0x1005, 0x1009, 0x100e, 0x100f, 0x1010, 0x1011, 0x1012, 0x1026, 0x103f}) words.push_back(state.xf(a));
    for (std::uint16_t a = 0x1040; a < 0x1048; ++a) words.push_back(state.xf(a));
    for (std::uint16_t a = 0x1050; a < 0x1058; ++a) words.push_back(state.xf(a));
    words.push_back(state.bp(0x00) & ~0xc000u); // Cull mode is raster state.
    for (unsigned a = 0x10; a < 0x20; ++a) words.push_back(state.bp(static_cast<std::uint8_t>(a)));
    for (unsigned a = 0x25; a < 0x30; ++a) words.push_back(state.bp(static_cast<std::uint8_t>(a)));
    for (unsigned a = 0xc0; a < 0xe0; ++a) words.push_back(state.bp(static_cast<std::uint8_t>(a)));
    words.push_back(state.bp(0xf3) & 0xff0000u); // Alpha references are uniforms.
    for (unsigned a = 0xf5; a < 0xfe; ++a) words.push_back(state.bp(static_cast<std::uint8_t>(a)));
    const auto pixel = state.bp(0x43);
    words.push_back(pixel);
    if (fast) {
        // Blending is GL state; only the forced-alpha enable shapes the program.
        words.push_back((state.bp(0x40) & 1u) | ((pixel & 7) == 1 ? (state.bp(0x42) & 0x100u) : 0u));
        words.push_back(identity_post(state));
        words.push_back(fast_fog(state));
        words.push_back(direct_program_register_word(state, shading));
        return;
    }
    // RGBA6 programs fix blend/subtract, factors, masks, dither and the forced-alpha enable.
    words.push_back((state.bp(0x40) & 1u) | ((pixel & 7) == 1 ? ((state.bp(0x41) & 0xfffu) << 1) | ((state.bp(0x42) & 0x100u) << 5) : 0u));
    std::uint32_t filters = 0;
    for (unsigned map = 0; map < 8; ++map) filters |= texture_filter(state, map) << (map * 3);
    words.push_back(filters);
}

namespace {
struct NoMemory : MemoryReader {
    bool copy(std::uint32_t, std::uint8_t*, std::size_t) override { return false; }
};
struct NoDraws : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return false; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
}

std::uint32_t direct_program_register_word(const State& state, DirectShading shading) {
    return shading == DirectShading::fast ? wide_registers(state) : 0;
}

bool direct_program_state(const std::vector<std::uint32_t>& words, State& state, VertexLayout& layout) {
    if (words.size() != 101) return false;
    const auto format = words[0];
    const auto normals = format & 15, colors = (format >> 4) & 15, fetch = (format >> 16) & 15;
    if ((format >> 20) != 1 || normals == 2 || normals > 3 || colors > 3 || fetch > 2) return false;
    NoMemory memory;
    NoDraws sink;
    Decoder decoder(memory, sink);
    bool ok = true;
    auto u8 = [&](std::uint8_t v) { ok = ok && decoder.write_u8(v); };
    auto u32 = [&](std::uint32_t v) { ok = ok && decoder.write_u32(v); };
    auto xf = [&](std::uint16_t address, const std::uint32_t* values, unsigned count) {
        u8(0x10); u32(((count - 1u) << 16) | address);
        for (unsigned i = 0; i < count; ++i) u32(values[i]);
    };
    auto bp = [&](unsigned address, std::uint32_t value) { u8(0x61); u32((address << 24) | (value & 0xffffffu)); };
    std::size_t at = 1;
    for (std::uint16_t a : {0x1005, 0x1009, 0x100e, 0x100f, 0x1010, 0x1011, 0x1012, 0x1026, 0x103f}) xf(a, &words[at++], 1);
    xf(0x1040, &words[at], 8); at += 8;
    xf(0x1050, &words[at], 8); at += 8;
    bp(0x00, words[at++]);
    for (unsigned a = 0x10; a < 0x20; ++a) bp(a, words[at++]);
    for (unsigned a = 0x25; a < 0x30; ++a) bp(a, words[at++]);
    for (unsigned a = 0xc0; a < 0xe0; ++a) bp(a, words[at++]);
    bp(0xf3, words[at++]);
    for (unsigned a = 0xf5; a < 0xfe; ++a) bp(a, words[at++]);
    bp(0x43, words[at++]);
    const auto blend = words[at++];
    bp(0x40, blend & 1u);
    bp(0x42, blend & 0x100u);
    if (words[at++]) {
        static constexpr std::uint32_t identity[12] = {0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0};
        xf(0x5f4, identity, 12);
    }
    const auto fog = words[at++];
    bp(0xf1, ((fog & 7u) << 21) | (((fog >> 3) & 1u) << 20));
    bp(0xe8, ((fog >> 4) & 1u) << 10);
    // An out-of-range component stands for every one the wide flags cover.
    const auto wide = words[at++];
    for (unsigned i = 0; i < 4; ++i) {
        const std::uint32_t color = wide & (1u << (2 * i)) ? 0x7ffu : 0, alpha = wide & (2u << (2 * i)) ? 0x7ffu : 0;
        bp(0xe0 + 2 * i, color | (alpha << 12));
        bp(0xe1 + 2 * i, 0);
    }
    if (!ok) return false;
    state = decoder.state();
    layout = {};
    if (normals) { layout.normal.mode = AttributeMode::direct; layout.normal.components = normals == 3 ? 9 : 3; }
    for (unsigned i = 0; i < 2; ++i) if (colors & (1u << i)) layout.colors[i].mode = AttributeMode::direct;
    for (unsigned i = 0; i < 8; ++i) if (format & (0x100u << i)) layout.texcoords[i].mode = AttributeMode::direct;
    std::vector<std::uint32_t> rebuilt;
    direct_program_words(state, layout, static_cast<FramebufferFetch>(fetch), DirectShading::fast, rebuilt);
    return rebuilt == words;
}

DirectProgramResult plan_direct_program(const State& state, const VertexLayout& layout, FramebufferFetch fetch, DirectShading shading,
                                        bool batched) {
    auto fail = [](DrawError error, const std::string& message) { return DirectProgramResult{reject(error, message), {}}; };
    auto p = std::make_shared<DirectProgram>();
    const auto tev = capture_tev(state);
    if (!tev) return fail(DrawError::tev, tev.status.message);
    p->tev = tev.material->pipeline;
    const auto indirect = capture_indirect(state, p->tev);
    if (!indirect) return fail(DrawError::tev, indirect.status.message);
    p->indirect = *indirect.snapshot;
    std::uint8_t raster_colors = 0;
    for (unsigned i = 0; i < p->tev.stage_count; ++i) raster_colors |= tev_input_use(p->tev.stages[i]).raster_mask;
    const auto transform = capture_transform_pipeline(state, layout, raster_colors, p->transform);
    if (!transform) return fail(DrawError::transform, transform.message);
    std::uint8_t direct_maps = 0;
    for (unsigned i = 0; i < p->tev.stage_count; ++i) {
        const auto& stage = p->tev.stages[i];
        const auto use = tev_input_use(stage);
        if (use.raster_mask & ~p->transform.computed_colors)
            return fail(DrawError::missing_input, "TEV consumes an unavailable raster color");
        if (!use.texture) continue;
        if (stage.texture_coord >= p->transform.texgen_count)
            return fail(DrawError::missing_input, "TEV consumes an unavailable texture coordinate");
        direct_maps |= 1u << stage.texture_map;
        const auto& s = p->indirect.stages[i];
        if (s.matrix > 3 || s.lookup > 3 || s.bias > 7 || s.wrap_s > 6 || s.wrap_t > 6 ||
            (s.matrix && !(p->indirect.lookup_mask & (1u << s.lookup))))
            return fail(DrawError::unsupported_state, "invalid captured indirect stage");
    }
    if (p->indirect.lookup_mask & ~15u) return fail(DrawError::unsupported_state, "invalid indirect lookup mask");
    p->maps = direct_maps;
    for (unsigned i = 0; i < 4; ++i) if (p->indirect.lookup_mask & (1u << i)) {
        const auto& lookup = p->indirect.lookups[i];
        if (lookup.coordinate >= p->transform.texgen_count)
            return fail(DrawError::missing_input, "indirect lookup consumes an unavailable coordinate");
        if (lookup.shift_s > 15 || lookup.shift_t > 15) return fail(DrawError::unsupported_state, "invalid indirect coordinate shift");
        p->maps |= 1u << lookup.map;
    }
    for (unsigned map = 0; map < 8; ++map) if (p->maps & (1u << map)) {
        const auto filter = texture_filter(state, map);
        if (filter != 0 && filter != 4) p->mip_maps |= 1u << map;
    }
    p->rgba6 = (state.bp(0x43) & 7) == 1;
    p->fast = shading == DirectShading::fast;
    p->early_depth = (state.bp(0x40) & 1) && (state.bp(0x43) & 64);
    if (batched && !p->fast) return fail(DrawError::unsupported_state, "batched programs require fast shading");
    p->batched = batched;
    if (p->fast) {
        p->constant_alpha = p->rgba6 && (state.bp(0x42) & 0x100);
        p->fog = fast_fog(state);
        p->affine = affine_coordinates(state, p->transform);
        const auto vertex = generate_transform_glsl(p->transform, TransformArithmetic::float32_candidate_v1, true, batched, p->affine, true);
        if (!vertex) return fail(DrawError::transform, vertex.status.message);
        p->vertex = vertex.source;
        p->fragment = fast_fragment_source(p->tev, p->indirect, p->maps, p->early_depth, p->constant_alpha, batched, p->affine, p->fog,
                                           wide_registers(state));
        return {{}, std::move(p)};
    }
    if (p->rgba6) {
        const auto pixel = capture_pixel(state, PixelPolicy::candidate_v1);
        if (!pixel) return fail(DrawError::raster, pixel.status.message);
        if (pixel.snapshot->plan().format != PixelFormat::rgba6 || pixel.snapshot->plan().operation == PixelOperation::logic)
            return fail(DrawError::unsupported_state, "RGBA6 renderer requires owned copy/blend/subtract pixel state");
        p->pixel = pixel.snapshot->plan();
        p->needs_fetch = pixel.snapshot->plan().operation != PixelOperation::copy;
        if (p->needs_fetch && fetch != FramebufferFetch::arm && fetch != FramebufferFetch::ext)
            return fail(DrawError::unsupported_state, "RGBA6 blending requires coherent framebuffer fetch");
    }
    const auto vertex = generate_transform_glsl(p->transform, TransformArithmetic::float32_candidate_v1);
    if (!vertex) return fail(DrawError::transform, vertex.status.message);
    const auto shader = generate_tev_shader(p->tev, TevArithmeticModel::fixed8_candidate_v1);
    if (!shader) return fail(DrawError::tev, shader.status.message);
    p->vertex = vertex.source;
    p->fragment = gles_fragment_source(p->tev, shader.functions, p->indirect, p->maps, p->mip_maps, p->rgba6 ? &p->pixel : nullptr,
                                       p->needs_fetch, p->needs_fetch ? fetch : FramebufferFetch::none, p->early_depth,
                                       state.efb().height);
    return {{}, std::move(p)};
}

DrawStatus plan_direct_state(const State& state, const DirectProgram& program, TextureCmprModel cmpr, DirectDrawState& out,
                             GuardBand guard_band) {
    const auto raster = capture_raster(state);
    if (!raster) return reject(DrawError::raster, raster.status.message);
    const auto plan = program.fast ? plan_gles_raster_fast(*raster.snapshot, guard_band)
                                   : plan_gles_raster(*raster.snapshot, program.rgba6, guard_band);
    if (!plan) return reject(DrawError::raster, plan.status.message);
    out.raster = plan.plan;
    if (out.raster.early_depth != program.early_depth) return reject(DrawError::unsupported_state, "early depth differs from program");
    const auto& fog = out.raster.fog;
    if (program.fast && (fog.type | fog.orthographic << 3 | fog.range << 4) != program.fog)
        return reject(DrawError::unsupported_state, "fog differs from program");
    out.tev = capture_tev_uniforms(state);
    if (program.fast) {
        out.pixel_controls = {};
        out.alpha_write = out.raster.alpha_write;
        out.constant_alpha = {float(state.bp(0x42) & 0xff) / 255.0f, out.raster.tev_alpha ? 1.0f : 0.0f, float(out.raster.premultiply)};
    } else if (program.rgba6) {
        const auto pixel = capture_pixel(state, PixelPolicy::candidate_v1);
        if (!pixel) return reject(DrawError::raster, pixel.status.message);
        if ((pixel.snapshot->plan().operation != PixelOperation::copy) != program.needs_fetch)
            return reject(DrawError::unsupported_state, "RGBA6 operation differs from program");
        out.pixel_controls = pixel.snapshot->uniforms();
        out.alpha_write = pixel.snapshot->plan().alpha_write;
    } else {
        out.pixel_controls = {};
        out.alpha_write = false;
    }
    for (unsigned map = 0; map < 8; ++map) if (program.maps & (1u << map)) {
        auto& d = out.textures[map];
        const auto described = describe_texture(state, static_cast<std::uint8_t>(map), cmpr, {}, d);
        if (!described) return reject(DrawError::texture, described.message);
        const auto& sampler = d.sampler;
        const auto filter = static_cast<unsigned>(sampler.min_filter);
        if (!program.fast && (sampler.linear_magnification != ((filter & 4) != 0) || sampler.bias_clamp || sampler.anisotropy != 1))
            return reject(DrawError::unsupported_state, "GLES isotropic sampler requires matching min/mag filters without bias clamp");
        out.sampler_lod[map] = {{sampler.lod_bias_thirtyseconds / 32.0f, sampler.min_lod_sixteenths / 16.0f,
                                 sampler.max_lod_sixteenths / 16.0f, sampler.edge_lod ? 1.0f : 0.0f}};
    }
    auto check_scale = [&](unsigned map, unsigned coordinate) -> const char* {
        const auto& scale = raster.snapshot->coordinates[coordinate];
        const auto& d = out.textures[map];
        if ((scale.words[0] | scale.words[1]) & 0x20000) return "GX cylindrical coordinate wrapping is not implemented";
        const auto automatic_repeat = [](std::uint32_t word, TextureWrap wrap, unsigned s, unsigned size) {
            return !(word & 0x10000) || (wrap == TextureWrap::repeat && s == size && (size & (size - 1)) == 0);
        };
        const auto image = d.bp_words[2];
        if (!automatic_repeat(scale.words[0], d.sampler.wrap_s, scale.s, (image & 1023) + 1) ||
            !automatic_repeat(scale.words[1], d.sampler.wrap_t, scale.t, ((image >> 10) & 1023) + 1))
            return "GX range bias requires the SDK automatic power-of-two repeat mapping";
        return nullptr;
    };
    for (unsigned i = 0; i < program.tev.stage_count; ++i) {
        const auto& stage = program.tev.stages[i];
        if (!tev_input_use(stage).texture) continue;
        if (const auto error = check_scale(stage.texture_map, stage.texture_coord)) return reject(DrawError::unsupported_state, error);
        const auto& d = out.textures[stage.texture_map];
        const auto& scale = raster.snapshot->coordinates[stage.texture_coord];
        // Texture coordinates scale against the size the image register holds; a host extent
        // keeps them normalized to its whole texture.
        const auto image = d.bp_words[2];
        out.coordinate_scale[i] = {{float(scale.s) / ((image & 1023) + 1), float(scale.t) / (((image >> 10) & 1023) + 1)}};
        out.image_size[i] = {{float(d.width), float(d.height)}};
    }
    if (program.indirect.lookup_mask) {
        const auto indirect = capture_indirect(state, program.tev);
        if (!indirect) return reject(DrawError::tev, indirect.status.message);
        for (unsigned i = 0; i < 6; ++i) out.indirect_rows[i] = indirect.snapshot->matrices[i / 2].rows[i % 2];
        for (unsigned i = 0; i < 4; ++i) if (program.indirect.lookup_mask & (1u << i)) {
            const auto& lookup = program.indirect.lookups[i];
            if (const auto error = check_scale(lookup.map, lookup.coordinate)) return reject(DrawError::unsupported_state, error);
            const auto& scale = raster.snapshot->coordinates[lookup.coordinate];
            const auto& d = out.textures[lookup.map];
            out.indirect_scale[i] = {{float(scale.s) * 128, float(scale.t) * 128, 1.0f / (128 * d.width), 1.0f / (128 * d.height)}};
        }
    }
    return {};
}

} // namespace starlet::gx
