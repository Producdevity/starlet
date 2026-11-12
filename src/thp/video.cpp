#include "starlet/thp/thp.hpp"
#include "view.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace starlet::thp {
namespace {
[[noreturn]] void fail(const char *message) { throw Error(message); }
inline void require(bool value, const char *message) {
    if (!value)
        fail(message);
}

// Coefficients are stored transposed, at column * 8 + row, so that both IDCT passes
// work across contiguous lanes. This maps coding order (ITU-T T.81 figure A.6) there.
constexpr std::array<std::uint8_t, 64> transposed_order{
    0,  8,  1,  2,  9,  16, 24, 17, 10, 3,  4,  11, 18, 25, 32, 40, 33, 26, 19, 12, 5,  6,
    13, 20, 27, 34, 41, 48, 56, 49, 42, 35, 28, 21, 14, 7,  15, 22, 29, 36, 43, 50, 57, 58,
    51, 44, 37, 30, 23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63};

// AAN scale factors, sqrt(2) cos(k pi / 16) with 1 for k = 0. Folding them into the
// quantization tables leaves the IDCT with five multiplications per pass.
constexpr std::array<float, 8> aan_scale{1.0f,         1.387039845f, 1.306562965f, 1.175875602f,
                                         1.0f,         0.785694958f, 0.541196100f, 0.275899379f};

// One-dimensional inverse DCT of eight AAN-scaled coefficients, eight times too large.
inline void idct8(float *v, std::size_t stride) {
    const float x0 = v[0], x1 = v[stride], x2 = v[2 * stride], x3 = v[3 * stride];
    const float x4 = v[4 * stride], x5 = v[5 * stride], x6 = v[6 * stride], x7 = v[7 * stride];
    const float sum04 = x0 + x4, difference04 = x0 - x4;
    const float sum26 = x2 + x6, rotation26 = (x2 - x6) * 1.414213562f - sum26;
    const float e0 = sum04 + sum26, e3 = sum04 - sum26;
    const float e1 = difference04 + rotation26, e2 = difference04 - rotation26;
    const float z13 = x5 + x3, z10 = x5 - x3, z11 = x1 + x7, z12 = x1 - x7;
    const float o7 = z11 + z13, o11 = (z11 - z13) * 1.414213562f;
    const float z5 = (z10 + z12) * 1.847759065f;
    const float o10 = z5 - z12 * 1.082392200f, o12 = z5 - z10 * 2.613125930f;
    const float o6 = o12 - o7, o5 = o11 - o6, o4 = o10 - o5;
    v[0] = e0 + o7;
    v[7 * stride] = e0 - o7;
    v[stride] = e1 + o6;
    v[6 * stride] = e1 - o6;
    v[2 * stride] = e2 + o5;
    v[5 * stride] = e2 - o5;
    v[3 * stride] = e3 + o4;
    v[4 * stride] = e3 - o4;
}

// Two-dimensional inverse DCT of a transposed block (column * 8 + row) into row-major
// samples: rows first, as columns of the transposed block, then columns.
void inverse_dct(std::array<float, 64> &c, std::array<std::uint8_t, 64> &samples) {
    for (unsigned row = 0; row < 8; ++row)
        idct8(c.data() + row, 8);
    alignas(16) std::array<float, 64> t;
    for (unsigned row = 0; row < 8; ++row)
        for (unsigned column = 0; column < 8; ++column)
            t[row * 8 + column] = c[column * 8 + row];
    for (unsigned column = 0; column < 8; ++column)
        idct8(t.data() + column, 8);
    for (unsigned i = 0; i < 64; ++i)
        samples[i] = std::uint8_t(std::clamp(int(t[i] * 0.125f), 0, 255));
}

constexpr unsigned lookup_bits = 9;

// A symbol that a lookup_bits prefix resolves. When its code and the coefficient bits
// that follow both fit, value holds that coefficient and skip covers both.
struct FastSymbol {
    std::int16_t value;
    std::uint8_t skip;
    std::uint8_t symbol;
};

// Canonical Huffman table (T.81 annex C, decoding as in F.2.2.3).
struct Huffman {
    std::array<std::int32_t, 17> max_code{};
    std::array<std::int32_t, 17> offset{};
    std::array<std::uint8_t, 256> values{};
    std::array<FastSymbol, 1 << lookup_bits> fast{};
};

int extend(unsigned bits, unsigned size) {
    return bits < (1u << (size - 1)) ? int(bits) - (1 << size) + 1 : int(bits);
}

// THP's entropy-coded data has neither 0xFF00 stuffing nor restart markers; a restart
// interval only realigns to the next byte. Bits are kept MSB-first in a 64-bit cache.
class BitReader {
  public:
    explicit BitReader(View data) : data_(data.data()), size_(data.size()) { refill(); }
    // Keeps at least 32 bits cached, enough for one coefficient. Bytes past the end read
    // as zero; check() rejects a frame that consumed them.
    [[gnu::always_inline]] void refill() {
        if (available_ < 32)
            load();
    }
    [[gnu::always_inline]] unsigned peek(unsigned count) const { return unsigned(buffer_ >> (64 - count)); }
    [[gnu::always_inline]] void skip(unsigned count) {
        buffer_ <<= count;
        available_ -= count;
    }
    void align() { skip(available_ % 8); }
    void check() const { require(available_ >= padding_, "THP video data ends inside the frame"); }

