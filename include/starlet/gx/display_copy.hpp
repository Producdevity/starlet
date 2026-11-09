#pragma once

#include "starlet/gx/efb_clear.hpp"
#include <optional>
#include <vector>

namespace starlet::gx {
struct DisplayCopyRegisters {
    std::uint32_t source, size, stride, command;
};
struct DisplayCopy {
    EfbLayout efb;
    int x = 0, y = 0, width = 0, height = 0;
    std::size_t stride = 0;
    std::array<unsigned, 3> filter{};
    bool clamp_top = false, clamp_bottom = false, rgba6 = false;
    std::optional<EfbClear> clear;
};
struct FramebufferView {
    ByteView bytes;
    unsigned width = 0, height = 0;
    std::size_t stride = 0;
};
DisplayCopy plan_display_copy(const State &, DisplayCopyRegisters, bool clear);
std::size_t display_copy_bytes(const DisplayCopy &);
void validate_framebuffer(const FramebufferView &);
// Reference arithmetic; RGBA EFB rows run bottom-to-top, XFB rows top-to-bottom.
std::vector<std::uint8_t> copy_display_reference(const DisplayCopy &, ByteView rgba);
std::vector<std::uint8_t> decode_framebuffer_reference(const FramebufferView &);
class DisplayCopyBackend {
  public:
    virtual ~DisplayCopyBackend() = default;
    virtual void copy_display(const DisplayCopy &, std::uint8_t *destination, std::size_t capacity) = 0;
};
} // namespace starlet::gx
