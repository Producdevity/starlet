#pragma once

// GameCube memory card contents (src/card/card.md): the 64-byte directory entry,
// Dolphin GCI files, and whole card images with their ID, directory and block
// allocation table, as the Dolphin SDK's CARD library writes and verifies them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace starlet::card {

constexpr std::size_t block_size = 8192;
constexpr std::size_t entry_size = 64;
constexpr std::size_t name_size = 32;
constexpr int max_files = 127;
constexpr int system_blocks = 5;  // ID, two directories, two allocation tables.
constexpr std::uint32_t no_address = 0xFFFFFFFF;

// The SDK's CARD_RESULT_* values.
enum Result : std::int32_t {
    ready = 0,
    busy = -1,
    wrong_device = -2,
    no_card = -3,
    no_file = -4,
    io_error = -5,
    broken = -6,
    exist = -7,
    no_entry = -8,
    insufficient_space = -9,
    no_permission = -10,
    limit = -11,
    name_too_long = -12,
    encoding = -13,
    cancelled = -14,
    fatal_error = -128,
};

// Font encodings (CARD_ENCODE_*): the console's and the card's must agree.
enum class Encoding : std::uint16_t { ansi = 0, shift_jis = 1 };

// Card sizes in megabits; user blocks are 16 per megabit less the 5 system blocks.
// Memory Card 59 is 4, Memory Card 251 is 16, Memory Card 1019 is 64.
constexpr int blocks_for(int size_mbits) { return size_mbits * 16; }
bool valid_size(int size_mbits);

// A directory entry, kept as the card stores it (big-endian).
class Entry {
public:
    Entry() { bytes.fill(0xFF); }  // A free entry on a formatted card.
    std::array<std::uint8_t, entry_size> bytes;

    std::string game() const;     // 4 characters.
    std::string company() const;  // 2 characters.
    std::string name() const;     // Up to the first NUL, at most 32 characters.
    bool in_use() const { return bytes[0] != 0xFF; }
    bool same_file(const Entry& other) const;  // Game, company and name.
    bool name_is(const char* name) const;      // __CARDCompareFileName.

    std::uint8_t banner_format() const { return bytes[0x07]; }
    std::uint32_t time() const { return u32(0x28); }
    std::uint32_t icon_address() const { return u32(0x2C); }
    std::uint16_t icon_format() const { return u16(0x30); }
    std::uint16_t icon_speed() const { return u16(0x32); }
    std::uint8_t permission() const { return bytes[0x34]; }
    std::uint8_t copy_times() const { return bytes[0x35]; }
    std::uint16_t first_block() const { return u16(0x36); }
    std::uint16_t length() const { return u16(0x38); }  // In blocks.
    std::uint32_t comment_address() const { return u32(0x3C); }

    void set_owner(const char game[4], const char company[2]);
    void set_name(const char* name);  // Truncated to 32 bytes, NUL-padded.
    void set_banner_format(std::uint8_t v) { bytes[0x07] = v; }
    void set_time(std::uint32_t v) { put32(0x28, v); }
    void set_icon_address(std::uint32_t v) { put32(0x2C, v); }
    void set_icon_format(std::uint16_t v) { put16(0x30, v); }
    void set_icon_speed(std::uint16_t v) { put16(0x32, v); }
    void set_permission(std::uint8_t v) { bytes[0x34] = v; }
    void set_copy_times(std::uint8_t v) { bytes[0x35] = v; }
    void set_first_block(std::uint16_t v) { put16(0x36, v); }
    void set_length(std::uint16_t v) { put16(0x38, v); }
    void set_comment_address(std::uint32_t v) { put32(0x3C, v); }

private:
    std::uint16_t u16(std::size_t at) const { return std::uint16_t(bytes[at] << 8 | bytes[at + 1]); }
    std::uint32_t u32(std::size_t at) const {
        return std::uint32_t(bytes[at]) << 24 | std::uint32_t(bytes[at + 1]) << 16 | std::uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
    }
    void put16(std::size_t at, std::uint16_t v) { bytes[at] = std::uint8_t(v >> 8); bytes[at + 1] = std::uint8_t(v); }
    void put32(std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = std::uint8_t(v >> (24 - 8 * i));
    }
};

// One card file: its entry and its length() blocks of data.
struct File {
    Entry entry;
    std::vector<std::uint8_t> data;
};

// Dolphin GCI: the entry followed by the data. read_gci rejects sizes that do not
// match the entry's length.
std::vector<std::uint8_t> write_gci(const File&);
std::optional<File> read_gci(const std::uint8_t* bytes, std::size_t size);
// Dolphin's GCI folder name: <company>-<game>-<file name>.gci, bytes that file
// systems reject written as __xx__. Example: 01-GPVE-Pikmin2_SaveData.gci.
std::string gci_name(const Entry&);

// __CARDCheckSum over big-endian 16-bit words.
void checksum(const std::uint8_t* bytes, std::size_t length, std::uint16_t& sum, std::uint16_t& inverse);

// What the SDK puts in a card's ID when it formats it, besides the size.
struct Identity {
    std::array<std::uint8_t, 12> flash_id{};  // The console SRAM's ID for the slot.
    std::uint64_t format_time = 0;           // OSTime ticks (40.5 MHz since 2000).
    std::uint32_t counter_bias = 0;          // SRAM counter bias.
    std::uint32_t language = 0;              // SRAM language.
    std::uint32_t vi_dtv = 0;                // VI DTV status register.
    Encoding encoding = Encoding::ansi;
};
// The 32-byte serial CARDFormat derives from the identity.
std::array<std::uint8_t, 32> serial_bytes(const Identity&);
// CARDGetSerialNo: the four big-endian 64-bit words of the serial, exclusive-ored.
std::uint64_t serial_number(const std::array<std::uint8_t, 32>&);
// The flash ID a card's serial was derived from (VerifyID inverted).
std::array<std::uint8_t, 12> flash_id_of(const std::uint8_t* serial);
// The SRAM checksum byte for one slot's flash ID: the complement of its byte sum.
std::uint8_t flash_id_check(const std::array<std::uint8_t, 12>&);
// OSTime of a host time (seconds since the Unix epoch, local time offset applied).
std::uint64_t os_time(std::int64_t unix_seconds, std::int64_t utc_offset_seconds);
// The card clock: seconds since 2000-01-01 in local time.
std::uint32_t card_time_now();

// A card image: the ID, both directories and both allocation tables consistent and
// checksummed, files stored in contiguous blocks in order. Returns nothing when the
// files do not fit (more than 127 or more blocks than the card has).
std::optional<std::vector<std::uint8_t>> build_image(int size_mbits, const Identity&, const std::vector<File>&);

// Reading an image back, as CARDCheck verifies it: the ID against the flash ID and
// the console's encoding, the directory and allocation table copies by checksum and
// free count (one damaged copy is repaired from the other), then every file's block
// chain. result is ready, broken, encoding or no_card (unformatted: all bytes 0xFF).
struct Parsed {
    Result result = broken;
    std::array<std::uint8_t, 32> serial{};
    std::uint16_t size_mbits = 0;
    Encoding encoding = Encoding::ansi;
    std::vector<File> files;  // Entries keep the image's first block.
};
Parsed parse_image(const std::uint8_t* image, std::size_t size, const std::array<std::uint8_t, 12>& flash_id, Encoding console);

}