  private:
    void load() {
        if (size_ - position_ >= 8) {
            std::uint64_t word = 0;
            for (unsigned i = 0; i < 8; ++i)
                word = word << 8 | data_[position_ + i];
            buffer_ |= word >> available_;
            const unsigned bytes = (63 - available_) / 8;
            position_ += bytes;
            available_ += bytes * 8;
            return;
        }
        while (available_ <= 56) {
            if (position_ < size_)
                buffer_ |= std::uint64_t(data_[position_++]) << (56 - available_);
            else
                padding_ += 8;
            available_ += 8;
        }
    }

    const std::uint8_t *data_;
    std::size_t size_, position_ = 0;
    std::uint64_t buffer_ = 0;
    unsigned available_ = 0, padding_ = 0;
};

struct Coefficient {
    unsigned symbol;
    int value;
};

// Reads a run/size symbol and the coefficient its size announces. The cache must hold
// at least 31 bits.
[[gnu::always_inline]] inline Coefficient next_coefficient(BitReader &bits, const Huffman &table) {
    unsigned symbol;
    if (const auto entry = table.fast[bits.peek(lookup_bits)]; entry.skip) {
        bits.skip(entry.skip);
        if (entry.value)
            return {entry.symbol, entry.value};
        symbol = entry.symbol;
    } else {
        unsigned length = lookup_bits + 1;
        while (length <= 16 && std::int32_t(bits.peek(length)) > table.max_code[length])
            ++length;
        require(length <= 16, "invalid THP Huffman code");
        symbol = table.values[std::size_t(table.offset[length] + std::int32_t(bits.peek(length)))];
        bits.skip(length);
    }
    const unsigned size = symbol & 15;
    if (!size)
        return {symbol, 0};
    const auto value = extend(bits.peek(size), size);
    bits.skip(size);
    return {symbol, value};
}

class Segment {
  public:
    Segment(View frame, std::size_t &position) {
        require(position + 2 <= frame.size(), "THP marker segment is truncated");
        const std::size_t length = frame.be16(position);
        require(length >= 2 && position + length <= frame.size(), "THP marker segment exceeds the frame");
        data_ = frame.subview(position + 2, length - 2);
        position += length;
    }
    std::uint8_t u8() {
        require(at_ < data_.size(), "THP marker segment is truncated");
        return data_.u8(at_++);
    }
    std::uint16_t be16() {
        const auto high = u8();
        return std::uint16_t(high << 8 | u8());
    }
    bool done() const { return at_ == data_.size(); }

  private:
    View data_;
    std::size_t at_ = 0;
};
} // namespace

struct VideoDecoder::Impl {
    std::array<std::array<float, 64>, 4> quantization{};
    std::array<bool, 4> quantization_valid{};
    std::array<Huffman, 4> huffman{};
    std::array<bool, 4> huffman_valid{};
    std::array<std::uint8_t, 3> quantization_selector{};
    std::array<const Huffman *, 3> dc{}, ac{};
    unsigned restart_interval = 0;
    // GX I8 layout: 8x4 texel tiles in row-major tile order, so an 8x8 block is two
    // 32-byte runs one tile row apart. Planes cover whole MCU rows.
    std::array<std::vector<std::uint8_t>, 3> plane;
    std::array<unsigned, 3> plane_width{};

