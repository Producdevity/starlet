#include "jfw_clear_fixture.hpp"
#include "starlet/gx/direct.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace starlet::gx;
using namespace starlet::gx::test;
namespace {
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
DirectProgramResult direct(Fixture& f, DirectShading shading, bool batched = false) {
    return plan_direct_program(f.decoder.state(), f.command().layout, FramebufferFetch::none, shading, batched);
}
DirectDrawState direct_state(Fixture& f, const DirectProgram& program) {
    DirectDrawState state;
    const auto status = plan_direct_state(f.decoder.state(), program, TextureCmprModel::reject_unverified, state);
    check(bool(status), status.message.c_str());
    return state;
}
std::vector<std::uint32_t> words(Fixture& f, DirectShading shading) {
    std::vector<std::uint32_t> out;
    direct_program_words(f.decoder.state(), f.command().layout, FramebufferFetch::none, shading, out);
    return out;
}

void jfw_clear() {
    for (const bool alpha : {false, true}) {
        Fixture f; jfw_clear_efb(f, {{0x20, 0x40, 0x60, 0xff}}, alpha);
        const auto captured = f.capture(); const auto& draw = packet(captured);
        check(draw.texture_mask == 1 && draw.tev->pipeline.z_texture_op == 2 && draw.tev->pipeline.z_texture_type == 2 &&
              draw.tev->uniforms.z_texture_bias == 0, "the clear captures its Z24X8 lookup and Z texture");
        check(draw.tev->uniforms.registers[1] == TevSignedColor{{0x20, 0x40, 0x60, 0xff}}, "colour from C0");
        const auto& texels = draw.textures[0]->levels()[0].rgba;
        check(texels.size() == 64, "4x4 texels");
        for (std::size_t i = 0; i < texels.size(); i += 4)
            check(z_texture_depth({2, 2, 0}, {{texels[i], texels[i + 1], texels[i + 2], texels[i + 3]}}, 0) == 0xffffff,
                  "every texel replaces Z with the far plane");

        const auto planned = plan_gles_draw(draw); const auto& gpu = pipeline(planned);
        check(gpu.raster.depth_test && gpu.raster.depth_write && gpu.raster.depth_compare == 7 && !gpu.raster.early_depth,
              "ALWAYS test with writes, after texturing");
        check(!gpu.full_replacement, "a depth-writing draw is no colour-only replacement");
        const auto& frag = gpu.fragment;
        check(has(frag, "uniform highp uint uGxZBias;") && has(frag, "highp uvec4 gxZTexel=uvec4(samples[0]);") &&
              has(frag, "gxZ=((gxZTexel.r<<16u|gxZTexel.g<<8u|gxZTexel.b)+uGxZBias)&16777215u;") &&
              has(frag, "gl_FragDepth=float(gxZ)/16777216.0;") && has(frag, "samples[0]=gxQuantize(texture(uGxTexture0,"),
              "replace writes the Z24X8 texel as fragment depth");
        check(!has(frag, "early_fragment_tests") && !has(frag, "gxZReference"), "replace reads no rasterized Z");
        check(frag.find("gl_FragDepth") > frag.find("discard;"), "depth is written after the alpha test");

        // Direct exact path: the same program and per-draw state.
        const auto exact = direct(f, DirectShading::exact);
        check(bool(exact), exact.status.message.c_str());
        check(exact.program->fragment == frag && exact.program->vertex == gpu.vertex && exact.program->maps == 1, "direct exact parity");
        check(exact.program->z_texture.op == 2 && exact.program->z_texture.type == 2 && exact.program->z_texture.depth &&
              !exact.program->early_depth, "direct exact Z-texture program");
        const auto exact_state = direct_state(f, *exact.program);
        check(exact_state.coordinate_scale[0] == gpu.coordinate_scale[0] && exact_state.tev.z_texture_bias == 0 &&
              !exact_state.raster.early_depth, "direct exact state");

        // Fast shading, the Pikmin 2 configuration, and its batched variant for small draws.
        for (const bool batched : {false, true}) {
            const auto fast = direct(f, DirectShading::fast, batched);
            check(bool(fast), fast.status.message.c_str());
            const auto& source = fast.program->fragment;
            check(fast.program->maps == 1 && fast.program->constant_alpha == alpha, "fast maps and constant alpha");
            check(has(source, "gxZLookup=ivec4(texture(uGxTexture0,") && has(source, "highp uvec4 gxZTexel=uvec4(gxZLookup);") &&
                  has(source, "gl_FragDepth=float(gxZ)/16777216.0;") && !has(source, "early_fragment_tests"),
                  "fast program writes the Z texel as fragment depth");
            const auto state = direct_state(f, *fast.program);
            check(state.coordinate_scale[0][0] == 1 && state.coordinate_scale[0][1] == 1, "fast coordinate scale of the 4x4 map");
            State rebuilt; VertexLayout layout;
            check(direct_program_state(words(f, DirectShading::fast), rebuilt, layout), "Z-texture program words rebuild");
            const auto again = plan_direct_program(rebuilt, layout, FramebufferFetch::none, DirectShading::fast, batched);
            check(again && again.program->fragment == source, "rebuilt Z-texture program matches");
        }
    }
}

void ops_types_and_bias() {
    const char* lanes[3] = {"gxZ=(gxZTexel.a+uGxZBias", "gxZ=((gxZTexel.a<<8u|gxZTexel.r)+uGxZBias",
                            "gxZ=((gxZTexel.r<<16u|gxZTexel.g<<8u|gxZTexel.b)+uGxZBias"};
    std::vector<std::vector<std::uint8_t>> keys;
    std::vector<std::vector<std::uint32_t>> fast_words;
    for (unsigned op = 1; op < 3; ++op) for (unsigned type = 0; type < 3; ++type) {
        Fixture f; jfw_clear_efb(f, {{0, 0, 0, 0}});
        f.bp(0xf5, (op << 2) | type); f.bp(0xf4, 0x10203);
        const auto captured = f.capture(); const auto& draw = packet(captured);
        check(draw.tev->uniforms.z_texture_bias == 0x10203, "bias captured");
        const auto planned = plan_gles_draw(draw); const auto& gpu = pipeline(planned);
        check(has(gpu.fragment, lanes[type]), "type selects the texel lanes");
        check(has(gpu.fragment, "dFdx(gl_FragCoord.z)") == (op == 1) && has(gpu.fragment, "+uGxZBias+gxZReference)") == (op == 1),
              "add offsets the quad's reference Z");
        check(std::find(keys.begin(), keys.end(), gpu.key) == keys.end(), "op and type are program identity");
        keys.push_back(gpu.key);
        const auto fast = direct(f, DirectShading::fast);
        check(fast && has(fast.program->fragment, lanes[type]) &&
              has(fast.program->fragment, "gxZReference") == (op == 1), "fast program follows op and type");
        check(direct_state(f, *fast.program).tev.z_texture_bias == 0x10203, "bias is per-draw state");
        const auto w = words(f, DirectShading::fast), exact = words(f, DirectShading::exact);
        check(std::find(fast_words.begin(), fast_words.end(), w) == fast_words.end(), "op and type are direct program words");
        fast_words.push_back(w);
        f.bp(0xf4, 0xfedcba);
        check(words(f, DirectShading::fast) == w && words(f, DirectShading::exact) == exact, "the bias is no program word");
    }
}

void depth_and_fog_gates() {
    Fixture f; jfw_clear_efb(f, {{0, 0, 0, 0}});
    f.bp(0x40, 0); // Depth test off: the Z texture changes nothing.
    auto captured = f.capture(); auto planned = plan_gles_draw(packet(captured));
    const auto off_key = pipeline(planned).key;
    check(!has(planned.pipeline.fragment, "gl_FragDepth") && !has(planned.pipeline.fragment, "uGxZBias"),
          "no Z code without a depth test");
    auto fast = direct(f, DirectShading::fast);
    check(fast && !has(fast.program->fragment, "gxZ") && !fast.program->z_texture.depth, "fast: no Z code without depth or fog");
    f.bp(0xf1, 2u << 21); // Linear fog reads the Z-texture depth.
    fast = direct(f, DirectShading::fast);
    check(fast && has(fast.program->fragment, "highp float gxDepth=float(gxZ)/16777216.0;") &&
          has(fast.program->fragment, "gxDepth*uGxFog.z") && !has(fast.program->fragment, "gl_FragCoord.z*uGxFog.z") &&
          !has(fast.program->fragment, "gl_FragDepth"), "fog uses the Z texture's Z");
    f.bp(0xf1, 0); f.bp(0x40, 7); f.bp(0x43, 64); // Early placement still tests late.
    captured = f.capture(); planned = plan_gles_draw(packet(captured));
    check(!pipeline(planned).raster.early_depth && !has(planned.pipeline.fragment, "early_fragment_tests") &&
          has(planned.pipeline.fragment, "gl_FragDepth") && planned.pipeline.key != off_key, "Z textures test depth late");
    f.bp(0xf5, 2); // Disabled: the stage's lookup is unused again.
    captured = f.capture();
    check(bool(captured) && !captured.packet->texture_mask && !captured.packet->tev->uniforms.z_texture_bias, "disabled Z texture");
    planned = plan_gles_draw(packet(captured));
    check(pipeline(planned).raster.early_depth && !has(planned.pipeline.fragment, "gxZ"), "disabled: early depth, no Z code");
}

void swaps_and_rejections() {
    // The Z texel bypasses the stage's swap table.
    Fixture f; jfw_clear_efb(f, {{0, 0, 0, 0}});
    f.bp(0xc1, 0x80000 | (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 2)); f.bp(0xf8, 0); f.bp(0xf9, 0);
    const auto captured = f.capture();
    check(packet(captured).tev->pipeline.stages[0].texture_swap == std::array<std::uint8_t, 4>{{0, 0, 0, 0}}, "swap table 1 is RRRR");
    check(has(pipeline(plan_gles_draw(packet(captured))).fragment, "gxZTexel=uvec4(samples[0]);"), "exact texel before the swap");
    const auto fast = direct(f, DirectShading::fast);
    check(fast && has(fast.program->fragment, "gxZLookup=ivec4(texture(uGxTexture0,(gx_tex0.xy/gx_tex0.z)*uGxCoordinateScale[0],"
                                              "uGxSamplerBias[0])*255.0+0.5);"), "fast texel before the swap");
    {
        Fixture g; jfw_clear_efb(g, {{0, 0, 0, 0}}); g.bp(0xc0, 0x8fff8);
        const auto c = g.capture();
        check(!c && c.status.message == "the Z texture occupies the last TEV stage's texture input", "the Z texel is no TEV input");
        check(!direct(g, DirectShading::fast), "direct programs reject it too");
    }
    {
        Fixture g; jfw_clear_efb(g, {{0, 0, 0, 0}}); g.bp(0x28, 7u << 7);
        check(!g.capture() && !direct(g, DirectShading::fast), "a Z texture needs the last stage's lookup");
    }
    {
        Fixture g; jfw_clear_efb(g, {{0, 0, 0, 0}}); g.bp(0xf5, 12);
        check(!g.capture() && !direct(g, DirectShading::fast) && !direct(g, DirectShading::exact), "reserved op rejected");
    }
}
} // namespace

int main() {
    try {
        jfw_clear(); ops_types_and_bias(); depth_and_fog_gates(); swaps_and_rejections();
        std::cout << "GX Z-texture tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
