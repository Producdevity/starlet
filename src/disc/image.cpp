#include "starlet/disc/image.hpp"
#include "starlet/disc/sha1.hpp"

#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace starlet::disc {
namespace {
constexpr std::uint32_t gamecube_magic = 0xc2339f3d, wii_magic = 0x5d1c9ea3;
constexpr std::uint32_t compression_none = 0, compression_zstd = 5;
constexpr std::uint64_t sector = 0x8000, sector_data = 0x7c00, hash_block = 0x200000, max_chunk = 64u << 20;
constexpr std::uint64_t max_disc = std::uint64_t{1} << 34;
constexpr std::size_t junk_bytes = 521 * 4, max_exception_list = 2 + 0xffff * 22;

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
std::uint64_t be64(const std::uint8_t* p) { return std::uint64_t{be32(p)} << 32 | be32(p + 4); }

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

bool digest_matches(const std::uint8_t* data, std::size_t size, const std::uint8_t* expected) {
    Sha1 hash;
    hash.update(data, size);
    const auto digest = hash.finish();
    return std::equal(digest.begin(), digest.end(), expected);
}

bool ends_with_lower(const std::string& text, const char* suffix) {
    const auto n = std::strlen(suffix);
    if (text.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        auto c = text[text.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

int open_file(const std::string& path, std::uint64_t& size) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw DiscError("cannot open " + path + ": " + std::strerror(errno));
    struct stat info {};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        ::close(fd);
        throw DiscError(path + " is not a file");
    }
    size = static_cast<std::uint64_t>(info.st_size);
    return fd;
}

void read_at(int fd, std::uint64_t offset, std::uint8_t* output, std::size_t size) {
    while (size) {
        const auto count = ::pread(fd, output, size, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw DiscError(std::string("cannot read the disc image: ") + std::strerror(errno));
        if (count == 0) throw DiscError("the disc image ended while it was read");
        output += count;
        offset += static_cast<std::uint64_t>(count);
        size -= static_cast<std::size_t>(count);
    }
}

std::string compression_name(std::uint32_t method) {
    static const char* const names[] = {"no", "PURGE", "bzip2", "LZMA", "LZMA2", "Zstandard"};
    return method < 6 ? names[method] : "unknown (" + std::to_string(method) + ")";
}
}

IsoImage::IsoImage(const std::string& path) { fd_ = open_file(path, size_); }

IsoImage::~IsoImage() { ::close(fd_); }

void IsoImage::read(std::uint64_t offset, void* output, std::size_t size) {
    if (offset > size_ || size > size_ - offset)
        throw DiscError("the disc image ends at " + hex(size_) + ", before " + hex(offset + size) +
                        " (is it a trimmed or incomplete copy?)");
    read_at(fd_, offset, static_cast<std::uint8_t*>(output), size);
}

JunkGenerator::JunkGenerator(const std::uint8_t* seed) {
    for (unsigned i = 0; i < 17; ++i) buffer_[i] = be32(seed + i * 4);
    for (unsigned i = 17; i < buffer_.size(); ++i)
        buffer_[i] = (buffer_[i - 17] << 23) ^ (buffer_[i - 16] >> 9) ^ buffer_[i - 1];
    for (unsigned i = 0; i < 4; ++i) forward();
}

void JunkGenerator::forward() {
    for (unsigned i = 0; i < 32; ++i) buffer_[i] ^= buffer_[i + buffer_.size() - 32];
    for (unsigned i = 32; i < buffer_.size(); ++i) buffer_[i] ^= buffer_[i - 32];
}

void JunkGenerator::skip(std::size_t bytes) {
    position_ += bytes;
    while (position_ >= junk_bytes) {
        forward();
        position_ -= junk_bytes;
    }
}

void JunkGenerator::generate(std::uint8_t* output, std::size_t bytes) {
    // The second byte of each word comes from bits 18-25, not 16-23.
    static constexpr unsigned shifts[4] = {24, 18, 8, 0};
    while (bytes) {
        const auto take = std::min(bytes, junk_bytes - position_);
        for (std::size_t i = 0; i < take; ++i, ++position_)
            output[i] = static_cast<std::uint8_t>(buffer_[position_ / 4] >> shifts[position_ % 4]);
        output += take;
        bytes -= take;
        if (position_ == junk_bytes) {
            forward();
            position_ = 0;
        }
    }
}

struct WiaImage::State {
    struct Group { std::uint32_t offset4 = 0, size = 0, packed = 0; };
    struct Raw { std::uint64_t offset = 0, size = 0; std::uint32_t group = 0, groups = 0; };

    int fd = -1;
    std::uint64_t file_size = 0, disc_size = 0;
    bool rvz = false, wii = false;
    std::uint32_t compression = 0, chunk_size = 0;
    std::array<std::uint8_t, 0x80> header{};
    std::vector<WiaPartition> partitions;
    std::vector<Raw> raw;
    std::vector<Group> groups;
    ZSTD_DCtx* zstd = nullptr;
    std::optional<std::size_t> data_partition;
    std::mutex mutex;
    std::uint32_t cached_index = ~0u;
    std::vector<std::uint8_t> cached, stored_bytes, decoded_bytes;

    ~State() {
        ZSTD_freeDCtx(zstd);
        if (fd >= 0) ::close(fd);
    }

    void read_file(std::uint64_t offset, std::uint8_t* output, std::size_t size) const {
        if (offset > file_size || size > file_size - offset) throw DiscError("disc image is truncated");
        read_at(fd, offset, output, size);
    }

    // One complete Zstandard frame of at most limit bytes, into output's first bytes; output
    // only grows, so group loads reuse it.
    std::size_t decompress(const std::uint8_t* data, std::size_t size, std::size_t limit, std::vector<std::uint8_t>& output) {
        if (ZSTD_isError(ZSTD_DCtx_reset(zstd, ZSTD_reset_session_only))) throw DiscError("Zstandard reset failed");
        // One spare byte lets the decoder finish a frame that fills the limit exactly.
        const auto capacity = limit + 1;
        const auto first = std::min<std::size_t>(capacity, 0x40000);
        if (output.size() < first) output.resize(first);
        ZSTD_inBuffer in{data, size, 0};
        ZSTD_outBuffer out{output.data(), std::min(output.size(), capacity), 0};
        while (true) {
            const auto status = ZSTD_decompressStream(zstd, &out, &in);
            if (ZSTD_isError(status)) throw DiscError("disc image data is damaged (Zstandard)");
            if (!status) break;
            if (out.pos == out.size) {
                if (out.size >= capacity) throw DiscError("disc image data decompresses beyond its bound");
                const auto grown = std::min(capacity, out.size * 2);
                if (output.size() < grown) output.resize(grown);
                out.dst = output.data();
                out.size = grown;
            } else if (in.pos == in.size) {
                throw DiscError("disc image data is truncated (Zstandard)");
            }
        }
        if (out.pos > limit) throw DiscError("disc image data decompresses beyond its bound");
        return out.pos;
    }

    std::vector<std::uint8_t> table(std::uint64_t offset, std::uint32_t stored, std::size_t expected) {
        if (!expected) return {};
        if (compression == compression_none) {
            if (stored < expected) throw DiscError("disc image table is truncated");
            std::vector<std::uint8_t> bytes(expected);
            read_file(offset, bytes.data(), bytes.size());
            return bytes;
        }
        if (stored > expected * 2 + 0x1000) throw DiscError("disc image table is damaged");
        std::vector<std::uint8_t> bytes(stored), decoded;
        read_file(offset, bytes.data(), bytes.size());
        if (decompress(bytes.data(), bytes.size(), expected, decoded) != expected) throw DiscError("disc image table is truncated");
        decoded.resize(expected);
        return decoded;
    }

    // data_offset is where the group starts: on the disc for raw data, within the partition
    // data for a partition. RVZ padding is generated relative to it.
    const std::vector<std::uint8_t>& group(std::uint32_t index, bool partition, std::uint64_t data_offset,
                                           std::size_t expected) {
        if (index == cached_index) return cached;
        if (index >= groups.size()) throw DiscError("disc image group index is out of range");
        const auto& entry = groups[index];
        const bool compressed = compression != compression_none && (!rvz || entry.size >> 31);
        const std::size_t stored = rvz ? entry.size & 0x7fffffff : entry.size;
        const std::size_t lists = partition ? std::max<std::size_t>(chunk_size / hash_block, 1) : 0;
        const std::size_t bound = std::max<std::size_t>(expected, entry.packed) * 2 + lists * max_exception_list + 0x10000;
        cached_index = ~0u;
        cached.resize(expected);
        if (!stored) {
            std::memset(cached.data(), 0, expected);
            cached_index = index;
            return cached;
        }
        if (stored > bound) throw DiscError("disc image group is damaged");
        if (stored_bytes.size() < stored) stored_bytes.resize(stored);
        read_file(std::uint64_t{entry.offset4} * 4, stored_bytes.data(), stored);
        const std::uint8_t* data = stored_bytes.data();
        std::size_t size = stored;
        if (compressed) {
            size = decompress(stored_bytes.data(), stored, bound, decoded_bytes);
            data = decoded_bytes.data();
        }
        std::size_t at = 0;
        // Hash exceptions only matter when re-creating encrypted sectors.
        for (std::size_t i = 0; i < lists; ++i) {
            if (size - at < 2) throw DiscError("disc image exception list is truncated");
            at += 2 + std::size_t{be16(data + at)} * 22;
            if (at > size) throw DiscError("disc image exception list is truncated");
        }
        if (lists && !compressed) at = std::min((at + 3) & ~std::size_t{3}, size);
        if (rvz && entry.packed) {
            std::size_t out = 0;
            while (out < expected) {
                if (size - at < 4) throw DiscError("disc image packed data is truncated");
                const auto run = be32(data + at);
                at += 4;
                const std::size_t length = run & 0x7fffffff;
                if (length > expected - out) throw DiscError("disc image packed data overruns its group");
                if (run >> 31) {
                    if (size - at < 68) throw DiscError("disc image padding seed is truncated");
                    JunkGenerator junk(data + at);
                    at += 68;
                    junk.skip((data_offset + out) % sector);
                    junk.generate(cached.data() + out, length);
                } else {
                    if (size - at < length) throw DiscError("disc image packed data is truncated");
                    std::memcpy(cached.data() + out, data + at, length);
                    at += length;
                }
                out += length;
            }
        } else {
            if (size - at < expected) throw DiscError("disc image group is truncated");
            std::memcpy(cached.data(), data + at, expected);
        }
        cached_index = index;
        return cached;
    }

    void read_disc(std::uint64_t offset, std::uint8_t* output, std::size_t size) {
        if (offset > disc_size || size > disc_size - offset)
            throw DiscError("read past the end of the disc at " + hex(offset));
        while (size) {
            if (offset < header.size()) {
                const auto take = std::min<std::size_t>(size, header.size() - offset);
                std::memcpy(output, header.data() + offset, take);
                offset += take; output += take; size -= take;
                continue;
            }
            const auto found = std::find_if(raw.begin(), raw.end(), [&](const Raw& r) {
                return offset >= r.offset && offset - r.offset < r.size;
            });
            if (found == raw.end()) {
                for (const auto& p : partitions)
                    for (const auto& d : p.data)
                        if (offset >= d.first_sector * sector && offset - d.first_sector * sector < d.sectors * sector)
                            throw DiscError("disc offset " + hex(offset) + " holds Wii partition data; read it by partition");
                throw DiscError("the disc image does not store disc offset " + hex(offset));
            }
            const auto relative = offset - found->offset;
            const auto index = relative / chunk_size;
            if (index >= found->groups) throw DiscError("disc image raw data group is missing");
            const auto start = index * chunk_size;
            const auto expected = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, found->size - start));
            const auto& data = group(found->group + static_cast<std::uint32_t>(index), false, found->offset + start, expected);
            const auto within = static_cast<std::size_t>(relative - start);
            const auto take = std::min<std::size_t>(size, expected - within);
            std::memcpy(output, data.data() + within, take);
            offset += take; output += take; size -= take;
        }
    }

