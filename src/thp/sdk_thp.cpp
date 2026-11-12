// The SDK's THPInit, THPVideoDecode and THPAudioDecode on starlet::thp, for ports whose player is
// the SDK's. Frames and audio records are as read from the disc.
#include "starlet/os/os_core.hpp"
#include "starlet/thp/thp.hpp"
#include <cstdio>

namespace {
constexpr std::size_t packet_limit = 4 * 1024 * 1024;
starlet::thp::VideoDecoder* decoder() {
    static starlet::thp::VideoDecoder instance;
    return &instance;
}
}

extern "C" {
starlet::sdk::BOOL THPInit() {
    decoder();
    return 1;
}

starlet::sdk::s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    if (!file || !tileY || !tileU || !tileV || !work) return 1;
    // Like the SDK's, the call gets no frame length: the frame ends with its scan.
    const auto* frame = static_cast<const std::uint8_t*>(file);
    unsigned width = 0, height = 0;
    if (!starlet::thp::frame_size(frame, packet_limit, width, height)) return 1;
    const std::size_t luma = std::size_t(width) * height;
    starlet::os::before_dma_write(tileY, luma);
    starlet::os::before_dma_write(tileU, luma / 4);
    starlet::os::before_dma_write(tileV, luma / 4);
    try {
        decoder()->decode(frame, packet_limit, width, height,
                          {{{static_cast<std::uint8_t*>(tileY), luma},
                            {static_cast<std::uint8_t*>(tileU), luma / 4},
                            {static_cast<std::uint8_t*>(tileV), luma / 4}}});
    } catch (const starlet::thp::Error& error) {
        std::fprintf(stderr, "THP video frame not decoded: %s\n", error.what());
        return 1;
    }
    // The SDK's locked-cache DMA wrote the planes to memory behind the data cache; GX texture
    // caches must see the new frame.
    starlet::os::report_cpu_write(tileY, luma);
    starlet::os::report_cpu_write(tileU, luma / 4);
    starlet::os::report_cpu_write(tileV, luma / 4);
    return 0;
}

starlet::sdk::u32 THPAudioDecode(starlet::sdk::s16* audioBuffer, starlet::sdk::u8* audioFrame, starlet::sdk::s32 flag) {
    if (!audioBuffer || !audioFrame) return 0;
    const auto be32 = [&](unsigned at) {
        return std::uint32_t(audioFrame[at]) << 24 | std::uint32_t(audioFrame[at + 1]) << 16 |
               std::uint32_t(audioFrame[at + 2]) << 8 | audioFrame[at + 3];
    };
    const auto right = be32(0), samples = be32(4);
    const std::size_t encoded = (std::size_t(samples) + 13) / 14 * 8;
    const std::size_t size = 80 + (right ? std::size_t(right) + encoded : encoded);
    try {
        return starlet::thp::decode_audio(audioFrame, size, audioBuffer, std::size_t(samples) * 2,
                                          starlet::thp::audio_sample_limit, flag == 1);
    } catch (const starlet::thp::Error& error) {
        std::fprintf(stderr, "THP audio record not decoded: %s\n", error.what());
        return 0;
    }
}
}
