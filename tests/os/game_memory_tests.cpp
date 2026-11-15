// A port's executable image keeps its link address: under window_base 0x80000000 its physical
// addresses resolve back to the image, and game memory may not lie where they would point.
#include "starlet/os/game_memory.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

alignas(32) unsigned char static_buffer[256];

}

int main() {
    namespace os = starlet::os;
    std::uintptr_t image_begin = 0, image_end = 0, alias_begin = 0, alias_end = 0;
    os::image_range(image_begin, image_end);
    expect(image_end <= 0x40000000u, "the test image links low, like a port");

    os::set_memory_map({0});
    os::image_alias(alias_begin, alias_end);
    expect(alias_begin == alias_end, "window_base 0: the image sits at its own physical addresses");

    os::set_memory_map({0x80000000u});
    os::image_alias(alias_begin, alias_end);
    expect(alias_begin == 0x80000000u + image_begin && alias_end == 0x80000000u + image_end,
           "window_base 0x80000000: the image's physical addresses would name window_base + p");

    const auto native = reinterpret_cast<std::uintptr_t>(static_buffer);
    const std::uint32_t physical = os::native_to_physical(static_buffer);
    expect(physical == (std::uint32_t(native) - 0x80000000u & 0x3FFFFFFF), "the physical address of a static");
    expect(os::physical_to_native(physical) == static_buffer, "a static's physical address resolves to the static");
    expect(os::physical_to_native(physical | 0x80000000u) == static_buffer, "and its cached form");
    expect(os::physical_to_native(physical & (os::gx_window_size - 1)) == static_buffer,
           "and the GX copy register's 64 MiB form");
    expect(os::physical_to_native(0x01000000u) == reinterpret_cast<void*>(0x81000000u),
           "other physical addresses stay window_base + p");

    expect(throws([&] { os::set_arena(reinterpret_cast<void*>(0x80004000u), std::size_t{64} << 20); }),
           "an arena over the image's physical addresses fails");
    expect(throws([&] { os::map_fixed(alias_end - 0x1000, 0x2000, "test region"); }),
           "a fixed region over the image's physical addresses fails");
    expect(!throws([&] { os::set_arena(reinterpret_cast<void*>(alias_end), 0x1000); }), "an arena above them is accepted");

    // The SDK's OSCachedToPhysical form, as the AR and AI take it.
    expect(os::hardware_to_native(std::uint32_t(native) - 0x80000000u, sizeof static_buffer) == static_buffer,
           "a static's physical form reaches the static");
    expect(os::hardware_to_native(std::uint32_t(native), sizeof static_buffer) == static_buffer,
           "a static's pointer reaches the static");

    std::printf("%s\n", failures ? "game_memory: failures" : "game_memory: ok");
    return failures ? 1 : 0;
}
