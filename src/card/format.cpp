#include "starlet/card/format.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace starlet::card {
namespace {

std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }
void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v >> 8); p[1] = std::uint8_t(v); }
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (24 - 8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = std::uint8_t(v >> (56 - 8 * i));
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

// OSTime arithmetic in CARDFormat and VerifyID: a signed 64-bit linear congruence
// shifted right arithmetically.
std::int64_t next(std::int64_t r) { return std::int64_t(std::uint64_t(r) * 1103515245u + 12345u) >> 16; }

// Offsets in the ID block (CARDID), the directory block's check and the FAT.
constexpr std::size_t id_device = 0x20, id_size = 0x22, id_encode = 0x24, id_sum = 0x1FC;
constexpr std::size_t dir_check_code = 0x1FFA, dir_sum = 0x1FFC;
constexpr std::size_t fat_check_code = 4, fat_free = 6, fat_last = 8;
constexpr std::uint16_t fat_chain_end = 0xFFFF;

bool checksummed(const std::uint8_t* block, std::size_t length, std::size_t at) {
    std::uint16_t sum, inverse;
    checksum(block, length, sum, inverse);
    return get16(block + at) == sum && get16(block + at + 2) == inverse;
}

}

bool valid_size(int size_mbits) {
    return size_mbits == 4 || size_mbits == 8 || size_mbits == 16 || size_mbits == 32 || size_mbits == 64 || size_mbits == 128;
}

std::string Entry::game() const { return std::string(reinterpret_cast<const char*>(bytes.data()), 4); }
std::string Entry::company() const { return std::string(reinterpret_cast<const char*>(bytes.data() + 4), 2); }
std::string Entry::name() const {
    const char* p = reinterpret_cast<const char*>(bytes.data() + 8);
    return std::string(p, strnlen(p, name_size));
}
bool Entry::same_file(const Entry& other) const {
    return std::memcmp(bytes.data(), other.bytes.data(), 6) == 0 && name() == other.name();
}
bool Entry::name_is(const char* name) const {
    for (std::size_t i = 0; i < name_size; ++i) {
        if (bytes[8 + i] != static_cast<std::uint8_t>(name[i])) return false;
        if (!name[i]) return true;
    }
    return name[name_size] == '\0';
}
void Entry::set_owner(const char game[4], const char company[2]) {
    std::memcpy(bytes.data(), game, 4);
    std::memcpy(bytes.data() + 4, company, 2);
}
void Entry::set_name(const char* name) {
    std::memset(bytes.data() + 8, 0, name_size);
    std::memcpy(bytes.data() + 8, name, strnlen(name, name_size));
}

std::vector<std::uint8_t> write_gci(const File& file) {
    std::vector<std::uint8_t> out(entry_size + file.data.size());
    std::memcpy(out.data(), file.entry.bytes.data(), entry_size);
    if (!file.data.empty()) std::memcpy(out.data() + entry_size, file.data.data(), file.data.size());
    return out;
}

std::optional<File> read_gci(const std::uint8_t* bytes, std::size_t size) {
    if (size < entry_size) return std::nullopt;
    File file;
    std::memcpy(file.entry.bytes.data(), bytes, entry_size);
    const std::size_t length = file.entry.length();
    if (!file.entry.in_use() || length == 0 || size != entry_size + length * block_size) return std::nullopt;
    file.data.assign(bytes + entry_size, bytes + size);
    return file;
}

std::string gci_name(const Entry& e) {
    const std::string raw = e.company() + "-" + e.game() + "-" + e.name();
    std::string name;
    for (const char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte >= 0x7F || std::strchr("\"*/:<>?\\|", c)) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "__%02x__", byte);
            name += escaped;
        } else {
            name += c;
        }
    }
    return name + ".gci";
}

void checksum(const std::uint8_t* bytes, std::size_t length, std::uint16_t& sum, std::uint16_t& inverse) {
    sum = inverse = 0;
    for (std::size_t i = 0; i + 1 < length; i += 2) {
        const std::uint16_t w = get16(bytes + i);
        sum = std::uint16_t(sum + w);
        inverse = std::uint16_t(inverse + std::uint16_t(~w));
    }
    if (sum == 0xFFFF) sum = 0;
    if (inverse == 0xFFFF) inverse = 0;
}

