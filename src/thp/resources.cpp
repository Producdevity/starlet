#include "starlet/thp/thp.hpp"
#include "view.hpp"
#include <cmath>
#include <cstring>

namespace starlet::thp {
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw Error(why);
}
std::int16_t signed16(const View& bytes, std::size_t at) {
    const auto word = bytes.be16(at);
    std::int16_t result;
    std::memcpy(&result, &word, 2);
    return result;
}

// One channel's ADPCM: 8-byte frames of a predictor and scale byte, then 14 four-bit samples. The
// arithmetic is the SDK's THPAudioDecode: 64-bit, rounded at bit 16 and clamped to 32 bits.
struct Channel {
    std::int16_t coefficients[8][2];
    std::int16_t yn1, yn2;
};
void decode_channel(const std::uint8_t* data, const Channel& channel, unsigned samples, std::int16_t* first,
                    std::int16_t* second, std::size_t step) {
    unsigned predictor = (data[0] & 0x70) >> 4, scale = data[0] & 0xf, nibbles = 2;
    std::size_t at = 1;
    std::int16_t yn1 = channel.yn1, yn2 = channel.yn2;
    for (unsigned i = 0; i < samples; ++i) {
        if (!(nibbles & 0xf)) {
            predictor = (data[at] & 0x70) >> 4;
            scale = data[at] & 0xf;
            ++at;
            nibbles += 2;
        }
        int sample = nibbles & 1 ? data[at++] & 0xf : data[at] >> 4;
        if (sample >= 8) sample -= 16;
        ++nibbles;
        std::int64_t yn = std::int64_t(channel.coefficients[predictor][1]) * yn2 +
                          std::int64_t(channel.coefficients[predictor][0]) * yn1 +
                          std::int64_t(sample) * (std::int64_t(1) << scale) * 2048;
        yn = yn * 32 + 0x8000;
        if (yn > 2147483647LL) yn = 2147483647LL;
        if (yn < -2147483648LL) yn = -2147483648LL;
        const auto value = std::int16_t(yn >> 16);
        first[i * step] = value;
        if (second) second[i * step] = value;
        yn2 = yn1;
        yn1 = value;
    }
}
} // namespace

