#pragma once

// starlet::thp decodes THP movies: the frame's baseline JPEG video into the GX I8 Y, U and V tiles
// THPDraw samples, and its ADPCM audio. Movie bytes stay big-endian, as read from the disc.

#include "starlet/sdk/thp.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace starlet::thp {

// Malformed or unsupported movie data, and destinations that do not fit.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Plane {
    std::uint8_t* data;
    std::size_t size;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    // Decodes one frame of width x height (the movie's THPVideoInfo) into Y, U and V; the planes
    // change only once the whole frame has decoded.
    void decode(const std::uint8_t* frame, std::size_t size, unsigned width, unsigned height, std::array<Plane, 3> planes);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The frame's dimensions from its start-of-frame segment.
bool frame_size(const std::uint8_t* frame, std::size_t size, unsigned& width, unsigned& height);

constexpr unsigned audio_sample_limit = 8192;

// The movie's metadata, checked against the file's size and the decoders' limits.
sdk::THPHeader read_header(const std::uint8_t* bytes, std::size_t size, std::uint64_t file_bytes);
sdk::THPFrameCompInfo read_components(const std::uint8_t* bytes, std::size_t size);
sdk::THPVideoInfo read_video_info(const std::uint8_t* bytes, std::size_t size);
sdk::THPAudioInfo read_audio_info(const std::uint8_t* bytes, std::size_t size);
// A frame's component sizes, each component's data checked within the frame.
std::array<std::uint32_t, 16> read_frame_sizes(const std::uint8_t* frame, std::size_t size,
                                               const sdk::THPFrameCompInfo& components, unsigned audio_tracks);
// Decodes one audio record into `output`: right and left interleaved, or with `planar` all right
// samples then all left ones; mono data fills both. Returns the samples per channel.
unsigned decode_audio(const std::uint8_t* record, std::size_t size, std::int16_t* output, std::size_t output_samples,
                      unsigned max_samples, bool planar = false);

} // namespace starlet::thp