std::array<std::uint8_t, 32> serial_bytes(const Identity& id) {
    std::array<std::uint8_t, 32> s{};
    std::int64_t rand = std::int64_t(id.format_time);
    for (int i = 0; i < 12; ++i) {
        rand = next(rand);
        s[i] = std::uint8_t(id.flash_id[i] + std::uint8_t(rand));
        rand = next(rand) & 0x7FFF;
    }
    put64(s.data() + 12, id.format_time);
    put32(s.data() + 20, id.counter_bias);
    put32(s.data() + 24, id.language);
    put32(s.data() + 28, id.vi_dtv);
    return s;
}

std::uint64_t serial_number(const std::array<std::uint8_t, 32>& s) {
    std::uint64_t n = 0;
    for (int i = 0; i < 4; ++i) n ^= get64(s.data() + 8 * i);
    return n;
}

std::array<std::uint8_t, 12> flash_id_of(const std::uint8_t* serial) {
    std::array<std::uint8_t, 12> flash{};
    std::int64_t rand = std::int64_t(get64(serial + 12));
    for (int i = 0; i < 12; ++i) {
        rand = next(rand);
        flash[i] = std::uint8_t(serial[i] - std::uint8_t(rand));
        rand = next(rand) & 0x7FFF;
    }
    return flash;
}

std::uint8_t flash_id_check(const std::array<std::uint8_t, 12>& flash) {
    std::uint8_t sum = 0;
    for (const auto b : flash) sum = std::uint8_t(sum + b);
    return std::uint8_t(~sum);
}

std::uint64_t os_time(std::int64_t unix_seconds, std::int64_t utc_offset_seconds) {
    const std::int64_t since_2000 = unix_seconds + utc_offset_seconds - 946684800;
    return std::uint64_t(std::max<std::int64_t>(since_2000, 0)) * 40500000u;
}

std::uint32_t card_time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return std::uint32_t(std::int64_t(now) + local.tm_gmtoff - 946684800);
}

std::optional<std::vector<std::uint8_t>> build_image(int size_mbits, const Identity& id, const std::vector<File>& files) {
    if (!valid_size(size_mbits) || files.size() > std::size_t(max_files)) return std::nullopt;
    const int blocks = blocks_for(size_mbits);
    std::size_t needed = 0;
    for (const File& f : files) {
        if (f.entry.length() == 0 || f.data.size() != std::size_t(f.entry.length()) * block_size) return std::nullopt;
        needed += f.entry.length();
    }
    if (needed > std::size_t(blocks - system_blocks)) return std::nullopt;

    std::vector<std::uint8_t> image(std::size_t(blocks) * block_size, 0xFF);
    std::uint8_t* header = image.data();
    const auto serial = serial_bytes(id);
    std::memcpy(header, serial.data(), serial.size());
    put16(header + id_device, 0);
    put16(header + id_size, std::uint16_t(size_mbits));
    put16(header + id_encode, std::uint16_t(id.encoding));
    std::uint16_t sum, inverse;
    checksum(header, id_sum, sum, inverse);
    put16(header + id_sum, sum);
    put16(header + id_sum + 2, inverse);

    std::vector<std::uint8_t> directory(block_size, 0xFF);
    std::vector<std::uint8_t> fat(block_size, 0x00);
    std::uint16_t block = system_blocks;
    for (std::size_t i = 0; i < files.size(); ++i) {
        Entry e = files[i].entry;
        e.set_first_block(block);
        std::memcpy(directory.data() + i * entry_size, e.bytes.data(), entry_size);
        for (std::uint16_t n = 0; n < e.length(); ++n, ++block) {
            put16(fat.data() + 2 * block, n + 1 < e.length() ? std::uint16_t(block + 1) : fat_chain_end);
            std::memcpy(image.data() + std::size_t(block) * block_size, files[i].data.data() + std::size_t(n) * block_size, block_size);
        }
    }
    put16(fat.data() + fat_free, std::uint16_t(blocks - system_blocks - needed));
    put16(fat.data() + fat_last, std::uint16_t(block - 1));
    // Both copies: check codes 0 and 1, as CARDFormat leaves them.
    for (int copy = 0; copy < 2; ++copy) {
        std::uint8_t* d = image.data() + std::size_t(1 + copy) * block_size;
        std::memcpy(d, directory.data(), block_size);
        put16(d + dir_check_code, std::uint16_t(copy));
        checksum(d, dir_sum, sum, inverse);
        put16(d + dir_sum, sum);
        put16(d + dir_sum + 2, inverse);
        std::uint8_t* f = image.data() + std::size_t(3 + copy) * block_size;
        std::memcpy(f, fat.data(), block_size);
        put16(f + fat_check_code, std::uint16_t(copy));
        checksum(f + fat_check_code, block_size - 4, sum, inverse);
        put16(f, sum);
        put16(f + 2, inverse);
    }
    return image;
}

