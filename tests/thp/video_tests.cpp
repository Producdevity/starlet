#include "api.hpp"
#ifdef STARLET_THP_SDK_TESTS
#include "starlet/os/os_core.hpp"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
struct Planes {
    std::array<std::vector<std::uint8_t>, 3> storage;
    std::array<platform::ThpPlane, 3> planes;
    Planes(unsigned w, unsigned h) {
        for (unsigned i = 0; i < 3; ++i) {
            const auto n = std::size_t(w >> (i != 0)) * (h >> (i != 0));
            storage[i].assign(n + 64, 0xa9);
            planes[i] = {storage[i].data() + 32, n};
        }
    }
    void canaries() const {
        for (const auto &p : storage)
            for (unsigned i = 0; i < 32; ++i)
                check(p[i] == 0xa9 && p[p.size() - 1 - i] == 0xa9, "THP plane bounds");
    }
};
template <class F> void rejects(F fn) {
    bool threw = false;
    try {
        fn();
    } catch (const assets::AssetError &) {
        threw = true;
    }
    check(threw, "invalid THP input must reject");
}
std::uint64_t digest(const Planes &planes) {
    std::uint64_t h = 14695981039346656037ull;
    for (const auto &p : planes.planes)
        for (std::size_t i = 0; i < p.size; ++i) {
            h ^= p.data[i];
            h *= 1099511628211ull;
        }
    return h;
}
void bounds() {
    platform::ThpVideoDecoder decoder;
    Planes out(16, 8);
    const std::array<std::uint8_t, 4> invalid{{1, 2, 3, 4}};
    auto decode = [&](assets::ByteView input, unsigned w, unsigned h, std::array<platform::ThpPlane, 3> p) {
        decoder.decode(input, w, h, p);
    };
    rejects([&] { decode({invalid.data(), invalid.size()}, 0, 8, out.planes); });
    rejects([&] { decode({invalid.data(), invalid.size()}, 16, 7, out.planes); });
    rejects([&] { decode({invalid.data(), invalid.size()}, 1040, 8, out.planes); });
    rejects([&] { decode({}, 16, 8, out.planes); });
    rejects([&] { decode({invalid.data(), 4 * 1024 * 1024 + 1}, 16, 8, out.planes); });
    for (unsigned i = 0; i < 3; ++i) {
        auto p = out.planes;
        p[i].size = 0;
        rejects([&] { decode({invalid.data(), invalid.size()}, 16, 8, p); });
    }
    rejects([&] { decode({invalid.data(), invalid.size()}, 16, 8, out.planes); });
    decoder.reset();
    out.canaries();
    for (const auto &p : out.storage)
        for (auto value : p)
            check(value == 0xa9, "failed frame must not publish output");
}
// Synthetic frames in THP's JPEG variant: baseline segments, 4:2:0 sampling, and entropy
// data without byte stuffing, where a restart interval only realigns to a byte.
constexpr std::array<std::uint8_t, 64> natural_order{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
unsigned quantizer(unsigned component, unsigned k) { return k ? (component ? 3 + k % 4 : 2 + k % 5) : 8; }

// Every code in a table has the same length, so a symbol's code is its index.
struct Table {
    unsigned length;
    std::vector<std::uint8_t> symbols;
    unsigned code(unsigned symbol) const {
        const auto at = std::find(symbols.begin(), symbols.end(), symbol);
        check(at != symbols.end(), "test Huffman table lacks a symbol");
        return unsigned(at - symbols.begin());
    }
};
const std::vector<std::uint8_t> dc_symbols{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const std::vector<std::uint8_t> ac_symbols{0x00, 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x31, 0x32};
// Luma codes fit the decoder's lookup table, alone or with their value bits; chroma
// codes are too long for it.
const std::array<Table, 2> dc_tables{{{4, dc_symbols}, {10, dc_symbols}}};
const std::array<Table, 2> ac_tables{{{4, ac_symbols}, {12, ac_symbols}}};

class BitWriter {
  public:
    void put(unsigned value, unsigned count) {
        while (count--) {
            byte_ = std::uint8_t(byte_ << 1 | (value >> count & 1));
            if (++used_ == 8) {
                bytes.push_back(byte_);
                used_ = 0;
            }
        }
    }
    void align() {
        while (used_)
            put(1, 1);
    }
    std::vector<std::uint8_t> bytes;

  private:
    std::uint8_t byte_ = 0;
    unsigned used_ = 0;
};

void put_value(BitWriter &bits, const Table &table, unsigned run, int value) {
    unsigned size = 0;
    for (auto magnitude = unsigned(std::abs(value)); magnitude; magnitude >>= 1)
        ++size;
    bits.put(table.code(run << 4 | size), table.length);
    bits.put(unsigned(value < 0 ? value + (1 << size) - 1 : value), size);
}

struct Block {
    int dc;
    std::vector<std::pair<unsigned, int>> ac; // coding-order index and quantized value
};
struct Frame {
    unsigned width, height, restart = 0;
    std::vector<Block> blocks; // Y0 Y1 Y2 Y3 U V per MCU, MCUs in raster order
    std::uint8_t frame_marker = 0xc0;
    bool chroma_tables = true;
};

void segment(std::vector<std::uint8_t> &out, std::uint8_t marker, const std::vector<std::uint8_t> &body) {
    const auto length = body.size() + 2;
    out.insert(out.end(), {0xff, marker, std::uint8_t(length >> 8), std::uint8_t(length)});
    out.insert(out.end(), body.begin(), body.end());
}

std::vector<std::uint8_t> encode(const Frame &frame) {
    std::vector<std::uint8_t> out{0xff, 0xff, 0xd8};
    segment(out, 0xe0, {'T', 'H', 'P'});
    std::vector<std::uint8_t> body;
    for (unsigned id = 0; id < 2; ++id) {
        body.push_back(std::uint8_t(id));
        for (unsigned k = 0; k < 64; ++k)
            body.push_back(std::uint8_t(quantizer(id, k)));
    }
    segment(out, 0xdb, body);
    for (unsigned id = 0; id < (frame.chroma_tables ? 2u : 1u); ++id)
        for (unsigned kind = 0; kind < 2; ++kind) {
            const auto &table = kind ? ac_tables[id] : dc_tables[id];
            body = {std::uint8_t(kind << 4 | id)};
            for (unsigned length = 1; length <= 16; ++length)
                body.push_back(std::uint8_t(length == table.length ? table.symbols.size() : 0));
            body.insert(body.end(), table.symbols.begin(), table.symbols.end());
            segment(out, 0xc4, body);
        }
    const auto w = frame.width, h = frame.height;
    segment(out, frame.frame_marker,
            {8, std::uint8_t(h >> 8), std::uint8_t(h), std::uint8_t(w >> 8), std::uint8_t(w), 3, 1, 0x22, 0, 2, 0x11,
             1, 3, 0x11, 1});
    if (frame.restart)
        segment(out, 0xdd, {std::uint8_t(frame.restart >> 8), std::uint8_t(frame.restart)});
    segment(out, 0xda, {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});
    const unsigned mcus = w / 16 * ((h + 15) / 16);
    check(frame.blocks.size() == mcus * 6, "test frame block count");
    BitWriter bits;
    int prediction[3] = {};
    for (unsigned m = 0; m < mcus; ++m) {
        for (unsigned b = 0; b < 6; ++b) {
            const unsigned component = b < 4 ? 0 : b - 3, tables = component != 0;
            const auto &block = frame.blocks[m * 6 + b];
            put_value(bits, dc_tables[tables], 0, block.dc - prediction[component]);
            prediction[component] = block.dc;
            unsigned last = 0;
            for (const auto &[k, value] : block.ac) {
                unsigned run = k - last - 1;
                for (; run > 15; run -= 16)
                    bits.put(ac_tables[tables].code(0xf0), ac_tables[tables].length);
                put_value(bits, ac_tables[tables], run, value);
                last = k;
            }
            if (last < 63)
                bits.put(ac_tables[tables].code(0x00), ac_tables[tables].length);
        }
        if (frame.restart && (m + 1) % frame.restart == 0) {
            bits.align();
            prediction[0] = prediction[1] = prediction[2] = 0;
        }
    }
    bits.align();
    out.insert(out.end(), bits.bytes.begin(), bits.bytes.end());
    return out;
}

// T.81 A.3.3 inverse DCT, level-shifted, rounded and clamped.
std::array<int, 64> reference_samples(const Block &block, unsigned component) {
    std::array<double, 64> f{};
    f[0] = double(block.dc) * quantizer(component, 0);
    for (const auto &[k, value] : block.ac)
        f[natural_order[k]] = double(value) * quantizer(component, k);
    const double pi = std::acos(-1.0);
    std::array<int, 64> samples;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            double sum = 0;
            for (unsigned v = 0; v < 8; ++v)
                for (unsigned u = 0; u < 8; ++u)
                    sum += (u ? 1 : std::sqrt(0.5)) * (v ? 1 : std::sqrt(0.5)) * f[v * 8 + u] *
                           std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
            samples[y * 8 + x] = std::clamp(int(std::lround(sum / 4 + 128)), 0, 255);
        }
    return samples;
}

std::size_t tiled(unsigned x, unsigned y, unsigned width) {
    return std::size_t(y / 4) * width * 4 + x / 8 * 32 + y % 4 * 8 + x % 8;
}

void expect(const Frame &frame, const Planes &out, int tolerance) {
    const unsigned columns = frame.width / 16, rows = (frame.height + 15) / 16;
    const std::array<unsigned, 3> width{frame.width, frame.width / 2, frame.width / 2};
    std::array<std::vector<int>, 3> samples;
    for (unsigned i = 0; i < 3; ++i)
        samples[i].resize(std::size_t(width[i]) * rows * (i ? 8 : 16));
    for (unsigned m = 0; m < columns * rows; ++m)
        for (unsigned b = 0; b < 6; ++b) {
            const unsigned component = b < 4 ? 0 : b - 3, size = component ? 8 : 16;
            const unsigned x = m % columns * size + (b < 4 ? b % 2 * 8 : 0);
            const unsigned y = m / columns * size + (b < 4 ? b / 2 * 8 : 0);
            const auto block = reference_samples(frame.blocks[m * 6 + b], component);
            for (unsigned row = 0; row < 8; ++row)
                for (unsigned column = 0; column < 8; ++column)
                    samples[component][std::size_t(y + row) * width[component] + x + column] = block[row * 8 + column];
        }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned y = 0; y < frame.height >> (i != 0); ++y)
            for (unsigned x = 0; x < width[i]; ++x) {
                const int decoded = out.planes[i].data[tiled(x, y, width[i])];
                check(std::abs(decoded - samples[i][std::size_t(y) * width[i] + x]) <= tolerance,
                      "decoded THP sample differs from the reference IDCT");
            }
    out.canaries();
}

