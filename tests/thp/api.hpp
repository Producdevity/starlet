#pragma once
// The call shapes these tests were first written against (Super Mario Galaxy's platform layer),
// over starlet::thp.
#include "starlet/thp/thp.hpp"
#include <array>
#include <cstdint>

namespace test {
namespace assets {
using AssetError = starlet::thp::Error;
class ByteView {
public:
    ByteView() = default;
    ByteView(const void* data, std::size_t size) : data_(static_cast<const std::uint8_t*>(data)), size_(size) {}
    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }
    ByteView subview(std::size_t offset, std::size_t count) const {
        if (offset > size_ || count > size_ - offset) throw AssetError("byte view outside resource");
        return {data_ + offset, count};
    }
    std::uint8_t u8(std::size_t at) const { return subview(at, 1).data_[0]; }
    std::uint16_t be16(std::size_t at) const {
        const auto* p = subview(at, 2).data_;
        return std::uint16_t(p[0] << 8 | p[1]);
    }
    std::uint32_t be32(std::size_t at) const {
        const auto* p = subview(at, 4).data_;
        return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
    }
private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};
} // namespace assets
namespace platform {
using ThpPlane = starlet::thp::Plane;
class ThpVideoDecoder {
public:
    void decode(assets::ByteView input, unsigned width, unsigned height, std::array<ThpPlane, 3> planes) {
        decoder_.decode(input.data(), input.size(), width, height, planes);
    }
    void reset() { decoder_.reset(); }
private:
    starlet::thp::VideoDecoder decoder_;
};
constexpr unsigned thp_audio_sample_limit = starlet::thp::audio_sample_limit;
inline auto read_thp_header(assets::ByteView b, std::uint64_t file_bytes) { return starlet::thp::read_header(b.data(), b.size(), file_bytes); }
inline auto read_thp_components(assets::ByteView b) { return starlet::thp::read_components(b.data(), b.size()); }
inline auto read_thp_video_info(assets::ByteView b) { return starlet::thp::read_video_info(b.data(), b.size()); }
inline auto read_thp_audio_info(assets::ByteView b) { return starlet::thp::read_audio_info(b.data(), b.size()); }
inline auto read_thp_frame_sizes(assets::ByteView b, const starlet::sdk::THPFrameCompInfo& c, unsigned tracks) {
    return starlet::thp::read_frame_sizes(b.data(), b.size(), c, tracks);
}
inline unsigned decode_thp_audio(assets::ByteView b, std::int16_t* output, std::size_t capacity, unsigned max_samples,
                                 bool planar = false) {
    return starlet::thp::decode_audio(b.data(), b.size(), output, capacity, max_samples, planar);
}
} // namespace platform
} // namespace test