    std::uint64_t partition_size(std::size_t partition) const {
        if (partition >= partitions.size()) throw DiscError("no partition " + std::to_string(partition) + " in the disc image");
        const auto& p = partitions[partition];
        return (std::uint64_t{p.data[0].sectors} + p.data[1].sectors) * sector_data;
    }

    void read_partition(std::size_t partition, std::uint64_t offset, std::uint8_t* output, std::size_t size) {
        const auto total = partition_size(partition);
        if (offset > total || size > total - offset) throw DiscError("read past the end of the partition data at " + hex(offset));
        const auto& p = partitions[partition];
        const auto sectors_per_group = std::max<std::uint64_t>(chunk_size / sector, 1);
        while (size) {
            const auto index = offset / sector_data;
            const WiaPartitionData* data = nullptr;
            std::uint64_t relative = 0, first = 0;
            for (const auto& d : p.data) {
                first = std::uint64_t{d.first_sector} - p.data[0].first_sector;
                if (d.sectors && index >= first && index - first < d.sectors) {
                    data = &d;
                    relative = index - first;
                    break;
                }
            }
            if (!data) throw DiscError("the disc image does not store partition offset " + hex(offset));
            const auto group_index = relative / sectors_per_group;
            if (group_index >= data->groups) throw DiscError("disc image partition group is missing");
            const auto group_first = group_index * sectors_per_group;
            const auto group_sectors = std::min<std::uint64_t>(sectors_per_group, data->sectors - group_first);
            const auto start = (first + group_first) * sector_data;
            const auto& bytes = group(data->group + static_cast<std::uint32_t>(group_index), true, start,
                                      static_cast<std::size_t>(group_sectors * sector_data));
            const auto within = static_cast<std::size_t>(offset - start);
            const auto take = std::min<std::size_t>(size, bytes.size() - within);
            std::memcpy(output, bytes.data() + within, take);
            offset += take; output += take; size -= take;
        }
    }