void decode_frame(const Frame &frame, Planes &out) {
    const auto bytes = encode(frame);
    platform::ThpVideoDecoder decoder;
    decoder.decode({bytes.data(), bytes.size()}, frame.width, frame.height, out.planes);
}

Block detailed(int dc, int scale) {
    return {dc, {{1, 3 * scale}, {2, 21 * scale}, {6, 2 * scale}, {23, -100 * scale}, {43, 2}, {63, -3 * scale}}};
}

void synthetic() {
    // Flat blocks take the DC-only path; their samples are exact, including clamping.
    Frame flat{32, 16};
    for (int i = 0; i < 12; ++i)
        flat.blocks.push_back({i * 37 % 300 - 150, {}});
    Planes flat_out(32, 16);
    decode_frame(flat, flat_out);
    expect(flat, flat_out, 0);

    // Every run/size path: values resolved with their code, values read after it, long
    // codes, zero runs of 16 and a coefficient in the last position.
    Frame detail{16, 16};
    for (int i = 0; i < 6; ++i)
        detail.blocks.push_back(detailed(i * 11 - 30, i % 2 ? 1 : -1));
    Planes detail_out(16, 16);
    decode_frame(detail, detail_out);
    expect(detail, detail_out, 1);

    // Restarts realign after every third MCU and reset the DC predictions.
    Frame restart{32, 32, 3};
    for (int i = 0; i < 24; ++i)
        restart.blocks.push_back(i % 3 ? detailed(i * 5 - 60, 1) : Block{i - 12, {}});
    Planes restart_out(32, 32);
    decode_frame(restart, restart_out);
    expect(restart, restart_out, 1);

    // An 8-line frame still codes a whole MCU row; only its visible rows are published.
    Frame short_frame{16, 8};
    for (int i = 0; i < 6; ++i)
        short_frame.blocks.push_back(detailed(40 - i * 13, 1));
    Planes short_out(16, 8);
    decode_frame(short_frame, short_out);
    expect(short_frame, short_out, 1);

    auto malformed = [](std::vector<std::uint8_t> bytes, unsigned width, unsigned height) {
        Planes out(width, height);
        platform::ThpVideoDecoder decoder;
        rejects([&] { decoder.decode({bytes.data(), bytes.size()}, width, height, out.planes); });
        out.canaries();
        for (const auto &p : out.storage)
            for (auto value : p)
                check(value == 0xa9, "failed frame must not publish output");
    };
    auto truncated = encode(detail);
    truncated.resize(truncated.size() - 2);
    malformed(truncated, 16, 16);
    malformed(encode(detail), 16, 32);
    Frame overflow = detail;
    overflow.blocks[0].ac.clear();
    for (unsigned k = 1; k <= 64; ++k)
        overflow.blocks[0].ac.push_back({k, 1});
    malformed(encode(overflow), 16, 16);
    Frame tables = detail;
    tables.chroma_tables = false;
    malformed(encode(tables), 16, 16);
    Frame progressive = detail;
    progressive.frame_marker = 0xc2;
    malformed(encode(progressive), 16, 16);
}
} // namespace
#ifdef STARLET_THP_SDK_TESTS
std::vector<std::pair<const void *, std::size_t>> reported;