    void read_quantization(Segment segment) {
        do {
            const auto id = segment.u8();
            require(id < 4, "THP quantization tables are 8-bit, ids 0 to 3");
            for (unsigned k = 0; k < 64; ++k) {
                const unsigned at = transposed_order[k];
                quantization[id][at] = float(segment.u8()) * aan_scale[at / 8] * aan_scale[at % 8];
            }
            quantization_valid[id] = true;
        } while (!segment.done());
    }
    void read_huffman(Segment segment) {
        do {
            const auto kind = segment.u8();
            require((kind >> 4) <= 1 && (kind & 15) <= 1, "THP Huffman tables are classes 0-1, ids 0-1");
            const unsigned index = (kind & 15) * 2 + (kind >> 4);
            std::array<std::uint8_t, 16> counts;
            for (auto &count : counts)
                count = segment.u8();
            auto &table = huffman[index];
            table.fast = {};
            std::int32_t code = 0, values = 0;
            for (unsigned length = 1; length <= 16; ++length) {
                const auto count = counts[length - 1];
                if (count) {
                    table.offset[length] = values - code;
                    code += count;
                    table.max_code[length] = code - 1;
                    values += count;
                    require(code <= (1 << length) && values <= 256, "invalid THP Huffman code lengths");
                } else {
                    table.max_code[length] = -1;
                }
                code <<= 1;
            }
            for (std::int32_t i = 0; i < values; ++i)
                table.values[std::size_t(i)] = segment.u8();
            for (unsigned length = 1; length <= lookup_bits; ++length) {
                const auto last = table.max_code[length];
                for (auto prefix = last + 1 - counts[length - 1]; prefix <= last; ++prefix) {
                    const auto symbol = table.values[std::size_t(table.offset[length] + prefix)];
                    const unsigned size = symbol & 15, spare = lookup_bits - length;
                    for (unsigned suffix = 0; suffix < 1u << spare; ++suffix) {
                        FastSymbol entry{0, std::uint8_t(length), symbol};
                        if (size && size <= spare) {
                            entry.value = std::int16_t(extend(suffix >> (spare - size), size));
                            entry.skip = std::uint8_t(length + size);
                        }
                        table.fast[(std::size_t(prefix) << spare) | suffix] = entry;
                    }
                }
            }
            huffman_valid[index] = true;
        } while (!segment.done());
    }
    void read_frame(Segment segment, unsigned width, unsigned height) {
        require(segment.u8() == 8, "THP video samples are 8-bit");
        const unsigned frame_height = segment.be16(), frame_width = segment.be16();
        require(frame_width == width && frame_height == height, "THP frame dimensions differ from movie metadata");
        require(segment.u8() == 3, "THP video has three components");
        for (unsigned i = 0; i < 3; ++i) {
            segment.u8();
            require(segment.u8() == (i ? 0x11 : 0x22), "THP video is sampled 4:2:0");
            quantization_selector[i] = segment.u8();
            require(quantization_selector[i] < 4, "invalid THP quantization table selector");
        }
        require(segment.done(), "THP frame header has extra data");
    }
    void read_scan(Segment segment) {
        require(segment.u8() == 3, "THP scans cover all three components");
        for (unsigned i = 0; i < 3; ++i) {
            segment.u8();
            const auto tables = segment.u8();
            const unsigned dc_index = (tables >> 4) * 2, ac_index = (tables & 15) * 2 + 1;
            require(dc_index < 4 && ac_index < 4 && huffman_valid[dc_index] && huffman_valid[ac_index],
                    "THP scan selects a missing Huffman table");
            dc[i] = &huffman[dc_index];
            ac[i] = &huffman[ac_index];
        }
    }
    // Decodes one block into the 8x8 area of plane component at (x, y).
    void block(BitReader &bits, unsigned component, int &prediction, unsigned x, unsigned y) {
        const auto &q = quantization[quantization_selector[component]];
        bits.refill();
        const auto difference = next_coefficient(bits, *dc[component]);
        require(difference.symbol < 16, "invalid THP DC difference size");
        prediction += difference.value;
        // The IDCT output is eight times too large: 1028 in the DC term level-shifts it by
        // 128 and rounds the truncating conversion below.
        alignas(16) std::array<float, 64> c{};
        c[0] = float(prediction) * q[0] + 1028.0f;
        bool detail = false;
        for (unsigned k = 1; k < 64;) {
            bits.refill();
            const auto [symbol, value] = next_coefficient(bits, *ac[component]);
            if (!(symbol & 15)) {
                if (symbol != 0xf0)
                    break;
                k += 16;
                continue;
            }
            k += symbol >> 4;
            require(k < 64, "THP block has more than 64 coefficients");
            const unsigned at = transposed_order[k++];
            c[at] = float(value) * q[at];
            detail = true;
        }
        bits.check();
        const auto width = plane_width[component];
        auto *out = plane[component].data() + std::size_t(y) * width + x * 4;
        if (!detail) {
            const auto value = std::uint8_t(std::clamp(int(c[0] * 0.125f), 0, 255));
            std::memset(out, value, 32);
            std::memset(out + width * 4, value, 32);
            return;
        }
        alignas(16) std::array<std::uint8_t, 64> samples;
        inverse_dct(c, samples);
        std::memcpy(out, samples.data(), 32);
        std::memcpy(out + width * 4, samples.data() + 32, 32);
    }
    void scan(BitReader bits, unsigned width, unsigned height) {
        for (unsigned i = 0; i < 3; ++i)
            require(quantization_valid[quantization_selector[i]], "THP frame selects a missing quantization table");
        const unsigned columns = width / 16, rows = (height + 15) / 16;
        plane_width = {width, width / 2, width / 2};
        plane[0].resize(std::size_t(width) * rows * 16);
        plane[1].resize(std::size_t(width / 2) * rows * 8);
        plane[2].resize(plane[1].size());
        int prediction[3] = {};
        unsigned until_restart = restart_interval;
        for (unsigned row = 0; row < rows; ++row)
            for (unsigned column = 0; column < columns; ++column) {
                const unsigned x = column * 16, y = row * 16;
                block(bits, 0, prediction[0], x, y);
                block(bits, 0, prediction[0], x + 8, y);
                block(bits, 0, prediction[0], x, y + 8);
                block(bits, 0, prediction[0], x + 8, y + 8);
                block(bits, 1, prediction[1], x / 2, y / 2);
                block(bits, 2, prediction[2], x / 2, y / 2);
                if (restart_interval && !--until_restart) {
                    bits.align();
                    prediction[0] = prediction[1] = prediction[2] = 0;
                    until_restart = restart_interval;
                }
            }
    }
};