    std::size_t find_data_partition() {
        if (!wii) throw DiscError("a GameCube disc has no partitions");
        std::uint8_t info[32];
        read_disc(0x40000, info, sizeof info);
        for (unsigned g = 0; g < 4; ++g) {
            const auto count = be32(info + g * 8);
            if (count > 64) throw DiscError("damaged disc partition table");
            std::vector<std::uint8_t> entries(std::size_t{count} * 8);
            if (count) read_disc(std::uint64_t{be32(info + g * 8 + 4)} << 2, entries.data(), entries.size());
            for (std::uint32_t e = 0; e < count; ++e) {
                if (be32(entries.data() + e * 8 + 4) != 0) continue;
                const auto offset = std::uint64_t{be32(entries.data() + e * 8)} << 2;
                std::uint8_t partition_header[0x2c0];
                read_disc(offset, partition_header, sizeof partition_header);
                const auto data = offset + (std::uint64_t{be32(partition_header + 0x2b8)} << 2);
                for (std::size_t i = 0; i < partitions.size(); ++i)
                    if (std::uint64_t{partitions[i].data[0].first_sector} * sector == data) return i;
                throw DiscError("the disc image lacks the game partition data");
            }
        }
        throw DiscError("the disc has no game partition");
    }
};

WiaImage::WiaImage(const std::string& path) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.fd = open_file(path, s.file_size);
    std::uint8_t head[0x48];
    if (s.file_size < sizeof head) throw DiscError("not a WIA or RVZ disc image");
    s.read_file(0, head, sizeof head);
    if (!std::memcmp(head, "RVZ\x01", 4)) s.rvz = true;
    else if (std::memcmp(head, "WIA\x01", 4)) throw DiscError("not a WIA or RVZ disc image");
    if (!digest_matches(head, 0x34, head + 0x34)) throw DiscError("disc image header is damaged");
    if (be32(head + 8) > 0x01000000) throw DiscError("disc image needs a newer reader");
    const auto disc_struct = be32(head + 0x0c);
    s.disc_size = be64(head + 0x24);
    if (disc_struct < 0xdc || disc_struct > 0x1000 || be64(head + 0x2c) != s.file_size || s.disc_size < s.header.size() ||
        s.disc_size > max_disc)
        throw DiscError("disc image size fields are inconsistent (incomplete copy?)");
    std::vector<std::uint8_t> disc(disc_struct);
    s.read_file(0x48, disc.data(), disc.size());
    if (!digest_matches(disc.data(), disc.size(), head + 0x10)) throw DiscError("disc image description is damaged");
    const auto type = be32(disc.data());
    if (type != 1 && type != 2) throw DiscError("not a GameCube or Wii disc image");
    s.wii = type == 2;
    s.compression = be32(disc.data() + 4);
    s.chunk_size = be32(disc.data() + 0x0c);
    if (s.compression != compression_none && s.compression != compression_zstd)
        throw DiscError("the disc image uses " + compression_name(s.compression) +
                        " compression; convert it with Dolphin to RVZ with Zstandard or no compression");
    if (s.compression == compression_zstd && !(s.zstd = ZSTD_createDCtx())) throw DiscError("cannot allocate the Zstandard decoder");
    const bool small = s.rvz && s.chunk_size >= sector && s.chunk_size < hash_block && !(s.chunk_size & (s.chunk_size - 1));
    if (!small && (!s.chunk_size || s.chunk_size % hash_block || s.chunk_size > max_chunk))
        throw DiscError("disc image chunk size " + hex(s.chunk_size) + " is not supported");
    std::memcpy(s.header.data(), disc.data() + 0x10, s.header.size());

    const auto part_count = be32(disc.data() + 0x90), part_size = be32(disc.data() + 0x94);
    if ((!s.wii && part_count) || part_count > 16 || part_size > 0x100) throw DiscError("damaged partition table");
    const auto raw_count = be32(disc.data() + 0xb4), group_count = be32(disc.data() + 0xc4);
    if (!raw_count || raw_count > 0x10000) throw DiscError("damaged raw data table");
    if (group_count > s.disc_size / s.chunk_size + raw_count + 2 * part_count + 1) throw DiscError("damaged group table");

    if (part_count) {
        std::vector<std::uint8_t> parts(std::size_t{part_count} * part_size);
        s.read_file(be64(disc.data() + 0x98), parts.data(), parts.size());
        if (!digest_matches(parts.data(), parts.size(), disc.data() + 0xa0)) throw DiscError("partition table is damaged");
        for (std::uint32_t i = 0; i < part_count; ++i) {
            std::uint8_t entry[0x30] = {};
            std::memcpy(entry, parts.data() + std::size_t{i} * part_size, std::min<std::size_t>(part_size, sizeof entry));
            WiaPartition partition;
            for (unsigned d = 0; d < 2; ++d) {
                const auto* p = entry + 0x10 + d * 0x10;
                const WiaPartitionData data{be32(p), be32(p + 4), be32(p + 8), be32(p + 12)};
                if ((std::uint64_t{data.first_sector} + data.sectors) * sector > s.disc_size || data.group > group_count ||
                    data.groups > group_count - data.group)
                    throw DiscError("partition table is damaged");
                partition.data[d] = data;
            }
            if (partition.data[1].sectors && partition.data[1].first_sector < partition.data[0].first_sector)
                throw DiscError("partition table is damaged");
            s.partitions.push_back(partition);
        }
    }

    const auto raw = s.table(be64(disc.data() + 0xb8), be32(disc.data() + 0xc0), std::size_t{raw_count} * 0x18);
    for (std::uint32_t i = 0; i < raw_count; ++i) {
        const auto* p = raw.data() + std::size_t{i} * 0x18;
        State::Raw entry{be64(p), be64(p + 8), be32(p + 16), be32(p + 20)};
        if (entry.offset > s.disc_size || entry.size > s.disc_size - entry.offset || entry.group > group_count ||
            entry.groups > group_count - entry.group)
            throw DiscError("damaged raw data table");
        // The first entry starts at 0x80; groups begin on sector boundaries.
        const auto aligned = entry.offset & ~(sector - 1);
        entry.size += entry.offset - aligned;
        entry.offset = aligned;
        s.raw.push_back(entry);
    }

    const std::size_t group_bytes = s.rvz ? 12 : 8;
    const auto groups = s.table(be64(disc.data() + 0xc8), be32(disc.data() + 0xd0), std::size_t{group_count} * group_bytes);
    s.groups.reserve(group_count);
    for (std::uint32_t i = 0; i < group_count; ++i) {
        const auto* p = groups.data() + std::size_t{i} * group_bytes;
        s.groups.push_back({be32(p), be32(p + 4), s.rvz ? be32(p + 8) : 0});
    }
}