Parsed parse_image(const std::uint8_t* image, std::size_t size, const std::array<std::uint8_t, 12>& flash_id, Encoding console) {
    Parsed out;
    if (size < std::size_t(blocks_for(4)) * block_size || size % block_size != 0) return out;
    if (std::all_of(image, image + size, [](std::uint8_t b) { return b == 0xFF; })) {
        out.result = no_card;  // Unformatted.
        return out;
    }
    const std::uint8_t* header = image;
    std::memcpy(out.serial.data(), header, 32);
    out.size_mbits = get16(header + id_size);
    out.encoding = Encoding(get16(header + id_encode));
    const int blocks = blocks_for(out.size_mbits);
    if (get16(header + id_device) != 0 || !valid_size(out.size_mbits) || std::size_t(blocks) * block_size != size ||
        !checksummed(header, id_sum, id_sum) || flash_id_of(header) != flash_id)
        return out;

    // The newer valid copy of each; VerifyDir and VerifyFAT compare check codes as s16.
    auto pick = [&](std::size_t first, auto valid, std::size_t code_at) -> const std::uint8_t* {
        const std::uint8_t* c[2] = {image + first * block_size, image + (first + 1) * block_size};
        const bool ok[2] = {valid(c[0]), valid(c[1])};
        if (ok[0] && ok[1]) return std::int16_t(get16(c[0] + code_at) - get16(c[1] + code_at)) < 0 ? c[1] : c[0];
        return ok[0] ? c[0] : ok[1] ? c[1] : nullptr;
    };
    const std::uint8_t* dir = pick(1, [](const std::uint8_t* d) { return checksummed(d, dir_sum, dir_sum); }, dir_check_code);
    const std::uint8_t* fat = pick(3, [&](const std::uint8_t* f) {
        std::uint16_t sum, inverse;
        checksum(f + fat_check_code, block_size - 4, sum, inverse);
        if (get16(f) != sum || get16(f + 2) != inverse) return false;
        int free_count = 0;
        for (int b = system_blocks; b < blocks; ++b) free_count += get16(f + 2 * b) == 0;
        return free_count == get16(f + fat_free);
    }, fat_check_code);
    if (!dir || !fat) return out;

    for (int i = 0; i < max_files; ++i) {
        File file;
        std::memcpy(file.entry.bytes.data(), dir + std::size_t(i) * entry_size, entry_size);
        if (!file.entry.in_use()) continue;
        std::uint16_t b = file.entry.first_block();
        for (std::uint16_t n = 0; n < file.entry.length(); ++n) {
            if (b < system_blocks || b >= blocks) return out;
            file.data.insert(file.data.end(), image + std::size_t(b) * block_size, image + std::size_t(b + 1) * block_size);
            b = get16(fat + 2 * b);
        }
        out.files.push_back(std::move(file));
    }
    out.result = out.encoding == console ? ready : encoding;
    return out;
}

}