// THPVideoDecode reports the planes it wrote, as the SDK's DMA bypassed the data cache, and
// nothing when it decodes no frame.
void sdk_reports_planes() {
    Frame frame{32, 16};
    for (int i = 0; i < 12; ++i)
        frame.blocks.push_back(detailed(i * 9 - 50, 1));
    auto bytes = encode(frame);
    bytes.resize(bytes.size() + 65536); // the SDK's call gets no length; keep its reads in bounds
    std::vector<std::uint8_t> y(32 * 16), u(16 * 8), v(16 * 8), work(64);
    starlet::os::set_cpu_write_observer(
        [](const void *data, std::size_t size) { reported.emplace_back(data, size); });
    check(THPInit() != 0, "THPInit failed");
    check(THPVideoDecode(bytes.data(), y.data(), u.data(), v.data(), work.data()) == 0, "THPVideoDecode failed");
    const std::vector<std::pair<const void *, std::size_t>> planes{
        {y.data(), y.size()}, {u.data(), u.size()}, {v.data(), v.size()}};
    check(reported == planes, "THPVideoDecode must report each plane it wrote");
    reported.clear();
    check(THPVideoDecode(bytes.data(), y.data(), u.data(), v.data(), nullptr) != 0, "missing work area accepted");
    check(reported.empty(), "a frame not decoded must report no writes");
    starlet::os::set_cpu_write_observer(nullptr);
}
#endif

int main() {
    try {
        bounds();
        synthetic();
#ifdef STARLET_THP_SDK_TESTS
        sdk_reports_planes();
#endif
        std::puts("THP video: bounds and synthetic frames passed");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "THP video: %s\n", e.what());
        return 1;
    }
}