WiaImage::~WiaImage() = default;

bool WiaImage::rvz() const noexcept { return state_->rvz; }
bool WiaImage::wii() const noexcept { return state_->wii; }
std::uint64_t WiaImage::size() const noexcept { return state_->disc_size; }
const std::vector<WiaPartition>& WiaImage::partitions() const noexcept { return state_->partitions; }
std::uint64_t WiaImage::partition_data_size(std::size_t partition) const { return state_->partition_size(partition); }

void WiaImage::read(std::uint64_t offset, void* output, std::size_t size) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->read_disc(offset, static_cast<std::uint8_t*>(output), size);
}

void WiaImage::read_partition(std::size_t partition, std::uint64_t offset, void* output, std::size_t size) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->read_partition(partition, offset, static_cast<std::uint8_t*>(output), size);
}

std::size_t WiaImage::data_partition() {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->data_partition) state_->data_partition = state_->find_data_partition();
    return *state_->data_partition;
}

std::unique_ptr<DiscImage> open_disc_image(const std::string& path) {
    std::uint8_t head[0x20] = {};
    {
        std::uint64_t size = 0;
        const int fd = open_file(path, size);
        try {
            read_at(fd, 0, head, static_cast<std::size_t>(std::min<std::uint64_t>(size, sizeof head)));
        } catch (...) {
            ::close(fd);
            throw;
        }
        ::close(fd);
    }
    if (!std::memcmp(head, "WIA\x01", 4) || !std::memcmp(head, "RVZ\x01", 4)) return std::make_unique<WiaImage>(path);
    if (be32(head + 0x1c) == gamecube_magic || be32(head + 0x18) == wii_magic) return std::make_unique<IsoImage>(path);
    if (ends_with_lower(path, ".wia") || ends_with_lower(path, ".rvz")) return std::make_unique<WiaImage>(path);
    if (ends_with_lower(path, ".iso") || ends_with_lower(path, ".gcm")) return std::make_unique<IsoImage>(path);
    throw DiscError(path.substr(path.find_last_of('/') + 1) + " is not a GameCube or Wii disc image");
}

