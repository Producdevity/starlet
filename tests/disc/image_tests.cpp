#include "disc_fixture.hpp"
#include "starlet/disc/image.hpp"
#include "starlet/disc/sha1.hpp"

#include <atomic>
#include <thread>

using namespace starlet::disc;
using namespace starlet::disc::fixture;

namespace {
std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}

void sha1_vectors() {
    Sha1 empty;
    require(empty.hex() == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of empty input");
    require(sha1_hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc");
    Sha1 million;
    const std::string chunk(1000, 'a');
    for (unsigned i = 0; i < 1000; ++i) million.update(chunk.data(), 7), million.update(chunk.data() + 7, chunk.size() - 7);
    require(million.hex() == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA-1 of a million a across splits");
}

// The spec's other formulation: bits 16-23 of every word replaced by bits 18-25 once after
// seeding, then plain big-endian words.
Bytes reference_junk(const std::uint8_t* seed, std::size_t size) {
    std::uint32_t buffer[521];
    for (unsigned i = 0; i < 17; ++i) buffer[i] = be32(seed + i * 4);
    for (unsigned i = 17; i < 521; ++i) buffer[i] = (buffer[i - 17] << 23) ^ (buffer[i - 16] >> 9) ^ buffer[i - 1];
    for (auto& word : buffer) word = (word & 0xff00ffffu) | ((word >> 2) & 0x00ff0000u);
    const auto forward = [&] {
        for (unsigned i = 0; i < 32; ++i) buffer[i] ^= buffer[i + 521 - 32];
        for (unsigned i = 32; i < 521; ++i) buffer[i] ^= buffer[i - 32];
    };
    for (unsigned i = 0; i < 4; ++i) forward();
    Bytes out;
    for (std::size_t word = 0; out.size() < size; ++word) {
        if (word == 521) { forward(); word = 0; }
        for (unsigned b = 0; b < 4 && out.size() < size; ++b) out.push_back(static_cast<std::uint8_t>(buffer[word] >> (24 - 8 * b)));
    }
    return out;
}

void junk_generator() {
    std::uint8_t seed[68];
    for (unsigned i = 0; i < 68; ++i) seed[i] = static_cast<std::uint8_t>(i * 7 + 1);
    JunkGenerator full(seed);
    Bytes stream(0x8000);
    full.generate(stream.data(), 1000);
    full.generate(stream.data() + 1000, stream.size() - 1000);
    require(stream == reference_junk(seed, stream.size()), "padding matches the spec's other formulation over a 32 KiB block");
    for (const std::size_t at : {std::size_t{1}, std::size_t{2083}, std::size_t{2084}, std::size_t{2085}, std::size_t{0x7c01}}) {
        JunkGenerator skipped(seed);
        skipped.skip(at);
        Bytes tail(stream.size() - at);
        skipped.generate(tail.data(), tail.size());
        require(std::equal(tail.begin(), tail.end(), stream.begin() + static_cast<std::ptrdiff_t>(at)),
                "skipping " + std::to_string(at) + " bytes matches generating them");
    }
}

// Reads the image in uneven pieces and at pseudo-random offsets and sizes.
void same_bytes(DiscImage& image, const Bytes& expected, const std::string& what) {
    require(image.size() == expected.size(), what + ": disc size");
    Bytes got(expected.size());
    for (std::size_t at = 0, step = 7; at < expected.size(); step = step * 3 % 70001 + 1) {
        const auto n = std::min(step, expected.size() - at);
        image.read(at, got.data() + at, n);
        at += n;
    }
    require(got == expected, what + ": sequential reads");
    std::uint32_t x = 12345;
    Bytes piece;
    for (unsigned i = 0; i < 800; ++i) {
        x = x * 1103515245u + 12345u;
        const std::uint64_t offset = (x >> 4) % expected.size();
        x = x * 1103515245u + 12345u;
        const std::size_t size = std::min<std::size_t>((x >> 8) % (i % 8 == 0 ? 0x30000 : 0x900) + 1, expected.size() - offset);
        piece.resize(size);
        image.read(offset, piece.data(), size);
        require(std::equal(piece.begin(), piece.end(), expected.begin() + static_cast<std::ptrdiff_t>(offset)),
                what + ": random read at " + std::to_string(offset) + "+" + std::to_string(size));
    }
    require(failure([&] { image.read(expected.size() - 4, got.data(), 8); }) != "", what + ": a read past the end throws");
}

void gamecube_formats(const Scratch& dir) {
    const GameCubeDisc disc;
    WiaOptions rvz_small, rvz_zstd, rvz_mixed, rvz_large, wia, wia_zstd;
    rvz_zstd.compression = 5;
    rvz_zstd.chunk = 0x10000;
    rvz_mixed = rvz_zstd;
    rvz_mixed.mixed = true;
    rvz_large.chunk = 0x20000;
    wia.rvz = false;
    wia.chunk = 0x200000;
    wia_zstd = wia;
    wia_zstd.compression = 5;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> one_raw{{0x80, GameCubeDisc::size}};
    const std::pair<std::string, std::string> images[] = {
        {disc.iso(dir / "game.iso"), "ISO"},
        {disc.wia(dir / "small.rvz", rvz_small), "RVZ, 32 KiB groups, two raw entries"},
        {disc.wia(dir / "large.rvz", rvz_large, one_raw), "RVZ, 128 KiB groups"},
        {disc.wia(dir / "zstd.rvz", rvz_zstd), "Zstandard RVZ"},
        {disc.wia(dir / "mixed.rvz", rvz_mixed, one_raw), "Zstandard RVZ with stored groups"},
        {disc.wia(dir / "plain.wia", wia), "WIA"},
        {disc.wia(dir / "zstd.wia", wia_zstd, one_raw), "Zstandard WIA"},
    };
    for (const auto& [path, what] : images) {
        const auto image = open_disc_image(path);
        same_bytes(*image, disc.bytes, what);
        Bytes cross(0x100);
        image->read(0x3ff80, cross.data(), cross.size());
        require(cross == disc.span(0x3ff80, 0x100), what + ": a read across raw data entries");
        GameVolume volume(*image);
        require(!volume.wii() && volume.shift() == 0 && volume.size() == GameCubeDisc::size, what + ": GameCube volume");
        require(volume.main_dol() == disc.dol(), what + ": main.dol ends at its furthest section");
        require(volume.fst() == disc.span(GameCubeDisc::fst_at, disc.fst_size), what + ": FST");
    }
    const auto rvz = open_disc_image(dir / "small.rvz");
    auto* wia_image = dynamic_cast<WiaImage*>(rvz.get());
    require(wia_image && wia_image->rvz() && !wia_image->wii() && wia_image->partitions().empty(), "RVZ opened by signature");
    require(failure([&] { wia_image->data_partition(); }) == "a GameCube disc has no partitions", "GameCube has no partitions");
    require(dynamic_cast<IsoImage*>(open_disc_image(dir / "game.iso").get()) != nullptr, "ISO opened by its magic word");

    write_file(dir / "renamed.bin", read_file(dir / "zstd.rvz"));
    require(dynamic_cast<WiaImage*>(open_disc_image(dir / "renamed.bin").get()) != nullptr, "signature wins over the extension");
    write_file(dir / "notes.txt", Bytes(0x40, 'x'));
    require(failure([&] { open_disc_image(dir / "notes.txt"); }) == "notes.txt is not a GameCube or Wii disc image", "other files named");
    write_file(dir / "empty.rvz", {});
    require(failure([&] { open_disc_image(dir / "empty.rvz"); }) == "not a WIA or RVZ disc image", "an empty .rvz is named");
    write_file(dir / "zeros.gcm", Bytes(0x100));
    require(dynamic_cast<IsoImage*>(open_disc_image(dir / "zeros.gcm").get()) != nullptr, "an unrecognised .gcm opens as a copy");
    require(failure([&] { GameVolume volume(*open_disc_image(dir / "zeros.gcm")); }) == "not a GameCube or Wii disc image",
            "a volume needs a disc magic word");
    require(failure([&] { open_disc_image(dir / "missing.iso"); }).rfind("cannot open " + (dir / "missing.iso") + ": ", 0) == 0,
            "a missing image is named");

    IsoImage trimmed(disc.iso(dir / "trimmed.iso", 0x2000));
    Bytes tail(0x10);
    trimmed.read(GameCubeDisc::size - 0x2010, tail.data(), tail.size());
    require(failure([&] { trimmed.read(GameCubeDisc::size - 0x2008, tail.data(), tail.size()); }) ==
                "the disc image ends at 0x5a000, before 0x5a008 (is it a trimmed or incomplete copy?)",
            "a read past a trimmed copy is explained");
}

void wii_formats(const Scratch& dir) {
    const WiiDisc disc;
    WiaOptions rvz, rvz_zstd, wia, wia_zstd;
    rvz_zstd.compression = 5;
    rvz_zstd.chunk = 0x10000;
    rvz_zstd.mixed = true;
    rvz_zstd.exceptions = 3;
    wia.rvz = false;
    wia.chunk = 0x200000;
    wia.exceptions = 2;
    wia_zstd = wia;
    wia_zstd.compression = 5;
    const std::pair<std::string, std::string> images[] = {
        {disc.wia(dir / "wii.rvz", rvz), "Wii RVZ"},
        {disc.wia(dir / "wii-zstd.rvz", rvz_zstd), "Wii Zstandard RVZ with hash exceptions"},
        {disc.wia(dir / "wii.wia", wia), "Wii WIA with padded hash exceptions"},
        {disc.wia(dir / "wii-zstd.wia", wia_zstd), "Wii Zstandard WIA"},
    };
    for (const auto& [path, what] : images) {
        WiaImage image(path);
        require(image.wii() && image.partitions().size() == 1 && image.size() == disc.disc.size(), what + ": parses");
        const auto& p = image.partitions()[0];
        require(p.data[0].first_sector == WiiDisc::data / 0x8000 && p.data[0].sectors == WiiDisc::split &&
                    p.data[1].first_sector == WiiDisc::data / 0x8000 + WiiDisc::split,
                what + ": both partition data segments");
        require(image.data_partition() == 0, what + ": DATA partition found through the disc partition table");
        require(image.partition_data_size(0) == disc.part.size(), what + ": partition data size");
        Bytes all(disc.part.size());
        image.read_partition(0, 0, all.data(), all.size());
        require(all == disc.part, what + ": partition data across segments, groups, exception lists and padding");
        std::uint32_t x = 99;
        for (unsigned i = 0; i < 300; ++i) {
            x = x * 1103515245u + 12345u;
            const std::uint64_t offset = (x >> 4) % all.size();
            x = x * 1103515245u + 12345u;
            const std::size_t size = std::min<std::size_t>((x >> 8) % 0x9000 + 1, all.size() - offset);
            Bytes piece(size);
            image.read_partition(0, offset, piece.data(), size);
            require(std::equal(piece.begin(), piece.end(), disc.part.begin() + static_cast<std::ptrdiff_t>(offset)),
                    what + ": random partition read");
        }
        Bytes raw(WiiDisc::data);
        image.read(0, raw.data(), raw.size());
        require(std::equal(raw.begin(), raw.end(), disc.disc.begin()), what + ": raw disc bytes before the partition data");
        require(failure([&] { image.read(WiiDisc::data + 0x10, raw.data(), 4); }) ==
                    "disc offset 0x70010 holds Wii partition data; read it by partition",
                what + ": partition data is not plain disc bytes");
        require(failure([&] { image.read_partition(0, all.size() - 2, raw.data(), 4); }) ==
                    "read past the end of the partition data at 0x3dffe",
                what + ": a read past the partition data throws");
        require(failure([&] { image.read_partition(1, 0, raw.data(), 4); }) == "no partition 1 in the disc image",
                what + ": partition index checked");
        GameVolume volume(image);
        require(volume.wii() && volume.shift() == 2 && volume.size() == disc.part.size(), what + ": Wii volume");
        require(volume.main_dol() == disc.dol() && volume.fst() == disc.fst(), what + ": main.dol and FST from the partition");
        require(open_disc_image(path)->size() == disc.disc.size(), what + ": opened by signature");
    }
    write_file(dir / "wii.iso", disc.disc);
    const auto iso = open_disc_image(dir / "wii.iso");
    require(failure([&] { GameVolume volume(*iso); }) ==
                "this Wii disc image is a plain copy, which keeps the game encrypted; convert it to RVZ with Dolphin",
            "a Wii ISO is refused with a way forward");
}

void rejected(const Scratch& dir) {
    const GameCubeDisc disc;
    WiaOptions options;
    options.damage_header = true;
    require(failure([&] { WiaImage(disc.wia(dir / "damaged.rvz", options)); }) == "disc image header is damaged",
            "a damaged header is rejected");
    options = {};
    options.wrong_size = true;
    require(failure([&] { WiaImage(disc.wia(dir / "short.rvz", options)); }) ==
                "disc image size fields are inconsistent (incomplete copy?)",
            "an incomplete copy is rejected");
    const std::pair<std::uint32_t, const char*> methods[] = {{1, "PURGE"}, {2, "bzip2"}, {3, "LZMA"}, {4, "LZMA2"}, {9, "unknown (9)"}};
    for (const auto& [method, name] : methods) {
        for (const bool rvz : {false, true}) {
            options = {};
            options.rvz = rvz;
            options.chunk = 0x200000;
            options.compression = method;
            const auto path = disc.wia(dir / "method.img", options);
            require(failure([&] { WiaImage image(path); }) == std::string("the disc image uses ") + name +
                                                                 " compression; convert it with Dolphin to RVZ with "
                                                                 "Zstandard or no compression",
                    std::string("compression ") + name + " is named with a way forward");
        }
    }
    options = {};
    options.chunk = 0x18000;
    require(failure([&] { WiaImage(disc.wia(dir / "chunk.rvz", options)); }) == "disc image chunk size 0x18000 is not supported",
            "a chunk size that is not a power of two is rejected");
    options.rvz = false;
    options.chunk = 0x8000;
    require(failure([&] { WiaImage(disc.wia(dir / "chunk.wia", options)); }) == "disc image chunk size 0x8000 is not supported",
            "WIA needs 2 MiB chunks");

    options = {};
    options.compression = 5;
    const auto path = disc.wia(dir / "cut-group.rvz", options);
    auto bytes = read_file(path);
    // Group 0 is a Zstandard frame at 0x400.
    bytes[0x400] ^= 0xff;
    write_file(path, bytes);
    WiaImage damaged(path);
    Bytes out(0x100);
    require(failure([&] { damaged.read(0x100, out.data(), out.size()); }) == "disc image data is damaged (Zstandard)",
            "a damaged Zstandard group is reported");
    write_file(dir / "text.wia", Bytes(0x100, 'w'));
    require(failure([&] { WiaImage image(dir / "text.wia"); }) == "not a WIA or RVZ disc image", "other data is not WIA");
}

void concurrent_reads(const Scratch& dir) {
    const GameCubeDisc disc;
    WiaOptions options;
    options.compression = 5;
    options.chunk = 0x8000;
    WiaImage image(disc.wia(dir / "threads.rvz", options));
    std::atomic<unsigned> mismatches{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            std::uint32_t x = 7 + t;
            Bytes piece;
            for (unsigned i = 0; i < 400; ++i) {
                x = x * 1103515245u + 12345u;
                const std::uint64_t offset = (x >> 4) % disc.bytes.size();
                const std::size_t size = std::min<std::size_t>(0x1000, disc.bytes.size() - offset);
                piece.resize(size);
                image.read(offset, piece.data(), size);
                if (!std::equal(piece.begin(), piece.end(), disc.bytes.begin() + static_cast<std::ptrdiff_t>(offset))) ++mismatches;
            }
        });
    for (auto& thread : threads) thread.join();
    require(mismatches == 0, "reads from four threads share one image");
}
}

int main() {
    sha1_vectors();
    junk_generator();
    const Scratch dir("starlet-disc-image");
    gamecube_formats(dir);
    wii_formats(dir);
    rejected(dir);
    concurrent_reads(dir);
    std::puts("disc images: ISO, WIA and RVZ (stored and Zstandard) read back byte-identical for GameCube and Wii");
    return 0;
}
