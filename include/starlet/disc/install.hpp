#pragma once

#include "starlet/disc/image.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace starlet::disc {

// Lowercase SHA-1 by path relative to gamedata ("sys/main.dol", "files/...").
using Manifest = std::map<std::string, std::string>;
// One "<40 lowercase hex digits>  <path>" line per file, as sha1sum writes them.
Manifest parse_manifest(const std::string& text);

// Image formats a port accepts; iso also covers .gcm.
namespace formats {
constexpr unsigned rvz = 1, wia = 2, iso = 4, all = rvz | wia | iso;
}

// The one accepted disc image in directory. title names the game in the errors, which name
// the folder and what is in it, since the user reads them on the device.
std::string find_disc_image(const std::string& directory, unsigned accepted, const std::string& title);

struct SupportedDisc {
    std::string id;             // game ID at 0x00, such as "GPVE01"
    std::uint8_t disc = 0;      // disc number at 0x06; 0 is the first disc
    std::uint8_t revision = 0;  // revision at 0x07
    std::string dol_sha1;       // lowercase SHA-1 of main.dol
    // With a manifest, the extracted gamedata files must match it and it must list nothing else.
    const Manifest* manifest = nullptr;
};

struct Game {
    std::string title;  // as in "this is not <title>: found GPVP01 revision 0"
    std::vector<SupportedDisc> discs;
    // Part of the prepared marker; raising it makes installed copies run setup again.
    unsigned setup = 1;
};

// The disc in image, checked by ID, disc number, revision and main.dol SHA-1. Reads only.
SupportedDisc identify_disc(DiscImage&, const Game&);

// System files extract writes under gamedata/sys.
namespace sys_files {
constexpr unsigned boot = 1, bi2 = 2, apploader = 4, fst = 8, dol = 16, all = boot | bi2 | apploader | fst | dol;
}

using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
// Runs after the gamedata files are written and checked; work holds gamedata/.
using Hook = std::function<void(DiscImage&, const SupportedDisc&, const std::string& work)>;

struct SetupOptions {
    unsigned system_files = sys_files::all;
    Progress progress;
    Hook hook;
    // Directories the hook writes in work, which install publishes beside gamedata.
    std::vector<std::string> outputs;
};

struct Extracted {
    SupportedDisc disc;
    std::uint64_t files = 0, bytes = 0;  // files/ entries and their bytes
};

// Identifies the disc and checks its file table and size before writing anything, then
// writes work/gamedata/sys/{boot.bin,bi2.bin,apploader.img,fst.bin,main.dol} as selected and
// work/gamedata/files/<FST tree, host_path() names>. Wii discs are read from their DATA
// partition. work must not exist; a failed extraction removes it.
Extracted extract(DiscImage&, const std::string& work, const Game&, const SetupOptions& = {});

// Extracts into port/.setup-work, writes gamedata/.prepared, syncs, then renames each output
// and finally gamedata into port, replacing earlier copies. A failed run removes its partial
// output and keeps the previous data.
Extracted install(DiscImage&, const std::string& port, const Game&, const SetupOptions& = {});

// "<ID> revision <n>, setup <n>\n", the text of gamedata/.prepared.
std::string prepared_marker(const Game&, const SupportedDisc&);
// Whether port/gamedata/.prepared holds the marker of one of the game's discs.
bool is_prepared(const std::string& port, const Game&);

} // namespace starlet::disc
