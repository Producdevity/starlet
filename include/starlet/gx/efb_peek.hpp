#pragma once

#include <cstdint>

namespace starlet::gx {
// A rectangle of native EFB pixels, top-left origin; an empty one stands for the whole EFB.
struct EfbRegion {
    int x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    bool contains(int px, int py) const { return empty() || (px >= x && px < x + width && py >= y && py < y + height); }
};
class EfbPeekBackend {
public:
    virtual ~EfbPeekBackend() = default;
    // Synchronous reads after prior draws; native EFB coordinates start at top-left.
    // Depth is D24; colour is the stored RGBA8 value as 0xAARRGGBB.
    virtual std::uint32_t peek_depth(int x, int y) = 0;
    virtual std::uint32_t peek_color(int x, int y) = 0;
    // Optional: retain the EFB depth and colour as of all prior draws and return its
    // index, so draw-sync callbacks can run later against it. -1 keeps callbacks
    // synchronous.
    virtual int snapshot_efb() { return -1; }
    // As snapshot_efb, retaining the whole EFB, but only region is read back ahead of the
    // peeks; a peek outside it waits for queued GPU work and reads the retained snapshot.
    virtual int snapshot_efb(const EfbRegion&) { return snapshot_efb(); }
    // Any thread: peeks against the snapshot answer without waiting for queued GPU work.
    virtual bool snapshot_ready(int) const { return true; }
    // Any thread, while the snapshot at index is retained: its value at (x, y) once
    // ready and inside the read-back region, or false, leaving the read to peek_depth or
    // peek_color.
    virtual bool peek_depth_snapshot(int, int, int, std::uint32_t&) const { return false; }
    virtual bool peek_color_snapshot(int, int, int, std::uint32_t&) const { return false; }
    // Directs later peeks to a retained snapshot; -1 restores the live EFB.
    virtual void select_snapshot(int) {}
    // The snapshot at index may be reused; other retained snapshots stay valid.
    virtual void release_snapshot(int) {}
    // Every snapshot taken so far may be reused.
    virtual void release_snapshots() {}
};
} // namespace starlet::gx
