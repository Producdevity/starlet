#include "starlet/gx/decoder.hpp"
#include "starlet/gx/raster.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace starlet::gx;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Memory : MemoryReader {
    bool copy(std::uint32_t, std::uint8_t*, std::size_t) override { return false; }
};
struct Sink : CommandSink {
    bool draw(const State&, const Draw&, MemoryReader&) override { return true; }
    bool bp_write(const State&, std::uint8_t, std::uint32_t) override { return true; }
    bool invalidate_vertices() override { return true; }
};
struct Fixture {
    Memory memory;
    Sink sink;
    Decoder decoder;
    explicit Fixture(EfbLayout efb = {}) : decoder(memory, sink, {}, efb) { defaults(efb); }
    void bp(unsigned reg, std::uint32_t word) {
        check(bool(decoder.write_u8(0x61)), "BP opcode");
        check(bool(decoder.write_u32((reg << 24) | word)), "BP payload");
    }
    void xf(unsigned reg, float value) {
        check(bool(decoder.write_u8(0x10)), "XF opcode");
        check(bool(decoder.write_u32(reg)), "XF header");
        check(bool(decoder.write_f32(value)), "XF payload");
    }
    // A full-frame viewport and scissor for the layout's frame.
    void defaults(EfbLayout efb) {
        const float half = efb.height / 2.0f;
        const float viewport[] = {320, -half, 16777216, 662, 342 + half, 16777216};
        for (unsigned i = 0; i < 6; ++i) xf(0x101a + i, viewport[i]);
        bp(0x20, 0x156156); bp(0x21, (981u << 12) | (342u + efb.height - 1)); bp(0x59, 0x02acab);
        bp(0x00, 0x11); bp(0x40, 0x06); bp(0x41, 0x00f4a9);
        bp(0x42, 0); bp(0x43, 0); bp(0x44, 3); bp(0x68, 0);
        bp(0xf3, 0x3f0000);
    }
    RasterCapture capture() { return capture_raster(decoder.state()); }
};
void rect(const RasterRect& r, int x, int y, int w, int h) {
    check(r.x == x && r.y == y && r.width == w && r.height == h, "rectangle mismatch");
}
void baseline() {
    Fixture f;
    const auto c = f.capture(); check(bool(c), "baseline capture");
    const auto p = plan_gles_raster(*c.snapshot); check(bool(p), "baseline GLES plan");
    rect(p.plan.viewport, 0, 0, 640, 456); rect(p.plan.scissor, 0, 0, 640, 456);
    check(p.plan.blend && p.plan.color_write && p.plan.source_factor == 4 && p.plan.destination_factor == 5,
          "original font source-alpha blend");
    check(c.snapshot->viewport.xf[2] == 16777216 && c.snapshot->viewport.xf[5] == 16777216,
          "original GX depth units remain 2^24");
    f.bp(0x41, 0x00f4b9);
    check(bool(plan_gles_raster(*f.capture().snapshot)), "RGB8 has no alpha plane to update");
    f.bp(0x41, 0x00f4bd);
    check(bool(plan_gles_raster(*f.capture().snapshot)), "RGB8 dither enable is ineffective");
    f.bp(0x59, 0x02b0b0); // Physical offset 352,344 instead of342,342.
    const auto shifted = f.capture(); check(bool(shifted), "offset capture");
    check(shifted.snapshot->viewport.left == -10 && shifted.snapshot->viewport.top == -2,
          "quad offset translates viewport as well as scissor");
    rect(shifted.snapshot->scissor, 0, 2, 630, 454);
    const auto shifted_plan = plan_gles_raster(*shifted.snapshot);
    check(bool(shifted_plan) && !shifted_plan.plan.strip, "small offsets plan as a translation");
    rect(shifted_plan.plan.viewport, -10, 2, 640, 456);
    // FullScreenBlur: GXSetScissorBoxOffset(0, 1584) puts screen row 0 on EFB row 464.
    f.bp(0x59, (963u << 10) | 171u);
    const auto blur = f.capture(); check(bool(blur) && blur.snapshot->strip, "blur offset wraps below the frame");
    check(blur.snapshot->viewport.left == 0 && blur.snapshot->viewport.top == 8, "viewport in strip rows");
    rect(blur.snapshot->scissor, 0, 0, 640, 64);
    const auto blur_plan = plan_gles_raster(*blur.snapshot);
    check(bool(blur_plan) && blur_plan.plan.strip, "strip draw plans");
    rect(blur_plan.plan.viewport, 0, -392, 640, 456); rect(blur_plan.plan.scissor, 0, 0, 640, 64);
    f.bp(0x20, 0x156000); f.bp(0x21, 0x3d57ff);
    check(!f.capture(), "a scissor reaching both sides of the wrap fails explicitly");
    f.bp(0x20, 0x156156); f.bp(0x21, 0x3d531d); f.bp(0x59, 0x02acab);
    rect(p.plan.viewport, 0, 0, 640, 456); rect(c.snapshot->scissor, 0, 0, 640, 456);
    check(c.snapshot->state_revision != shifted.snapshot->state_revision, "owned revision");
}
void culling() {
    for (unsigned mode=0;mode<4;++mode) {
        Fixture f; f.bp(0x00,(mode<<14)|0x11);const auto captured=f.capture();
        const auto plan=plan_gles_raster(*captured.snapshot);
        check(bool(plan) && plan.plan.cull==mode,"all hardware face-rejection modes retained");
    }
}
void depth() {
    for (unsigned mode=0;mode<32;++mode) {
        Fixture f;f.bp(0x40,mode);f.bp(0x43,64);const auto captured=f.capture();
        const auto plan=plan_gles_raster(*captured.snapshot);
        check(bool(plan) && plan.plan.depth_test==bool(mode&1) && plan.plan.depth_write==bool(mode&16) &&
            plan.plan.depth_compare==((mode>>1)&7) && plan.plan.early_depth==bool(mode&1),"all depth modes and early ordering retained");
    }
    Fixture f;f.xf(0x101c,8388608);f.xf(0x101f,12582912);
    auto captured=f.capture();auto plan=plan_gles_raster(*captured.snapshot);
    check(bool(plan) && plan.plan.depth_range==std::array<float,2>{{.25f,.75f}},"original viewport depth units map to normalized near/far");
    f.xf(0x101f,33554432);captured=f.capture();check(!plan_gles_raster(*captured.snapshot),"out-of-range depth viewport rejected");
    // Freezeflame's star select: far just past the 24-bit maximum clamps like GX's per-pixel depth.
    f.xf(0x101c,16777200);f.xf(0x101f,16777400);captured=f.capture();plan=plan_gles_raster(*captured.snapshot);
    check(bool(plan) && plan.plan.depth_range[1]==1.0f && plan.plan.depth_range[0]>0 && plan.plan.depth_range[0]<1e-4f,
          "a depth range within 1/256 of [0,1] is clamped");
    Fixture texture;texture.bp(0xf5,10);texture.bp(0x40,15);captured=texture.capture();
    check(bool(plan_gles_raster(*captured.snapshot)),"always-pass read-only depth is independent of Z texture");
    texture.bp(0x40,7);captured=texture.capture();check(!plan_gles_raster(*captured.snapshot),"Z-texture comparison explicitly unsupported");
    texture.bp(0x40,31);captured=texture.capture();check(!plan_gles_raster(*captured.snapshot),"Z-texture writes explicitly unsupported");
}
void viewport_and_scissor() {
    Fixture f;
    // Upper-left 100,50; 200x100 in original top-left EFB coordinates.
    f.xf(0x101a, 100); f.xf(0x101b, -50); f.xf(0x101d, 542); f.xf(0x101e, 442);
    f.bp(0x20, (442u << 12) | 392u); f.bp(0x21, (641u << 12) | 491u);
    auto c = f.capture(); auto p = plan_gles_raster(*c.snapshot);
    check(bool(p), "subviewport plan"); rect(p.plan.viewport, 100, 306, 200, 100);
    rect(p.plan.scissor, 100, 306, 200, 100);
    // A single lower-right pixel uses inclusive GX end coordinates.
    f.bp(0x20, (641u << 12) | 491u); c = f.capture(); p = plan_gles_raster(*c.snapshot);
    check(bool(p), "one-pixel scissor"); rect(p.plan.scissor, 299, 306, 1, 1);
    f.bp(0x20, (642u << 12) | 492u); c = f.capture(); p = plan_gles_raster(*c.snapshot);
    check(bool(p), "inverted rectangle is empty"); rect(p.plan.scissor, 300, 306, 0, 0);
    f.bp(0x20, 0x156156); f.bp(0x21, 0x3d531d); c = f.capture();
    check(!plan_gles_raster(*c.snapshot), "guard-band region may not silently disappear");
    f.xf(0x101a, 0); c = f.capture(); check(!plan_gles_raster(*c.snapshot), "zero viewport unsupported");
    f.xf(0x101a, -1); c = f.capture(); check(!plan_gles_raster(*c.snapshot), "negative width unsupported");
    f.xf(0x101a, 320); f.xf(0x101d, 662.25f); c = f.capture();
    check(!plan_gles_raster(*c.snapshot), "fractional jitter not rounded away");
    for (float v : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()}) {
        f.xf(0x101c, v); c = f.capture();
        check(!c && !c.snapshot && c.status.error == RasterError::invalid_viewport, "nonfinite fails atomically");
    }
}
void coverage_oracle() {
    Fixture f;
    std::uint32_t random = 0x572ac9;
    auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    for (unsigned trial = 0; trial < 2000; ++trial) {
        const int x0 = next() & 2047, y0 = next() & 2047;
        const int x1 = next() & 2047, y1 = next() & 2047;
        const int qx = (next() & 1023) * 2, qy = (next() & 1023) * 2;
        f.bp(0x20, (unsigned(x0) << 12) | unsigned(y0));
        f.bp(0x21, (unsigned(x1) << 12) | unsigned(y1));
        f.bp(0x59, (unsigned(qy / 2) << 10) | unsigned(qx / 2));
        // A pixel e is drawn when (e + offset) mod 2048 is inside the scissor.
        const auto inside = [](int e, int first, int last, int q) { const int s = (e + q) & 2047; return s >= first && s <= last; };
        const auto split = [](int first, int last, int q, int extent) { return first <= last && last - q >= 0 && first - q + 2048 < extent; };
        const auto c = f.capture();
        if (!c) { check(split(x0, x1, qx, 640) || split(y0, y1, qy, 528), "only scissors split by the wrap fail"); continue; }
        const auto& r = c.snapshot->scissor;
        const bool strip = c.snapshot->strip;
        const int rows = strip ? 72 : 456, first_row = strip ? 456 : 0;
        check(r.x >= 0 && r.y >= 0 && r.width >= 0 && r.height >= 0 &&
              r.x + r.width <= 640 && r.y + r.height <= rows, "scissor bounded to its target");
        for (unsigned sample = 0; sample < 64; ++sample) {
            const int x = next() % 640, y = first_row + int(next() % unsigned(rows));
            const bool original = inside(x, x0, x1, qx) && inside(y, y0, y1, qy);
            const int gl_y = rows - 1 - (y - first_row);
            const bool captured = x >= r.x && x < r.x + r.width && gl_y >= r.y && gl_y < r.y + r.height;
            check(original == captured, "independent original-coordinate pixel membership");
        }
    }
}
void preserved_state_and_gates() {
    const std::pair<unsigned, unsigned> unsupported[] = {
        {0x00,0x111},{0x00,0x211},{0x00,0x80011},
        {0x40,0x20},{0x41,0xf4ab},{0x41,0xfca9},
        {0x41,0xf6a9},{0x41,0xf4c9},{0x42,0x100},{0x42,0x200},
        {0x43,1},{0x43,2},{0x43,8},{0x44,1},{0x44,2},{0x68,1},
        {0x22,1u<<22},{0xe8,1u<<10},{0xf1,2u<<21}
    };
    for (auto [reg, word] : unsupported) {
        Fixture f; f.bp(reg, word); const auto c = f.capture();
        check(bool(c), "unsupported state is still captured");
        check(!plan_gles_raster(*c.snapshot), "unsupported effect must not be ignored");
    }
    Fixture f;
    f.bp(0x30, 0x300ff); f.bp(0x31, 0x3007f);
    f.bp(0x3e, 0xffff); f.bp(0x3f, 0xffff);
    f.bp(0xee, 0x123456); f.bp(0xef, 0xabcdef); f.bp(0xf2, 0x112233);
    const auto c = f.capture();
    check(c.snapshot->coordinates[0].s == 256 && c.snapshot->coordinates[0].t == 128,
          "SU scale is size plus one");
    check(c.snapshot->coordinates[0].words[0] == 0x300ff && c.snapshot->coordinates[0].words[1] == 0x3007f,
          "coordinate modifier flags retained for sampling policy");
    check(c.snapshot->coordinates[7].s == 65536 && c.snapshot->coordinates[7].t == 65536,
          "maximum coordinate scale not truncated to16bits");
    check(c.snapshot->fog[6] == 0x123456 && c.snapshot->fog[7] == 0xabcdef && c.snapshot->fog[10] == 0x112233,
          "inactive fog parameters preserved");
}
void fast_fog() {
    // Perspective linear fog registers of a draw recorded on the Miyoo Flip.
    Fixture f;
    f.bp(0xe8, 0x156); f.bp(0xee, 0x3ad9e); f.bp(0xef, 0x40021b); f.bp(0xf0, 0x2); f.bp(0xf1, 0x43bd9d); f.bp(0xf2, 0xffffff);
    auto c = f.capture(); auto p = plan_gles_raster_fast(*c.snapshot);
    check(bool(p), "fast domain admits fog");
    const auto& fog = p.plan.fog;
    check(fog.type == 2 && !fog.orthographic && !fog.range, "perspective linear fog without range adjustment");
    check(std::abs(fog.parameters[0] - 0.0016622543f) < 1e-9f && fog.parameters[1] == 4194843 / 16777216.0f &&
          fog.parameters[2] == 0.25f && std::abs(fog.parameters[3] - 0.0066471100f) < 1e-9f, "fog A, B, shift and C decode");
    check(fog.color == std::array<float, 3>{{1, 1, 1}}, "fog color");
    check(!plan_gles_raster(*c.snapshot), "exact domain still rejects fog");
    // The center register holds center + 342; table entries are 12-bit 8.8 factors.
    f.bp(0xe8, (1u << 10) | (342 + 20)); f.bp(0xe9, (0x200u << 12) | 0x100);
    c = f.capture(); p = plan_gles_raster_fast(*c.snapshot);
    check(p && p.plan.fog.range && p.plan.fog.center == 20 && p.plan.fog.table[0] == 1.0f && p.plan.fog.table[1] == 2.0f,
          "range adjustment center and table decode");
    f.bp(0xf1, 1u << 21); c = f.capture();
    check(!plan_gles_raster_fast(*c.snapshot), "unknown fog type rejected");
}
void fast_constant_alpha() {
    // Constant alpha only replaces the stored alpha; colour blends with the TEV alpha.
    Fixture f;
    f.bp(0x43, 1); f.bp(0x42, 0x100);
    const auto plan = [&](std::uint32_t color_mode) {
        f.bp(0x41, color_mode); const auto c = f.capture(); auto p = plan_gles_raster_fast(*c.snapshot);
        check(bool(p), "fast constant alpha plan"); return p.plan;
    };
    constexpr std::uint32_t blend = 1, color = 8, alpha = 16;
    auto p = plan(blend | color | alpha | (4u << 8) | (1u << 5));
    check(p.premultiply == 1 && p.source_factor == 1 && p.destination_factor == 1 && p.alpha_factors == 1 && !p.tev_alpha,
          "source-alpha source factor applied by the fragment stage, constant stored");
    p = plan(blend | color | alpha | (5u << 8) | (7u << 5));
    check(p.premultiply == 2 && p.source_factor == 1 && p.destination_factor == 7 && p.alpha_factors == 1,
          "inverse source alpha with a destination-alpha factor");
    p = plan(blend | color | alpha | (4u << 8) | (5u << 5));
    check(!p.premultiply && p.source_factor == 4 && p.destination_factor == 5 && p.tev_alpha && p.alpha_factors == 2,
          "a destination factor reading source alpha keeps TEV alpha and zeroes stored alpha");
    p = plan(blend | color | (4u << 8) | (5u << 5));
    check(!p.premultiply && p.tev_alpha && !p.alpha_factors && !p.alpha_write, "without alpha writes only TEV alpha matters");
    p = plan(blend | color | alpha | (1u << 8) | (1u << 5));
    check(!p.premultiply && !p.tev_alpha && p.alpha_factors == 1, "factors without source alpha store the constant");
    p = plan(color | alpha | (1u << 11));
    check(p.subtract && !p.tev_alpha && p.alpha_factors == 1, "subtraction stores the constant");
    p = plan(color | alpha | (4u << 8) | (1u << 5));
    check(!p.blend && !p.premultiply && !p.tev_alpha && !p.alpha_factors, "no blending: the fragment writes the constant");
    f.bp(0x42, 0);
    p = plan(blend | color | alpha | (4u << 8) | (1u << 5));
    check(!p.premultiply && p.source_factor == 4 && !p.alpha_factors && !p.tev_alpha, "constant alpha disabled");
}
}
// A 640x480 frame (NTSC GameCube render modes) moves the frame/strip boundary to row 480.
void frame_layouts() {
    Fixture ntsc(EfbLayout{480});
    auto c = ntsc.capture(); check(bool(c), "480-row capture");
    auto p = plan_gles_raster(*c.snapshot); check(bool(p), "480-row plan");
    rect(p.plan.viewport, 0, 0, 640, 480); rect(p.plan.scissor, 0, 0, 640, 480);
    check(!p.plan.strip, "a 480-row frame is not a strip");
    for (const int height : {456, 480}) {
        Fixture f(EfbLayout{height});
        f.bp(0x20, (342u << 12) | (342u + 456)); f.bp(0x21, (981u << 12) | (342u + 479));
        c = f.capture(); check(bool(c), "rows 456..479 capture");
        check(c.snapshot->strip == (height == 456), "rows 456..479 are strip rows only below a 456-row frame");
        if (height == 480) rect(c.snapshot->scissor, 0, 0, 640, 24);
    }
    ntsc.bp(0x20, (342u << 12) | (342u + 480)); ntsc.bp(0x21, (981u << 12) | (342u + 527));
    c = ntsc.capture(); check(bool(c) && c.snapshot->strip, "rows below a 480-row frame are strip rows");
    rect(c.snapshot->scissor, 0, 0, 640, 48);
    for (const int height : {0, 457, 530}) {
        bool rejected = false;
        try { State state(EfbLayout{height}); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid frame heights are rejected");
    }
}

// A scissor wider than the viewport, as Twilight Princess sets for its 608x448 frame.
void guard_band() {
    Fixture f(EfbLayout{448});
    const float viewport[] = {304, -224, 16777216, 646, 566, 16777216};
    for (unsigned i = 0; i < 6; ++i) f.xf(0x101a + i, viewport[i]);
    f.bp(0x20, (342u << 12) | 342u); f.bp(0x21, ((342u + 639) << 12) | (342u + 479));
    const auto c = f.capture(); check(bool(c), "wide scissor capture");
    check(!plan_gles_raster(*c.snapshot), "the guard band is refused by default");
    const auto p = plan_gles_raster(*c.snapshot, false, GuardBand::clip_to_viewport);
    check(bool(p), "clip_to_viewport plans the draw");
    rect(p.plan.viewport, 0, 0, 608, 448); rect(p.plan.scissor, 0, 0, 608, 448);
    check(bool(plan_gles_raster_fast(*c.snapshot, GuardBand::clip_to_viewport)), "fast planning clips the same way");
}

int main() {
    try {
        frame_layouts();
        guard_band();
        baseline(); culling(); depth(); viewport_and_scissor(); coverage_oracle(); preserved_state_and_gates(); fast_fog();
        fast_constant_alpha();
        std::cout << "GX raster tests passed: 128000 original-coordinate pixel membership checks\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
