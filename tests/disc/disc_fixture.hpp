// Synthetic GameCube and Wii discs, written as ISO, WIA or RVZ (Dolphin's docs/WiaAndRvz.md)
// with valid header, disc and partition-table hashes.
#pragma once

#include "starlet/disc/image.hpp"
#include "starlet/disc/sha1.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ftw.h>
#include <iterator>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace starlet::disc::fixture {

using Bytes = std::vector<std::uint8_t>;

inline void require(bool value, const std::string& why) {
    if (!value) {
        std::fprintf(stderr, "disc: %s\n", why.c_str());
        std::exit(1);
    }
}
template <class F> std::string failure(F&& f) {
    try {
        f();
    } catch (const DiscError& e) {
        return e.what();
    }
    return {};
}

inline void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
inline void put64(Bytes& b, std::size_t at, std::uint64_t v) {
    put32(b, at, static_cast<std::uint32_t>(v >> 32));
    put32(b, at + 4, static_cast<std::uint32_t>(v));
}
inline void append16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void append32(Bytes& b, std::uint32_t v) {
    b.resize(b.size() + 4);
    put32(b, b.size() - 4, v);
}
inline void append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }
inline void fill(Bytes& b, std::size_t at, std::size_t size, unsigned salt) {
    for (std::size_t i = 0; i < size; ++i) b[at + i] = static_cast<std::uint8_t>((i * 31 + salt) ^ (i >> 7));
}
inline std::string hex(const Bytes& b) { return sha1_hex(b.data(), b.size()); }
inline std::array<std::uint8_t, 20> digest(const std::uint8_t* data, std::size_t size) {
    Sha1 hash;
    hash.update(data, size);
    return hash.finish();
}
inline Bytes slice(const Bytes& b, std::uint64_t at, std::uint64_t size) {
    return Bytes(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + size));
}

