#pragma once

#include "draw_fixture.hpp"

namespace starlet::gx::test {
// JSystem's JFWDisplay::clearEfb(0, 0, 640, 456, color) as the GX SDK writes it: a screen quad
// coloured from TEV register C0 whose depth is replaced by a 4x4 GX_TF_Z24X8 texture holding
// Z = 0xffffff. alpha selects the gameplay display (RGBA6, dither, GXSetDstAlpha(GX_ENABLE, 0));
// title and menu screens use RGB8.
inline void jfw_clear_efb(Fixture& f, std::array<std::uint8_t, 4> color, bool alpha = false) {
    // clearEfb_init: RGBA8 tiles, AR pairs (00, ff) then GB pairs (ff, ff).
    f.memory.bytes.assign(64, 0xff);
    for (unsigned i = 0; i < 32; i += 2) f.memory.bytes[i] = 0;
    // C_MTXOrtho(0, 456, 0, 640, 0, 1); GXSetProjection(GX_ORTHOGRAPHIC).
    const float projection[] = {2.0f / 640, -1, -2.0f / 456, 1, -1, -1};
    for (unsigned i = 0; i < 6; ++i) f.f(0x1020 + i, projection[i]);
    f.xf(0x1026, 1);
    // GXSetViewport(0, 0, 640, 456, 0, 1); GXSetScissor(0, 0, 640, 456).
    const float viewport[] = {320, -228, 16777215, 662, 570, 16777215};
    for (unsigned i = 0; i < 6; ++i) f.f(0x101a + i, viewport[i]);
    f.bp(0x20, (342u << 12) | 342u); f.bp(0x21, ((342u + 639) << 12) | (342u + 455));
    // GXLoadPosMtxImm(e_mtx, GX_PNMTX0) with the identity e_mtx; GXSetCurrentMtx(0), TEXCOORD0 uses GX_IDENTITY.
    for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 4; ++x) f.f(y * 4 + x, x == y ? 1 : 0);
    f.xf(0x1018, 60u << 6);
    // GXSetVtxDesc(GX_VA_POS, GX_DIRECT), (GX_VA_TEX0, GX_DIRECT); POS XY u16, TEX0 ST u8.
    f.cp(0x50, 1u << 9); f.cp(0x60, 1);
    f.cp(0x70, (2u << 1) | (1u << 21));
    // GXSetNumChans(0), both channels from registers, unlit.
    f.xf(0x1009, 0); f.xf(0x100e, 0); f.xf(0x100f, 0); f.xf(0x1010, 0); f.xf(0x1011, 0);
    // GXSetNumTexGens(1); GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_TEX0, GX_IDENTITY, ...).
    f.xf(0x103f, 1); f.xf(0x1040, (1u << 1) | (5u << 7));
    // GXLoadTexObj(GX_TEXMAP0): repeat, GX_NEAR filters, 4x4 RGBA8 (GX_TF_Z24X8 & 15).
    f.bp(0x80, 5); f.bp(0x84, 0); f.bp(0x88, 3u | (3u << 10) | (6u << 20)); f.bp(0x94, 0x8000 >> 5);
    // The SDK scales TEXCOORD0 by the map's size.
    f.bp(0x30, 3); f.bp(0x31, 3);
    // One texgen, no colour channels, GXSetNumTevStages(1), GXSetCullMode(GX_CULL_BACK).
    f.bp(0x00, 1u | (1u << 14));
    // GXSetTevColor(GX_TEVREG0, color).
    f.bp(0xe2, color[0] | (unsigned(color[3]) << 12)); f.bp(0xe3, color[2] | (unsigned(color[1]) << 12));
    // GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL).
    f.bp(0x28, (1u << 6) | (7u << 7));
    // Colour ZERO, ZERO, ZERO, C0 and alpha ZERO, ZERO, ZERO, A0: ADD, clamp, to PREV.
    f.bp(0xc0, 0x80000 | (15u << 12) | (15u << 8) | (15u << 4) | 2u);
    f.bp(0xc1, 0x80000 | (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4));
    // GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0).
    f.bp(0xf3, (7u << 16) | (7u << 19) | (1u << 22));
    // GXSetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0); GXSetZCompLoc(GX_DISABLE).
    f.bp(0xf4, 0); f.bp(0xf5, (2u << 2) | 2u);
    f.bp(0x43, alpha ? 1 : 0);
    // GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP), colour and alpha updates on.
    f.bp(0x41, (alpha ? (1u << 2) : 0u) | (1u << 3) | (1u << 4) | (5u << 12));
    f.bp(0x42, alpha ? 0x100 : 0);
    // GXSetZMode(GX_ENABLE, GX_ALWAYS, GX_ENABLE).
    f.bp(0x40, 1u | (7u << 1) | (1u << 4));
    // GXBegin(GX_QUADS, GX_VTXFMT0, 4): (x, y) u16 and (s, t) u8 per vertex.
    f.vertices.clear();
    const unsigned corners[4][4] = {{0, 0, 0, 0}, {640, 0, 1, 0}, {640, 456, 1, 1}, {0, 456, 0, 1}};
    for (const auto& c : corners) {
        f.vertices.insert(f.vertices.end(), {std::uint8_t(c[0] >> 8), std::uint8_t(c[0]), std::uint8_t(c[1] >> 8),
                                             std::uint8_t(c[1]), std::uint8_t(c[2]), std::uint8_t(c[3])});
    }
}
} // namespace starlet::gx::test