VideoDecoder::VideoDecoder() {
    impl_ = std::make_unique<Impl>();
}
VideoDecoder::~VideoDecoder() {
    impl_.reset();
}
// Frames are independent: each carries its tables before the scan.
void VideoDecoder::reset() {}
void VideoDecoder::decode(const std::uint8_t* frame_bytes, std::size_t frame_size, unsigned width, unsigned height,
                          std::array<Plane, 3> planes) {
    const View source(frame_bytes, frame_size);
    require(width && height && width <= 1024 && height <= 1024 && !(width % 16) && !(height % 8),
            "THP dimensions cannot fit the native tiled planes");
    require(source.size() && source.size() <= 4 * 1024 * 1024, "THP video packet exceeds its native bound");
    for (unsigned i = 0; i < 3; ++i) {
        const auto size = std::size_t(width >> (i != 0)) * (height >> (i != 0));
        const auto address = reinterpret_cast<std::uintptr_t>(planes[i].data);
        require(planes[i].data && planes[i].size >= size && size <= UINTPTR_MAX - address,
                "THP output plane is missing or too small");
    }
    auto &d = *impl_;
    d.quantization_valid = {};
    d.huffman_valid = {};
    d.restart_interval = 0;
    // Marker handling follows the SDK's THPVideoDecode: fill bytes are skipped and the
    // frame ends at its scan header, whose entropy-coded data runs to the packet's end.
    bool frame = false;
    for (std::size_t at = 0;;) {
        require(at < source.size() && source.u8(at) == 0xff, "THP video frame lacks a marker");
        while (at < source.size() && source.u8(at) == 0xff)
            ++at;
        require(at < source.size(), "THP video frame ends after a fill byte");
        const auto marker = source.u8(at++);
        if (marker == 0xd8)
            continue;
        if (marker == 0xdb) {
            d.read_quantization(Segment(source, at));
        } else if (marker == 0xc4) {
            d.read_huffman(Segment(source, at));
        } else if (marker == 0xc0) {
            d.read_frame(Segment(source, at), width, height);
            frame = true;
        } else if (marker == 0xdd) {
            Segment segment(source, at);
            d.restart_interval = segment.be16();
        } else if (marker == 0xda) {
            require(frame, "THP scan precedes its frame header");
            d.read_scan(Segment(source, at));
            d.scan(BitReader(source.subview(at, source.size() - at)), width, height);
            break;
        } else if ((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe) {
            Segment skipped(source, at);
        } else {
            throw Error("unsupported THP video marker");
        }
    }
    // The tiled planes start with the visible rows; the rest is the last MCU row's overhang.
    for (unsigned i = 0; i < 3; ++i)
        std::memcpy(planes[i].data, d.plane[i].data(), std::size_t(width >> (i != 0)) * (height >> (i != 0)));
}
bool frame_size(const std::uint8_t* frame, std::size_t size, unsigned& width, unsigned& height) {
    const View source(frame, size);
    try {
        for (std::size_t at = 0; at < source.size();) {
            if (source.u8(at) != 0xff) return false;
            while (at < source.size() && source.u8(at) == 0xff)
                ++at;
            const auto marker = source.u8(at++);
            if (marker == 0xd8) continue;
            if (marker == 0xda) return false;
            if (marker == 0xc0) {
                height = source.be16(at + 3);
                width = source.be16(at + 5);
                return true;
            }
            at += source.be16(at);
        }
    } catch (const Error&) {
    }
    return false;
}
} // namespace starlet::thp