GameVolume::GameVolume(DiscImage& image) : image_(image) {
    std::uint8_t header[0x20];
    image.read(0, header, sizeof header);
    if (be32(header + 0x1c) == gamecube_magic) return;
    if (be32(header + 0x18) != wii_magic) throw DiscError("not a GameCube or Wii disc image");
    auto* wia = dynamic_cast<WiaImage*>(&image);
    if (!wia || !wia->wii())
        throw DiscError("this Wii disc image is a plain copy, which keeps the game encrypted; convert it to RVZ with Dolphin");
    partition_ = wia->data_partition();
    wia_ = wia;
}

std::uint64_t GameVolume::size() const { return wia_ ? wia_->partition_data_size(partition_) : image_.size(); }

void GameVolume::read(std::uint64_t offset, void* output, std::size_t size) {
    if (wia_) wia_->read_partition(partition_, offset, output, size);
    else image_.read(offset, output, size);
}

std::array<std::uint8_t, 0x440> GameVolume::boot() {
    std::array<std::uint8_t, 0x440> bytes{};
    read(0, bytes.data(), bytes.size());
    return bytes;
}

std::vector<std::uint8_t> GameVolume::main_dol() {
    const auto offset = std::uint64_t{be32(boot().data() + 0x420)} << shift();
    std::uint8_t header[0x100];
    read(offset, header, sizeof header);
    // Seven text and eleven data sections: file offsets at 0x00, sizes at 0x90.
    std::uint64_t size = sizeof header;
    for (unsigned s = 0; s < 18; ++s)
        if (be32(header + 0x90 + s * 4))
            size = std::max<std::uint64_t>(size, std::uint64_t{be32(header + s * 4)} + be32(header + 0x90 + s * 4));
    if (size > 64u << 20) throw DiscError("damaged main.dol");
    std::vector<std::uint8_t> dol(static_cast<std::size_t>(size));
    read(offset, dol.data(), dol.size());
    return dol;
}

std::vector<std::uint8_t> GameVolume::fst() {
    const auto boot_bytes = boot();
    const auto offset = std::uint64_t{be32(boot_bytes.data() + 0x424)} << shift();
    const auto size = std::uint64_t{be32(boot_bytes.data() + 0x428)} << shift();
    if (!size || size > 8u << 20) throw DiscError("damaged game file table");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    read(offset, bytes.data(), bytes.size());
    return bytes;
}

} // namespace starlet::disc
