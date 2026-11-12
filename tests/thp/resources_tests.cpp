#include "api.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace test;
namespace {
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void word(std::vector<std::uint8_t> &b, unsigned at, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        b.at(at + i) = n >> ((3 - i) * 8);
}
template <class F> void rejects(F fn) {
    bool threw = false;
    try {
        fn();
    } catch (const assets::AssetError &) {
        threw = true;
    }
    check(threw, "invalid THP resource must reject");
}
void audio() {
    std::vector<std::uint8_t> bytes(96);
    word(bytes, 0, 8);
    word(bytes, 4, 14);
    // Zero predictors make the two channels distinct, exact signed-nibble ramps.
    for (unsigned i = 0; i < 7; ++i) {
        bytes[81 + i] = i * 0x22 + 1;
        bytes[89 + i] = 0xfe - i * 0x22;
    }
    auto original = bytes;
    std::array<std::int16_t, 32> out;
    out.fill(12345);
    check(platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data() + 2, 28, 14) == 14,
          "stereo sample count");
    for (unsigned i = 0; i < 14; ++i) {
        int l = i, r = 15 - i;
        if (l >= 8)
            l -= 16;
        if (r >= 8)
            r -= 16;
        check(out[2 + i * 2] == r && out[3 + i * 2] == l, "native THP R,L sample order and sign");
    }
    check(out[0] == 12345 && out[1] == 12345 && out[30] == 12345 && out[31] == 12345, "audio canaries");
    auto stereo = out;
    platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data() + 2, 28, 14, true);
    for (unsigned i = 0; i < 14; ++i)
        check(out[2 + i] == stereo[2 + i * 2] && out[16 + i] == stereo[3 + i * 2], "planar channel ordering");
    word(bytes, 0, 0);
    platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data() + 2, 28, 14);
    for (unsigned i = 0; i < 14; ++i)
        check(out[2 + i * 2] == stereo[3 + i * 2] && out[3 + i * 2] == stereo[3 + i * 2], "mono duplication");
    bytes = original;
    rejects([&] { platform::decode_thp_audio({bytes.data(), 79}, out.data(), 32, 14); });
    rejects([&] { platform::decode_thp_audio({bytes.data(), 95}, out.data(), 32, 14); });
    rejects([&] { platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data(), 27, 14); });
    rejects([&] { platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data(), 32, 13); });
    word(bytes, 0, 7);
    rejects([&] { platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data(), 32, 14); });
    word(bytes, 0, 8);
    word(bytes, 4, 0);
    rejects([&] { platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data(), 32, 14); });
    bytes = original;
    platform::decode_thp_audio(assets::ByteView(bytes.data(), bytes.size()), out.data(), 32, 14);
    check(bytes == original, "decoder must preserve canonical source");
    bytes.assign(96, 0);
    word(bytes, 0, 8);
    word(bytes, 4, 14);
    bytes[8] = 4;
    bytes[73] = 1;
    platform::decode_thp_audio({bytes.data(), bytes.size()}, out.data() + 2, 28, 14);
    for (unsigned i = 0; i < 14; ++i)
        check(out[2 + i * 2] == 0 && out[3 + i * 2] == 1, "SDK predictor rounds half upward on each sample");
}
void movie(const char *path, const char *dump) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(bool(input), "open original movie");
    const auto length = input.tellg();
    input.seekg(0);
    std::array<std::uint8_t, 96> bytes{};
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    assets::ByteView b(bytes.data(), bytes.size());
    auto header = platform::read_thp_header(b, length);
    auto comp = platform::read_thp_components(b.subview(header.compInfoDataOffsets, 20));
    auto video = platform::read_thp_video_info(b.subview(header.compInfoDataOffsets + 20, 12));
    auto sound = platform::read_thp_audio_info(b.subview(header.compInfoDataOffsets + 32, 16));
    check(video.xSize == 640 && video.ySize == 368 && sound.sndChannels == 2 && sound.sndFrequency == 32000,
          "original movie metadata");
    std::ofstream output;
    if (dump)
        output.open(dump, std::ios::binary);
    std::uint64_t digest = 14695981039346656037ull;
    auto offset = header.movieDataOffsets, size = header.firstFrameSize;
    for (unsigned f = 0; f < 20; ++f) {
        std::vector<std::uint8_t> packet(size);
        input.seekg(offset);
        input.read(reinterpret_cast<char *>(packet.data()), packet.size());
        check(std::size_t(input.gcount()) == size, "movie frame extent");
        assets::ByteView frame(packet.data(), packet.size());
        auto sizes = platform::read_thp_frame_sizes(frame, comp, sound.sndNumTracks);
        std::vector<std::int16_t> pcm(header.audioMaxSamples * 2 + 32, 12345);
        const auto samples = platform::decode_thp_audio(frame.subview(16 + sizes[0], sizes[1]), pcm.data() + 16,
                                                        header.audioMaxSamples * 2, header.audioMaxSamples);
        for (unsigned i = 0; i < 16; ++i)
            check(pcm[i] == 12345 && pcm[pcm.size() - 1 - i] == 12345, "original audio bounds");
        for (unsigned i = 0; i < samples * 2; ++i) {
            const auto n = static_cast<std::uint16_t>(pcm[i + 16]);
            digest ^= n & 255;
            digest *= 1099511628211ull;
            digest ^= n >> 8;
            digest *= 1099511628211ull;
        }
        if (dump)
            output.write(reinterpret_cast<const char *>(pcm.data() + 16), samples * 4);
        offset += size;
        size = frame.be32(0);
    }
    check(!dump || bool(output), "write movie audio proof");
    std::printf("THP_AUDIO frames=20 fnv64=%016llx\n", static_cast<unsigned long long>(digest));
    rejects([&] { platform::read_thp_header(b, 95); });
    rejects([&] { platform::read_thp_components(b.subview(48, 19)); });
    std::vector<std::uint8_t> broken(bytes.begin(), bytes.end());
    word(broken, 8, 4 * 1024 * 1024 + 1);
    rejects([&] { platform::read_thp_header(assets::ByteView(broken.data(), broken.size()), length); });
    word(broken, 8, header.bufSize);
    word(broken, 16, 0x7fc00000);
    rejects([&] { platform::read_thp_header(assets::ByteView(broken.data(), broken.size()), length); });
    broken.assign(16, 0);
    word(broken, 8, 0x1000);
    word(broken, 12, 0x1000);
    rejects([&] {
        platform::read_thp_frame_sizes(assets::ByteView(broken.data(), broken.size()), comp, sound.sndNumTracks);
    });
}
} // namespace
int main(int argc, char **argv) {
    try {
        audio();
        if (argc >= 2)
            movie(argv[1], argc == 3 ? argv[2] : nullptr);
        std::puts("native THP resources passed");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "THP resources: %s\n", e.what());
        return 1;
    }
}