inline Bytes read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
inline void write_file(const std::string& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "write " + path);
}
inline bool exists(const std::string& path) { struct stat info {}; return ::lstat(path.c_str(), &info) == 0; }
inline bool is_directory(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// A fresh directory under TMPDIR, removed with its contents when this goes away.
class Scratch {
public:
    explicit Scratch(const char* name) {
        const char* base = std::getenv("TMPDIR");
        std::string pattern = std::string(base && *base ? base : "/tmp") + "/" + name + "-XXXXXX";
        require(::mkdtemp(pattern.data()) != nullptr, "temporary directory");
        root_ = pattern;
    }
    ~Scratch() {
        ::nftw(root_.c_str(), [](const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }, 16,
               FTW_DEPTH | FTW_PHYS);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    std::string operator/(const std::string& name) const { return root_ + "/" + name; }
    const std::string& root() const { return root_; }

private:
    std::string root_;
};

inline Bytes zstd(const Bytes& data) {
    Bytes out(ZSTD_compressBound(data.size()));
    const auto size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
    require(!ZSTD_isError(size), "Zstandard compression");
    out.resize(size);
    return out;
}

// Generated padding at [begin, end) of some data offset space, inside one 0x8000-byte block.
struct JunkRun {
    std::uint64_t begin = 0, end = 0;
    std::array<std::uint8_t, 68> seed{};
};

// Fills data[begin, end) with disc padding, a fresh seed for each 0x8000-byte block as on a
// real disc, and records the runs an RVZ writer packs.
inline void add_junk(Bytes& data, std::uint64_t begin, std::uint64_t end, std::vector<JunkRun>& runs, unsigned salt) {
    while (begin < end) {
        JunkRun run;
        run.begin = begin;
        run.end = std::min(end, (begin | 0x7fff) + 1);
        for (unsigned i = 0; i < run.seed.size(); ++i) run.seed[i] = static_cast<std::uint8_t>(salt * 13 + (begin >> 15) * 7 + i * 5);
        JunkGenerator junk(run.seed.data());
        junk.skip(run.begin % 0x8000);
        junk.generate(data.data() + run.begin, run.end - run.begin);
        runs.push_back(run);
        begin = run.end;
    }
}

struct WiaOptions {
    bool rvz = true;
    std::uint32_t compression = 0;  // stored as given; 5 compresses with Zstandard
    std::uint32_t chunk = 0x8000;
    bool mixed = false;              // RVZ with Zstandard: every other unpacked group stays stored
    bool damage_header = false, wrong_size = false;
    std::uint16_t exceptions = 0;    // hash exceptions in the first list of each partition group
};

struct WiaPartitionSource {
    std::uint32_t first_sector = 0, split = 0;  // sectors in wia_part_data_t 0; the rest are in 1
    const Bytes* data = nullptr;                // decrypted data, 0x7C00 bytes per sector
    std::vector<JunkRun> junk;                  // in partition data offsets
};

struct WiaSource {
    std::uint32_t disc_type = 1;
    const Bytes* disc = nullptr;  // the disc's bytes; partition data areas are not read
    std::uint64_t disc_size = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> raws;  // [begin, end) on the disc
    std::vector<JunkRun> junk;                                  // in disc offsets
    std::vector<WiaPartitionSource> partitions;
};

inline std::string write_wia(const std::string& path, const WiaSource& source, const WiaOptions& o) {
    constexpr std::size_t data_start = 0x400;
    const bool zstd_groups = o.compression == 5;
    Bytes file(data_start), groups;
    std::uint32_t group_count = 0;
    const auto group = [&](const Bytes& unpacked, std::uint64_t data_offset, const std::vector<JunkRun>& junk, std::size_t lists) {
        const bool zero = std::all_of(unpacked.begin(), unpacked.end(), [](std::uint8_t b) { return b == 0; });
        while (file.size() % 4) file.push_back(0);
        const auto index = group_count++;
        if (zero && !(lists && o.exceptions)) {
            append32(groups, 0);
            append32(groups, 0);
            if (o.rvz) append32(groups, 0);
            return;
        }
        Bytes body = unpacked;
        std::uint32_t packed = 0;
        const auto end = data_offset + unpacked.size();
        if (o.rvz && std::any_of(junk.begin(), junk.end(), [&](const JunkRun& r) { return r.begin < end && r.end > data_offset; })) {
            body.clear();
            std::uint64_t at = data_offset;
            const auto literal = [&](std::uint64_t to) {
                if (to <= at) return;
                append32(body, static_cast<std::uint32_t>(to - at));
                body.insert(body.end(), unpacked.begin() + static_cast<std::ptrdiff_t>(at - data_offset),
                            unpacked.begin() + static_cast<std::ptrdiff_t>(to - data_offset));
                at = to;
            };
            for (const auto& run : junk) {
                if (run.end <= data_offset || run.begin >= end) continue;
                literal(std::max(run.begin, data_offset));
                const auto stop = std::min(run.end, end);
                append32(body, 0x80000000u | static_cast<std::uint32_t>(stop - at));
                body.insert(body.end(), run.seed.begin(), run.seed.end());
                at = stop;
            }
            literal(end);
            packed = static_cast<std::uint32_t>(body.size());
        }
        Bytes head;
        for (std::size_t i = 0; i < lists; ++i) {
            const std::uint16_t count = i == 0 ? o.exceptions : 0;
            append16(head, count);
            for (unsigned e = 0; e < count; ++e) {
                append16(head, static_cast<std::uint16_t>(e * 20));
                for (unsigned b = 0; b < 20; ++b) head.push_back(static_cast<std::uint8_t>(0xe0 + b));
            }
        }
        const bool compressed = zstd_groups && (!o.rvz || !o.mixed || packed || index % 2 == 0);
        Bytes stored;
        if (compressed) {
            append(head, body);
            stored = zstd(head);
        } else {
            stored = head;
            while (lists && stored.size() % 4) stored.push_back(0);
            append(stored, body);
        }
        append32(groups, static_cast<std::uint32_t>(file.size() / 4));
        append32(groups, static_cast<std::uint32_t>(stored.size()) | (o.rvz && compressed ? 0x80000000u : 0));
        if (o.rvz) append32(groups, packed);
        append(file, stored);
    };

    Bytes raws;
    for (const auto& [begin, end] : source.raws) {
        const auto first = group_count;
        for (std::uint64_t at = begin & ~std::uint64_t{0x7fff}; at < end; at += o.chunk) {
            const auto stop = std::min<std::uint64_t>(at + o.chunk, end);
            group(slice(*source.disc, at, stop - at), at, source.junk, 0);
        }
        raws.resize(raws.size() + 0x18);
        put64(raws, raws.size() - 0x18, begin);
        put64(raws, raws.size() - 0x10, end - begin);
        put32(raws, raws.size() - 8, first);
        put32(raws, raws.size() - 4, group_count - first);
    }
    Bytes parts;
    const std::uint32_t sectors_per_group = std::max<std::uint32_t>(o.chunk / 0x8000, 1);
    const std::size_t lists = std::max<std::uint32_t>(o.chunk / 0x200000, 1);
    for (const auto& p : source.partitions) {
        const auto sectors = static_cast<std::uint32_t>(p.data->size() / 0x7c00);
        Bytes entry(0x30);
        for (unsigned i = 0; i < 16; ++i) entry[i] = static_cast<std::uint8_t>(0x40 + i);
        const std::pair<std::uint32_t, std::uint32_t> segments[2] = {{0, p.split}, {p.split, sectors}};
        for (unsigned d = 0; d < 2; ++d) {
            const auto [from, to] = segments[d];
            const auto first = group_count;
            for (std::uint32_t s = from; s < to; s += sectors_per_group) {
                const auto count = std::min(sectors_per_group, to - s);
                group(slice(*p.data, std::uint64_t{s} * 0x7c00, std::uint64_t{count} * 0x7c00), std::uint64_t{s} * 0x7c00, p.junk, lists);
            }
            put32(entry, 0x10 + d * 0x10, p.first_sector + from);
            put32(entry, 0x14 + d * 0x10, to - from);
            put32(entry, 0x18 + d * 0x10, first);
            put32(entry, 0x1c + d * 0x10, group_count - first);
        }
        append(parts, entry);
    }

    const auto table = [&](const Bytes& bytes) {
        while (file.size() % 4) file.push_back(0);
        const std::pair<std::uint64_t, Bytes> placed{file.size(), zstd_groups ? zstd(bytes) : bytes};
        append(file, placed.second);
        return placed;
    };
    const auto raw_table = table(raws), group_table = table(groups);
    const std::uint64_t parts_at = file.size();
    append(file, parts);

    const std::size_t d = 0x48;
    put32(file, d, source.disc_type);
    put32(file, d + 4, o.compression);
    put32(file, d + 8, zstd_groups ? 3 : 0);
    put32(file, d + 0x0c, o.chunk);
    std::copy(source.disc->begin(), source.disc->begin() + 0x80, file.begin() + d + 0x10);
    put32(file, d + 0x90, static_cast<std::uint32_t>(source.partitions.size()));
    put32(file, d + 0x94, 0x30);
    put64(file, d + 0x98, parts_at);
    const auto parts_hash = digest(parts.data(), parts.size());
    std::copy(parts_hash.begin(), parts_hash.end(), file.begin() + d + 0xa0);
    put32(file, d + 0xb4, static_cast<std::uint32_t>(source.raws.size()));
    put64(file, d + 0xb8, raw_table.first);
    put32(file, d + 0xc0, static_cast<std::uint32_t>(raw_table.second.size()));
    put32(file, d + 0xc4, group_count);
    put64(file, d + 0xc8, group_table.first);
    put32(file, d + 0xd0, static_cast<std::uint32_t>(group_table.second.size()));
    std::memcpy(file.data(), o.rvz ? "RVZ\x01" : "WIA\x01", 4);
    put32(file, 4, 0x01000000);
    put32(file, 8, o.rvz ? 0x00030000 : 0x00090000);
    put32(file, 0x0c, 0xdc);
    const auto disc_hash = digest(file.data() + d, 0xdc);
    std::copy(disc_hash.begin(), disc_hash.end(), file.begin() + 0x10);
    put64(file, 0x24, source.disc_size);
    put64(file, 0x2c, file.size() + (o.wrong_size ? 1 : 0));
    const auto head_hash = digest(file.data(), 0x34);
    std::copy(head_hash.begin(), head_hash.end(), file.begin() + 0x34);
    if (o.damage_header) file[0x30] ^= 1;
    write_file(path, file);
    return path;
}

// A small GameCube disc: boot.bin, bi2.bin, apploader, main.dol and FST, then files that
// cross 0x8000-byte groups, an empty file and folder, padding, zero areas and a file that
// ends at the end of the disc.
struct GameCubeDisc {
    static constexpr std::uint32_t size = 0x5c000, dol_at = 0x4000, dol_size = 0x380, fst_at = 0x5000,
                                   apploader_size = 0x1220, junk_at = 0x30100, junk_end = 0x38000;
    struct File { const char* path; std::uint32_t offset, length; };
    static constexpr File files[] = {{"files/Data/Sub/zero.bin", 0, 0}, {"files/Data/b.arc", 0x10000, 0x9000},
                                     {"files/a.bin", 0x7ff0, 0x1234}, {"files/c.bin", 0x30000, 0x100},
                                     {"files/d.bin", 0x5a000, 0x2000}};
    // Where the name "c.bin" sits in the FST, for tests that rename it.
    static constexpr std::uint32_t c_name = fst_at + 9 * 12 + 36;
    Bytes bytes = Bytes(size);
    std::uint32_t fst_size = 0;
    std::vector<JunkRun> junk;
    std::string dol_digest;

    explicit GameCubeDisc(const char* id = "GPVE01", std::uint8_t disc = 0, std::uint8_t revision = 0) {
        std::memcpy(bytes.data(), id, 6);
        bytes[6] = disc;
        bytes[7] = revision;
        put32(bytes, 0x1c, 0xc2339f3d);
        std::memcpy(bytes.data() + 0x20, "STARLET TEST", 12);
        fill(bytes, 0x440, 0x2000, 9);
        std::memcpy(bytes.data() + 0x2440, "2004/02/02", 10);
        put32(bytes, 0x2450, 0x81200000);
        put32(bytes, 0x2454, 0x1000);
        put32(bytes, 0x2458, 0x200);
        fill(bytes, 0x2460, apploader_size - 0x20, 11);
        // main.dol: one text and one data section after the 0x100-byte header.
        put32(bytes, dol_at, 0x100);
        put32(bytes, dol_at + 0x48, 0x80003100);
        put32(bytes, dol_at + 0x90, 0x200);
        put32(bytes, dol_at + 0x1c, 0x300);
        put32(bytes, dol_at + 0x64, 0x80400000);
        put32(bytes, dol_at + 0xac, 0x80);
        put32(bytes, dol_at + 0xe0, 0x80003100);
        fill(bytes, dol_at + 0x100, dol_size - 0x100, 13);
        // Root: Data/{Sub/zero.bin, b.arc}, Empty/, a.bin, c.bin, d.bin.
        const char names[] = "Data\0Sub\0zero.bin\0b.arc\0Empty\0a.bin\0c.bin\0d.bin";
        const std::uint32_t count = 9, strings = count * 12;
        Bytes fst(strings + sizeof names);
        const auto entry = [&](unsigned i, std::uint8_t type, std::uint32_t name, std::uint32_t a, std::uint32_t b) {
            put32(fst, i * 12, std::uint32_t{type} << 24 | name);
            put32(fst, i * 12 + 4, a);
            put32(fst, i * 12 + 8, b);
        };
        entry(0, 1, 0, 0, count);
        entry(1, 1, 0, 0, 5);
        entry(2, 1, 5, 1, 4);
        entry(3, 0, 9, files[0].offset, files[0].length);
        entry(4, 0, 18, files[1].offset, files[1].length);
        entry(5, 1, 24, 0, 6);
        entry(6, 0, 30, files[2].offset, files[2].length);
        entry(7, 0, 36, files[3].offset, files[3].length);
        entry(8, 0, 42, files[4].offset, files[4].length);
        std::memcpy(fst.data() + strings, names, sizeof names);
        fst_size = static_cast<std::uint32_t>(fst.size());
        put32(bytes, 0x420, dol_at);
        put32(bytes, 0x424, fst_at);
        put32(bytes, 0x428, fst_size);
        put32(bytes, 0x42c, fst_size);
        std::copy(fst.begin(), fst.end(), bytes.begin() + fst_at);
        for (unsigned i = 1; i < std::size(files); ++i) fill(bytes, files[i].offset, files[i].length, 20 + i);
        add_junk(bytes, junk_at, junk_end, junk, 3);
        dol_digest = hex(dol());
    }
    Bytes span(std::uint32_t offset, std::uint32_t length) const { return slice(bytes, offset, length); }
    Bytes dol() const { return span(dol_at, dol_size); }
    std::map<std::string, std::string> manifest() const {
        std::map<std::string, std::string> m{{"sys/boot.bin", hex(span(0, 0x440))}, {"sys/bi2.bin", hex(span(0x440, 0x2000))},
                                             {"sys/apploader.img", hex(span(0x2440, apploader_size))},
                                             {"sys/fst.bin", hex(span(fst_at, fst_size))}, {"sys/main.dol", hex(dol())}};
        for (const auto& file : files) m[file.path] = hex(span(file.offset, file.length));
        return m;
    }
    std::string iso(const std::string& path, std::uint32_t cut = 0) const {
        write_file(path, Bytes(bytes.begin(), bytes.end() - cut));
        return path;
    }
    std::string wia(const std::string& path, const WiaOptions& options,
                    std::vector<std::pair<std::uint64_t, std::uint64_t>> raws = {{0x80, 0x40000}, {0x40000, size}}) const {
        WiaSource source;
        source.disc = &bytes;
        source.disc_size = size;
        source.raws = std::move(raws);
        source.junk = junk;
        return write_wia(path, source, options);
    }
};

// A small Wii disc: the disc header, partition table at 0x40000 and one DATA partition whose
// header at 0x50000 places its data at 0x70000. The partition data holds boot.bin, bi2.bin,
// apploader, main.dol, FST and files crossing sectors, then padding.
struct WiiDisc {
    static constexpr std::uint32_t partition = 0x50000, data = partition + 0x20000, sectors = 8, split = 3;
    static constexpr std::uint32_t dol_at = 0x2580, dol_size = 0x140, fst_at = 0x3000, apploader_size = 0x120;
    static constexpr std::uint32_t junk_at = 5 * 0x7c00 + 0x100, junk_end = 6 * 0x7c00;
    struct File { const char* path; std::uint32_t offset, length; };
    static constexpr File files[] = {{"files/Data/b.arc", 0x8000, 0x9000}, {"files/a.bin", 0x4000, 0x1234},
                                     {"files/c.bin", 5 * 0x7c00, 0x100}, {"files/z.bin", 0x30000, 0x1000}};
    Bytes disc = Bytes(data + std::uint64_t{sectors} * 0x8000), part = Bytes(sectors * 0x7c00);
    std::uint32_t fst_size = 0;
    std::vector<JunkRun> junk;
    std::string dol_digest;

    explicit WiiDisc(const char* id = "RMGE01") {
        std::memcpy(disc.data(), id, 6);
        put32(disc, 0x18, 0x5d1c9ea3);
        std::memcpy(disc.data() + 0x20, "STARLET WII TEST", 16);
        fill(disc, 0x100, 0x200, 41);
        // One partition group with one DATA partition entry.
        put32(disc, 0x40000, 1);
        put32(disc, 0x40004, 0x40020 >> 2);
        put32(disc, 0x40020, partition >> 2);
        put32(disc, 0x40024, 0);
        fill(disc, partition, 0x2b8, 43);
        put32(disc, partition + 0x2b8, 0x20000 >> 2);
        fill(disc, partition + 0x2c0, 0x1000, 47);
        // Bytes in the partition data area are never stored raw.
        std::fill(disc.begin() + data, disc.end(), 0xee);

        std::memcpy(part.data(), id, 6);
        put32(part, 0x18, 0x5d1c9ea3);
        fill(part, 0x440, 0x2000, 51);
        std::memcpy(part.data() + 0x2440, "2007/08/24", 10);
        put32(part, 0x2454, 0x100);
        put32(part, 0x2458, 0);
        fill(part, 0x2460, apploader_size - 0x20, 53);
        put32(part, 0x420, dol_at >> 2);
        put32(part, dol_at, 0x100);
        put32(part, dol_at + 0x48, 0x80004000);
        put32(part, dol_at + 0x90, 0x40);
        for (unsigned i = 0; i < 0x40; ++i) part[dol_at + 0x100 + i] = static_cast<std::uint8_t>(i + 1);
        // Root, Data/ holding b.arc, then a.bin, c.bin and z.bin at the root.
        const char names[] = "Data\0b.arc\0a.bin\0c.bin\0z.bin";
        const std::uint32_t entries = 6, table = entries * 12;
        Bytes fst(table + sizeof names);
        const auto entry = [&](unsigned i, std::uint8_t type, std::uint32_t name, std::uint32_t a, std::uint32_t b) {
            put32(fst, i * 12, std::uint32_t{type} << 24 | name);
            put32(fst, i * 12 + 4, a);
            put32(fst, i * 12 + 8, b);
        };
        entry(0, 1, 0, 0, entries);
        entry(1, 1, 0, 0, 3);
        entry(2, 0, 5, files[0].offset >> 2, files[0].length);
        entry(3, 0, 11, files[1].offset >> 2, files[1].length);
        entry(4, 0, 17, files[2].offset >> 2, files[2].length);
        entry(5, 0, 23, files[3].offset >> 2, files[3].length);
        std::memcpy(fst.data() + table, names, sizeof names);
        fst.resize((fst.size() + 3) & ~std::size_t{3});
        fst_size = static_cast<std::uint32_t>(fst.size());
        put32(part, 0x424, fst_at >> 2);
        put32(part, 0x428, fst_size >> 2);
        std::copy(fst.begin(), fst.end(), part.begin() + fst_at);
        for (unsigned i = 0; i < std::size(files); ++i) fill(part, files[i].offset, files[i].length, 60 + i);
        add_junk(part, junk_at, junk_end, junk, 7);
        dol_digest = hex(dol());
    }
    Bytes dol() const { return slice(part, dol_at, dol_size); }
    Bytes fst() const { return slice(part, fst_at, fst_size); }
    Bytes file(unsigned i) const { return slice(part, files[i].offset, files[i].length); }
    std::map<std::string, std::string> manifest() const {
        std::map<std::string, std::string> m{{"sys/fst.bin", hex(fst())}};
        for (unsigned i = 0; i < std::size(files); ++i) m[files[i].path] = hex(file(i));
        return m;
    }
    std::string wia(const std::string& path, const WiaOptions& options) const {
        WiaSource source;
        source.disc_type = 2;
        source.disc = &disc;
        source.disc_size = disc.size();
        source.raws = {{0x80, partition}, {partition, data}};
        source.partitions.push_back({data / 0x8000, split, &part, junk});
        return write_wia(path, source, options);
    }
};

} // namespace starlet::disc::fixture