sdk::THPHeader read_header(const std::uint8_t* data, std::size_t size, std::uint64_t file_bytes) {
    const View bytes(data, size);
    bytes.check(0, 48);
    sdk::THPHeader h{};
    std::memcpy(h.magic, bytes.data(), 4);
    h.version = bytes.be32(4);
    h.bufSize = bytes.be32(8);
    h.audioMaxSamples = bytes.be32(12);
    h.frameRate = bytes.be_float(16);
    h.numFrames = bytes.be32(20);
    h.firstFrameSize = bytes.be32(24);
    h.movieDataSize = bytes.be32(28);
    h.compInfoDataOffsets = bytes.be32(32);
    h.offsetDataOffsets = bytes.be32(36);
    h.movieDataOffsets = bytes.be32(40);
    h.finalFrameDataOffsets = bytes.be32(44);
    const std::uint64_t end = std::uint64_t(h.movieDataOffsets) + h.movieDataSize;
    require(bytes.be32(0) == 0x54485000 && h.version == 0x11000, "unsupported THP header");
    require(h.bufSize >= 16 && h.bufSize <= 4 * 1024 * 1024 && h.firstFrameSize >= 16 &&
                h.firstFrameSize <= h.bufSize && h.numFrames && h.audioMaxSamples <= audio_sample_limit &&
                std::isfinite(h.frameRate) && h.frameRate > 0 && h.frameRate <= 60,
            "invalid THP stream limits");
    require(
        h.compInfoDataOffsets >= 48 && std::uint64_t(h.compInfoDataOffsets) + 20 <= file_bytes &&
            h.movieDataOffsets >= 48 && h.movieDataSize >= h.firstFrameSize && end <= file_bytes &&
            h.finalFrameDataOffsets >= h.movieDataOffsets && h.finalFrameDataOffsets < end &&
            (!h.offsetDataOffsets || std::uint64_t(h.offsetDataOffsets) + std::uint64_t(h.numFrames) * 8 <= file_bytes),
        "THP stream offset exceeds its file");
    return h;
}
sdk::THPFrameCompInfo read_components(const std::uint8_t* data, std::size_t size) {
    const View bytes(data, size);
    bytes.check(0, 20);
    sdk::THPFrameCompInfo c{};
    c.numComponents = bytes.be32(0);
    std::memcpy(c.frameComp, bytes.data() + 4, 16);
    require(c.numComponents == 1 || c.numComponents == 2, "unsupported THP component count");
    require(c.frameComp[0] == 0 && (c.numComponents == 1 || c.frameComp[1] == 1),
            "THP requires video then optional audio");
    return c;
}
sdk::THPVideoInfo read_video_info(const std::uint8_t* data, std::size_t size) {
    const View bytes(data, size);
    sdk::THPVideoInfo info{bytes.be32(0), bytes.be32(4), bytes.be32(8)};
    require(info.xSize && info.ySize && info.xSize <= 1024 && info.ySize <= 1024 && !(info.xSize % 16) &&
                !(info.ySize % 8) && info.videoType <= 2,
            "unsupported THP video dimensions or scan type");
    return info;
}
sdk::THPAudioInfo read_audio_info(const std::uint8_t* data, std::size_t size) {
    const View bytes(data, size);
    sdk::THPAudioInfo info{bytes.be32(0), bytes.be32(4), bytes.be32(8), bytes.be32(12)};
    require((info.sndChannels == 1 || info.sndChannels == 2) && info.sndFrequency && info.sndFrequency <= 48000 &&
                info.sndNumTracks && info.sndNumTracks <= 16,
            "unsupported THP audio metadata");
    return info;
}
std::array<std::uint32_t, 16> read_frame_sizes(const std::uint8_t* data, std::size_t size, const sdk::THPFrameCompInfo &c,
                                               unsigned tracks) {
    const View bytes(data, size);
    require(c.numComponents >= 1 && c.numComponents <= 2 && tracks <= 16, "invalid THP frame component metadata");
    bytes.check(0, 8 + std::size_t(c.numComponents) * 4);
    std::array<std::uint32_t, 16> sizes{};
    std::size_t position = 8 + std::size_t(c.numComponents) * 4;
    for (unsigned i = 0; i < c.numComponents; ++i) {
        sizes[i] = bytes.be32(8 + i * 4);
        require(sizes[i] && sizes[i] <= 4 * 1024 * 1024, "invalid THP component extent");
        const auto copies = c.frameComp[i] == 1 ? tracks : 1u;
        require(copies && c.frameComp[i] <= 1, "invalid THP frame audio tracks");
        const auto length = std::size_t(sizes[i]) * copies;
        bytes.check(position, length);
        position += length;
    }
    return sizes;
}
unsigned decode_audio(const std::uint8_t* record, std::size_t size, std::int16_t *output, std::size_t capacity,
                      unsigned max_samples, bool planar) {
    const View source(record, size);
    source.check(0, 80);
    const auto right = source.be32(0), samples = source.be32(4);
    require(max_samples <= audio_sample_limit && samples && samples <= max_samples && output &&
                capacity >= std::size_t(samples) * 2,
            "THP audio sample count exceeds output capacity");
    const auto encoded = ((std::size_t(samples) + 13) / 14) * 8;
    source.check(80, encoded);
    if (right) {
        require(right >= encoded, "overlapping THP audio channels");
        source.check(80 + std::size_t(right), encoded);
    }
    Channel left_channel{}, right_channel{};
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            left_channel.coefficients[i][j] = signed16(source, 8 + (i * 2 + j) * 2);
            right_channel.coefficients[i][j] = signed16(source, 40 + (i * 2 + j) * 2);
        }
    left_channel.yn1 = signed16(source, 72);
    left_channel.yn2 = signed16(source, 74);
    right_channel.yn1 = signed16(source, 76);
    right_channel.yn2 = signed16(source, 78);
    // Right then left: interleaved pairs, or all right samples followed by all left ones.
    std::int16_t* right_out = output;
    std::int16_t* left_out = planar ? output + samples : output + 1;
    const std::size_t step = planar ? 1 : 2;
    if (!right) {
        decode_channel(source.data() + 80, left_channel, samples, left_out, right_out, step);
    } else {
        decode_channel(source.data() + 80, left_channel, samples, left_out, nullptr, step);
        decode_channel(source.data() + 80 + right, right_channel, samples, right_out, nullptr, step);
    }
    return samples;
}
} // namespace starlet::thp
