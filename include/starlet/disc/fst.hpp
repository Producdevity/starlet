#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace starlet::disc {

struct FstEntry {
    std::string name;          // as stored on the disc
    std::uint32_t parent = 0;  // index of the containing directory
    std::uint32_t next = 0;    // first index after this entry and everything inside it
    std::uint32_t length = 0;  // file size in bytes
    std::uint64_t offset = 0;  // file position in the game volume, in bytes
    bool directory = false;
};

// The file system table (sys/fst.bin): big-endian 12-byte records, then the name table.
// Wii tables store file offsets divided by 4; shift is 2 for them and 0 for GameCube
// (GameVolume::shift()). Throws DiscError on a malformed table, including sibling names
// that differ only in case, which a FAT card cannot hold side by side.
class FileTable {
public:
    FileTable(const std::uint8_t* data, std::size_t size, unsigned shift);
    FileTable(const std::vector<std::uint8_t>& fst, unsigned shift) : FileTable(fst.data(), fst.size(), shift) {}

    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(entries_.size()); }
    const FstEntry& entry(std::uint32_t index) const;
    // "/dir/name" from the stored names; "/" for the root.
    std::string path(std::uint32_t index) const;
    // The same path from host_name() components: where extraction writes the entry under files/.
    std::string host_path(std::uint32_t index) const;
    // The DVD SDK's lookup: ASCII case-insensitive, with "." and "..", and a '/' at the start
    // of any component returns to the root. Relative paths start at cwd, a directory.
    std::optional<std::uint32_t> resolve(std::string_view path, std::uint32_t cwd = 0) const;

private:
    std::vector<FstEntry> entries_;
};

// The name a disc file gets on the host. FST names are bytes, and some discs carry
// Shift-JIS names. Every byte outside printable ASCII, each of "*:<>?\|% and a final '.' or
// ' ' becomes %XX (uppercase hex), so names are ASCII, valid on FAT, exFAT and APFS, and map
// back uniquely.
std::string host_name(std::string_view fst_name);

} // namespace starlet::disc
