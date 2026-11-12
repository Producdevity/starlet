#pragma once

// Memory cards stored as Dolphin GCI folders (src/card/card.md).
//
// Folder: one slot's card as files on the host, one .gci per card file plus
// card.txt holding the card's identity. Both levels use it:
// - SDK level (source ports): Card implements the CARD calls on a Folder.
// - Hardware level (EXI card models): image() builds a formatted card image from
//   the folder, and store_image() writes a changed image's files back.

#include "starlet/card/format.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace starlet::card {

// Console regions, from the fourth character of a game ID (E, P, J ...).
enum class Region { usa, europe, japan };
Region region_of(char game_id_region);
// Dolphin's layout: <saves>/<USA|EUR|JAP>/Card A (slot 0) or Card B.
std::string folder_for(const std::string& saves_dir, Region, int slot);

// Messages for the port's log, one line each without a newline. Default: stderr.
using Log = std::function<void(const std::string&)>;

// What the emulated console holds for its card slots (SRAM and the VI DTV status),
// and its font encoding. A card's serial is derived from the slot's flash ID.
struct Console {
    std::array<std::array<std::uint8_t, 12>, 2> flash_id{};
    std::uint32_t counter_bias = 0;
    std::uint32_t language = 0;
    std::uint32_t vi_dtv = 0;
    Encoding encoding = Encoding::ansi;
};

class Folder {
public:
    struct Stored {
        File file;
        std::string host_name;  // Name in the folder.
    };
    // Creates the folder (and its parents) when missing. Without a usable folder,
    // ok() is false and the slot holds no card.
    Folder(std::string path, int slot, Console console, int size_mbits = 16, Log log = {});
    bool ok() const { return ok_; }
    const std::string& path() const { return path_; }
    int size_mbits() const { return size_mbits_; }
    int slot() const { return slot_; }
    const Console& console() const { return console_; }

    // The card's identity: card.txt holds its format time and encoding. A missing
    // or damaged card.txt is a freshly formatted card in the console's encoding.
    Result load_identity(Identity&);
    // Every GCI file in name order, with one log line for each file skipped: not a
    // GCI, a second copy of a file, or beyond what the card holds.
    Result load_files(std::vector<Stored>&);
    // Atomic replace: a temporary file, synced and renamed over the old one, then the
    // folder synced. New files get Dolphin's name, numbered when taken.
    Result save(Stored&);
    Result remove(const Stored&);  // Moves the file to removed-<time>/.
    void discard(const Stored&);   // Deletes the file (a renamed file's old copy).
    // An empty card with a new identity: every GCI moves to formatted-<time>/.
    Result format(Identity&);

    // Hardware level: a formatted image of the card's files and the identity it was
    // built with (one log line per file left out when they do not fit).
    std::vector<std::uint8_t> image();
    // Writes a changed image back: files that changed are saved, files the image no
    // longer has move to removed-<time>/, a format writes the new identity. An image
    // that does not verify (unformatted, broken) leaves the folder unchanged.
    Result store_image(const std::uint8_t* image, std::size_t size);
    // One-time import of a raw card image formatted anywhere (a model's old .raw file,
    // Dolphin's .gcp): its files join the folder, replacing same-named ones, and the
    // folder keeps its identity. ready, or why the image is unusable.
    Result import_image(const std::uint8_t* image, std::size_t size);

private:
    Result write_identity(const Identity&);
    Result move_aside(const std::vector<std::string>& names, const char* prefix);
    void report(const std::string&) const;
    std::string path_;
    int slot_, size_mbits_;
    Console console_;
    Log log_;
    bool ok_ = false;
};

// One CARD slot at the SDK level. Calls are synchronous and not thread-safe: the
// port serializes them and models the SDK's asynchronous completion itself.
// Results and argument checks are the SDK's.
class Card {
public:
    struct FileInfo {
        int file_no = -1;
        std::int32_t offset = 0;
        std::uint16_t block = 0;  // iBlock: the block holding offset.
    };
    struct Stat {
        char file_name[name_size];
        std::uint32_t length, time;
        char game[4], company[2];
        std::uint8_t banner_format;
        std::uint32_t icon_address;
        std::uint16_t icon_format, icon_speed;
        std::uint32_t comment_address;
        std::uint32_t offset_banner, offset_banner_tlut, offset_icon[8], offset_icon_tlut, offset_data;
    };
    // owner: the boot disc's game and company codes (DVDDiskID); only its files are
    // writable, other games' public files are readable.
    Card(std::shared_ptr<Folder>, const char game[4], const char company[2]);

    bool present() const { return folder_ && folder_->ok(); }
    int size_mbits() const { return folder_->size_mbits(); }
    bool mounted() const { return mounted_; }

    Result mount();  // Reads the folder: ready, encoding or io_error.
    void unmount();
    Result check();  // The mounted card's verdict: ready or encoding.
    Result format();
    Result free_blocks(std::int32_t& bytes, std::int32_t& files) const;
    Result serial(std::uint64_t&) const;

    Result open(const char* name, FileInfo&) const;
    Result fast_open(int file_no, FileInfo&) const;
    Result create(const char* name, std::uint32_t size, FileInfo&);
    // read: offset and length multiples of 512; write: of 8 KiB.
    Result read(FileInfo&, void* out, std::int32_t length, std::int32_t offset) const;
    Result write(FileInfo&, const void* in, std::int32_t length, std::int32_t offset);
    Result get_status(int file_no, Stat&) const;
    Result set_status(int file_no, const Stat&);
    Result get_attributes(int file_no, std::uint8_t&) const;
    Result set_attributes(int file_no, std::uint8_t);
    Result erase(const char* name);
    Result erase(int file_no);
    Result rename(const char* old_name, const char* new_name);

    // CARDGetStatus offsets from an entry (UpdateIconOffsets).
    static void icon_offsets(const Entry&, Stat&);

private:
    Result writable(int file_no) const;
    Result readable(int file_no) const;
    Result usable() const;
    Result seek(FileInfo&, std::int32_t length, std::int32_t offset) const;
    Result save_entry(int file_no, Entry, const std::vector<std::uint8_t>* data);
    std::shared_ptr<Folder> folder_;
    char game_[4], company_[2];
    bool mounted_ = false;
    Result verdict_ = no_card;
    Identity identity_;
    struct Held {
        Folder::Stored stored;
        std::vector<std::uint16_t> blocks;  // The card's blocks in file order.
    };
    bool allocate(std::uint16_t length, std::vector<std::uint16_t>& blocks) const;
    std::vector<std::unique_ptr<Held>> files_;  // By file number, max_files.
    std::vector<bool> used_;                    // By block number.
};

}
