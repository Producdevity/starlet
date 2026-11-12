// starlet::card_sdk: the SDK's CARD calls on a GCI folder (Dolphin's <saves>/USA/Card A)
// that keeps the card across mounts; slot B is empty. Results follow the SDK; file bytes
// follow Dolphin's GCI folder format.
#include "starlet/card/sdk.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/sdk/card.h"

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace starlet::sdk;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void expect_result(s32 got, s32 want, const char* what) {
    if (got != want) {
        std::printf("FAIL: %s: result %d, expected %d\n", what, int(got), int(want));
        ++failures;
    }
}

std::string root, saves, folder;
alignas(32) u8 work_area[0xA000];
alignas(32) u8 buffer[0xC000];
alignas(32) u8 other[0xC000];
char save_name[] = "Pikmin2_SaveData";
const std::string save_gci = "01-GPVE-Pikmin2_SaveData.gci";

std::vector<u8> read_bytes(const std::string& path) {
    std::vector<u8> bytes;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        u8 chunk[4096];
        for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
        std::fclose(f);
    }
    return bytes;
}

void write_bytes(const std::string& path, const std::vector<u8>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("FAIL: cannot write %s\n", path.c_str());
        ++failures;
        return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

std::vector<std::string> names_in(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d))
            if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) names.emplace_back(e->d_name);
        closedir(d);
    }
    return names;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int count_suffix(const std::string& dir, const char* suffix) {
    int n = 0;
    for (const auto& name : names_in(dir)) n += ends_with(name, suffix);
    return n;
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

// Seconds since 2000-01-01, local time.
u32 card_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return u32(now + local.tm_gmtoff - 946684800);
}

bool recent(u32 time) { return time + 120 >= card_now() && time <= card_now() + 120; }

void fill(u8* bytes, std::size_t n, u8 seed) {
    for (std::size_t i = 0; i < n; ++i) bytes[i] = u8(seed + i * 7 + (i >> 8));
}

void free_space(s32 chan, s32 blocks, s32 files, const char* what) {
    s32 bytes = -1, entries = -1;
    expect_result(CARDFreeBlocks(chan, &bytes, &entries), CARD_RESULT_READY, what);
    if (bytes != blocks * 0x2000 || entries != files) {
        std::printf("FAIL: %s: %d free blocks and %d free files, expected %d and %d\n", what, int(bytes / 0x2000),
                    int(entries), int(blocks), int(files));
        ++failures;
    }
}

void remount(const char* what) {
    starlet::card::initialize_cards(saves);
    expect_result(CARDMount(0, work_area, nullptr), CARD_RESULT_READY, what);
}

// Slot B never has a card; slot A has none without a usable folder.
void missing_card() {
    u64 serial = 0;
    s32 mem = 0, sector = 0;
    expect(!CARDProbe(1), "slot B probe");
    expect_result(CARDProbeEx(1, &mem, &sector), CARD_RESULT_NOCARD, "slot B ProbeEx");
    expect_result(CARDMount(1, work_area, nullptr), CARD_RESULT_NOCARD, "slot B mount");
    expect_result(CARDFreeBlocks(1, nullptr, nullptr), CARD_RESULT_NOCARD, "slot B FreeBlocks");
    expect_result(CARDGetSerialNo(1, &serial), CARD_RESULT_NOCARD, "slot B serial");
    expect_result(CARDGetSerialNo(2, &serial), CARD_RESULT_FATAL_ERROR, "channel 2");
    expect(!CARDProbe(2), "channel 2 probe");

    write_bytes(root + "/not-a-folder", {1, 2, 3});
    starlet::card::initialize_cards(root + "/not-a-folder");
    expect(!CARDProbe(0), "slot A probe without a folder");
    expect_result(CARDProbeEx(0, &mem, &sector), CARD_RESULT_NOCARD, "slot A ProbeEx without a folder");
    expect_result(CARDMount(0, work_area, nullptr), CARD_RESULT_NOCARD, "slot A mount without a folder");
}

u64 first_serial = 0;

void fresh_card() {
    starlet::card::initialize_cards(saves);
    s32 mem = 0, sector = 0;
    expect(CARDProbe(0), "slot A probe");
    expect_result(CARDProbeEx(0, &mem, &sector), CARD_RESULT_READY, "slot A ProbeEx");
    expect(mem == 16 && sector == 0x2000, "Memory Card 251 geometry");
    CARDFileInfo info;
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_NOCARD, "open before mount");
    expect_result(CARDMount(0, work_area, nullptr), CARD_RESULT_READY, "mount");
    expect_result(CARDCheck(0), CARD_RESULT_READY, "check");
    free_space(0, 251, 127, "empty card");
    expect_result(CARDGetSerialNo(0, &first_serial), CARD_RESULT_READY, "serial");
    expect(!read_bytes(folder + "/card.txt").empty(), "card identity file");
}

// The created file's GCI header, as the SDK's CARDCreate fills the entry.
const u8 created_header[0x40] = {
    'G', 'P', 'V', 'E', '0', '1', 0xFF, 0x00,
    'P', 'i', 'k', 'm', 'i', 'n', '2', '_', 'S', 'a', 'v', 'e', 'D', 'a', 't', 'a',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,              // Time, checked separately.
    0xFF, 0xFF, 0xFF, 0xFF,  // No icon.
    0x00, 0x00, 0x00, 0x01,  // Icon formats, speeds.
    0x04, 0x00,              // Public, never copied.
    0x00, 0x05, 0x00, 0x1B,  // First block, 27 blocks.
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// After the game's MemoryCard::Mgr::doSetCardStat.
const u8 status_header[0x40] = {
    'G', 'P', 'V', 'E', '0', '1', 0xFF, 0x05,
    'P', 'i', 'k', 'm', 'i', 'n', '2', '_', 'S', 'a', 'v', 'e', 'D', 'a', 't', 'a',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,
    0x00, 0x00, 0x00, 0x00,  // Icon at 0.
    0x00, 0x05, 0x00, 0x0F,  // Two C8 icons, slow.
    0x04, 0x00,
    0x00, 0x05, 0x00, 0x1B,
    0xFF, 0xFF, 0x00, 0x00, 0x18, 0x00,  // Comment at 0x1800.
};

bool header_is(const std::vector<u8>& gci, const u8* expected) {
    if (gci.size() < 0x40) return false;
    return std::memcmp(gci.data(), expected, 0x28) == 0 && std::memcmp(gci.data() + 0x2C, expected + 0x2C, 0x14) == 0 &&
           recent(be32(gci.data() + 0x28));
}

void create_write_read() {
    CARDFileInfo info;
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_NOFILE, "open a missing file");
    expect(info.chan == -1, "failed open leaves the file closed");
    expect_result(CARDCreate(0, save_name, 0x36000, &info), CARD_RESULT_READY, "create");
    expect(info.chan == 0 && info.iBlock == 5, "created file info");
    expect_result(CARDClose(&info), CARD_RESULT_READY, "close");
    free_space(0, 251 - 27, 126, "after create");
    const std::vector<u8> gci = read_bytes(folder + "/" + save_gci);
    expect(gci.size() == 0x40 + 0x36000, "GCI size");
    expect(header_is(gci, created_header), "created GCI header");

    expect_result(CARDCreate(0, save_name, 0x2000, &info), CARD_RESULT_EXIST, "create twice");
    char big[] = "Big";
    expect_result(CARDCreate(0, big, 225 * 0x2000, &info), CARD_RESULT_INSSPACE, "create past free space");
    expect_result(CARDCreate(0, big, 0x2100, &info), CARD_RESULT_FATAL_ERROR, "create a partial block");
    char long_name[] = "A_name_of_thirty-three_characters";
    expect_result(CARDCreate(0, long_name, 0x2000, &info), CARD_RESULT_NAMETOOLONG, "create with a long name");

    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_READY, "open");
    expect(info.fileNo == 0 && info.iBlock == 5 && info.offset == 0, "opened file info");
    fill(buffer, 0x4000, 0x11);
    expect_result(CARDWrite(&info, buffer, 0x4000, 0x2000), CARD_RESULT_READY, "write");
    expect_result(CARDWrite(&info, buffer, 0x1000, 0x2000), CARD_RESULT_FATAL_ERROR, "write a partial block");
    expect_result(CARDWrite(&info, buffer, 0x4000, 0x34000), CARD_RESULT_LIMIT, "write past the end");
    expect_result(CARDRead(&info, other, 0x200, 0x100), CARD_RESULT_FATAL_ERROR, "read off a 512-byte boundary");
    std::memset(other, 0, sizeof other);
    expect_result(CARDRead(&info, other, 0x200, 0x2200), CARD_RESULT_READY, "read");
    expect(std::memcmp(other, buffer + 0x200, 0x200) == 0, "read returns the written bytes");
    const std::vector<u8> written = read_bytes(folder + "/" + save_gci);
    expect(written.size() == gci.size() && std::memcmp(written.data() + 0x40 + 0x2000, buffer, 0x4000) == 0,
           "the GCI holds the written blocks");

    CARDStat stat;
    expect_result(CARDGetStatus(0, info.fileNo, &stat), CARD_RESULT_READY, "get status");
    expect(stat.length == 0x36000 && std::strcmp(stat.fileName, save_name) == 0, "status name and length");
    expect(std::memcmp(stat.gameName, "GPVE", 4) == 0 && std::memcmp(stat.company, "01", 2) == 0, "status owner");
    expect(stat.iconAddr == 0xFFFFFFFF && stat.commentAddr == 0xFFFFFFFF && stat.offsetData == 0, "status without icon");
    expect(recent(stat.time), "status time");

    CARDSetIconAddress(&stat, 0);
    CARDSetCommentAddress(&stat, 0x1800);
    CARDSetBannerFormat(&stat, CARD_STAT_BANNER_C8);
    CARDSetIconAnim(&stat, CARD_STAT_ANIM_BOUNCE);
    for (int i = 0; i < 8; ++i) {
        CARDSetIconFormat(&stat, i, i < 2 ? 1 : 0);
        CARDSetIconSpeed(&stat, i, i < 2 ? 3 : 0);
    }
    expect_result(CARDSetStatus(0, info.fileNo, &stat), CARD_RESULT_READY, "set status");
    expect(stat.offsetBanner == 0 && stat.offsetBannerTlut == 0xC00 && stat.offsetIcon[0] == 0xE00 &&
               stat.offsetIcon[1] == 0x1200 && stat.offsetIcon[2] == 0xFFFFFFFF && stat.offsetIconTlut == 0x1600 &&
               stat.offsetData == 0x1800,
           "image offsets match the game's header layout");
    CARDStat again;
    expect_result(CARDGetStatus(0, info.fileNo, &again), CARD_RESULT_READY, "get status again");
    expect(again.iconAddr == 0 && again.commentAddr == 0x1800 && again.bannerFormat == 5 && again.iconFormat == 5 &&
               again.iconSpeed == 0xF,
           "status after set");
    expect(header_is(read_bytes(folder + "/" + save_gci), status_header), "GCI header after set status");
    stat.commentAddr = 0x1FF0;
    expect_result(CARDSetStatus(0, info.fileNo, &stat), CARD_RESULT_FATAL_ERROR, "comment past its block");
    expect_result(CARDGetStatus(0, 127, &stat), CARD_RESULT_FATAL_ERROR, "status of file 127");
    expect_result(CARDGetStatus(0, 5, &stat), CARD_RESULT_NOFILE, "status of an empty entry");

    expect_result(CARDClose(&info), CARD_RESULT_READY, "close after transfers");
    expect_result(CARDRead(&info, other, 0x200, 0), CARD_RESULT_FATAL_ERROR, "read a closed file");
    expect(count_suffix(folder, ".tmp") == 0, "no temporary files remain");
}

OSThreadQueue async_waiters;
volatile bool async_done = false;
s32 async_result = 1;
bool async_in_interrupt = false;

void async_callback(s32 chan, s32 result) {
    async_result = chan == 0 ? result : 1;
    async_in_interrupt = starlet::os::in_interrupt();
    async_done = true;
    OSWakeupThread(&async_waiters);
}

// Asynchronous requests complete as interrupts; the slot is busy until then.
void async_read() {
    OSInitThreadQueue(&async_waiters);
    CARDFileInfo info;
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_READY, "open for async read");
    std::memset(other, 0, sizeof other);
    expect_result(CARDReadAsync(&info, other, 0x400, 0x2000, async_callback), CARD_RESULT_READY, "async read");
    u64 serial = 0;
    expect_result(CARDGetSerialNo(0, &serial), CARD_RESULT_BUSY, "busy during a transfer");
    const BOOL enabled = OSDisableInterrupts();
    while (!async_done) OSSleepThread(&async_waiters);
    OSRestoreInterrupts(enabled);
    expect(async_result == CARD_RESULT_READY && async_in_interrupt, "callback ran as an interrupt");
    expect(std::memcmp(other, buffer, 0x400) == 0, "async read data");
    CARDClose(&info);
}

void persistence() {
    remount("mount after re-init");
    u64 serial = 0;
    expect_result(CARDGetSerialNo(0, &serial), CARD_RESULT_READY, "serial after re-init");
    expect(serial == first_serial, "serial persists");
    free_space(0, 251 - 27, 126, "free space after re-init");
    CARDFileInfo info;
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_READY, "open after re-init");
    std::memset(other, 0, sizeof other);
    expect_result(CARDRead(&info, other, 0x4000, 0x2000), CARD_RESULT_READY, "read after re-init");
    expect(std::memcmp(other, buffer, 0x4000) == 0, "data persists");
    CARDStat stat;
    expect_result(CARDGetStatus(0, info.fileNo, &stat), CARD_RESULT_READY, "status after re-init");
    expect(stat.commentAddr == 0x1800 && stat.iconFormat == 5, "status persists");
    CARDClose(&info);

    expect_result(CARDUnmount(0), CARD_RESULT_READY, "unmount");
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_NOCARD, "open after unmount");
    expect_result(CARDMount(0, work_area, nullptr), CARD_RESULT_READY, "mount after unmount");
}

// A power cut leaves the old file and maybe a temporary one; a failed write
// leaves the old file.
void rename_safe_writes() {
    const std::string path = folder + "/" + save_gci;
    write_bytes(path + ".tmp", {0xDE, 0xAD});
    remount("mount with a stale temporary file");
    free_space(0, 251 - 27, 126, "a temporary file is not on the card");
    CARDFileInfo info;
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_READY, "open beside a temporary file");
    fill(buffer, 0x2000, 0x55);
    expect_result(CARDWrite(&info, buffer, 0x2000, 0), CARD_RESULT_READY, "write over a stale temporary file");
    expect(count_suffix(folder, ".tmp") == 0, "the temporary file became the GCI");

    if (geteuid() == 0) {
        std::printf("card: running as root, skipping the read-only folder case\n");
    } else {
        const std::vector<u8> before = read_bytes(path);
        chmod(folder.c_str(), 0500);
        fill(other, 0x2000, 0x99);
        expect_result(CARDWrite(&info, other, 0x2000, 0), CARD_RESULT_IOERROR, "write into a read-only folder");
        chmod(folder.c_str(), 0700);
        expect(read_bytes(path) == before, "a failed write leaves the file");
        std::memset(other, 0, 0x2000);
        expect_result(CARDRead(&info, other, 0x2000, 0), CARD_RESULT_READY, "read after a failed write");
        expect(std::memcmp(other, buffer, 0x2000) == 0, "a failed write leaves the card's data");
        expect_result(CARDWrite(&info, buffer, 0x2000, 0), CARD_RESULT_READY, "write after a failed write");
    }
    CARDClose(&info);
}

// Hand-made GCI files: a two-block file of this game copied from another card,
// two files of another game (one public), a truncated file and a duplicate.
const u8 handmade_header[0x40] = {
    'G', 'P', 'V', 'E', '0', '1', 0xFF, 0x05,
    'H', 'a', 'n', 'd', 'm', 'a', 'd', 'e',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x1A, 0x2B, 0x3C, 0x4D,  // Time.
    0x00, 0x00, 0x00, 0x00,  // Icon at 0.
    0x00, 0x05, 0x00, 0x0F,
    0x04, 0x02,              // Public, copied twice.
    0x01, 0x23,              // First block on the card it came from.
    0x00, 0x02,              // Two blocks.
    0xFF, 0xFF, 0x00, 0x00, 0x18, 0x00,
};

std::vector<u8> gci(const u8* header, u16 blocks, u8 seed) {
    std::vector<u8> bytes(header, header + 0x40);
    bytes[0x38] = u8(blocks >> 8);
    bytes[0x39] = u8(blocks);
    bytes.resize(0x40 + blocks * 0x2000u);
    fill(bytes.data() + 0x40, blocks * 0x2000u, seed);
    return bytes;
}

std::vector<u8> other_game(const char* name, u8 permission) {
    u8 header[0x40];
    std::memcpy(header, handmade_header, sizeof header);
    std::memcpy(header, "GXXE", 4);
    std::memset(header + 8, 0, 32);
    std::memcpy(header + 8, name, std::strlen(name));
    header[0x34] = permission;
    return gci(header, 1, 0x77);
}

void gci_round_trip() {
    const std::vector<u8> handmade = gci(handmade_header, 2, 0x31);
    write_bytes(folder + "/01-GPVE-Handmade.gci", handmade);
    write_bytes(folder + "/zz-copy-of-handmade.gci", handmade);
    write_bytes(folder + "/01-GXXE-Private.gci", other_game("Private", 0x00));
    write_bytes(folder + "/01-GXXE-Shared.gci", other_game("Shared", 0x04));
    std::vector<u8> truncated = handmade;
    truncated.resize(0x40 + 0x2100);
    write_bytes(folder + "/01-GPVE-Truncated.gci", truncated);
    remount("mount with hand-made files");
    free_space(0, 251 - 27 - 2 - 1 - 1, 123, "hand-made files on the card");

    char name[] = "Handmade";
    CARDFileInfo info;
    expect_result(CARDOpen(0, name, &info), CARD_RESULT_READY, "open the hand-made file");
    const u16 start = info.iBlock;
    expect(start >= 5 && start < 256, "the mount assigns a block");
    CARDStat stat;
    expect_result(CARDGetStatus(0, info.fileNo, &stat), CARD_RESULT_READY, "hand-made status");
    expect(stat.time == 0x1A2B3C4D && stat.length == 0x4000 && stat.bannerFormat == 0x05 && stat.iconAddr == 0 &&
               stat.iconFormat == 0x0005 && stat.iconSpeed == 0x000F && stat.commentAddr == 0x1800 &&
               std::strcmp(stat.fileName, "Handmade") == 0,
           "big-endian header fields decode");
    expect_result(CARDRead(&info, other, 0x4000, 0), CARD_RESULT_READY, "read the hand-made file");
    expect(std::memcmp(other, handmade.data() + 0x40, 0x4000) == 0, "hand-made data");

    fill(buffer, 0x2000, 0xA0);
    expect_result(CARDWrite(&info, buffer, 0x2000, 0x2000), CARD_RESULT_READY, "write the hand-made file");
    const std::vector<u8> after = read_bytes(folder + "/01-GPVE-Handmade.gci");
    expect(after.size() == handmade.size(), "hand-made size after write");
    if (after.size() == handmade.size()) {
        expect(std::memcmp(after.data(), handmade.data(), 0x28) == 0, "header bytes 0x00-0x27 round trip");
        expect(recent(be32(after.data() + 0x28)), "a write stamps the time");
        expect(std::memcmp(after.data() + 0x2C, handmade.data() + 0x2C, 0x0A) == 0, "header bytes 0x2C-0x35 round trip");
        expect(after[0x36] == u8(start >> 8) && after[0x37] == u8(start), "first block is this card's");
        expect(std::memcmp(after.data() + 0x38, handmade.data() + 0x38, 0x08) == 0, "header bytes 0x38-0x3F round trip");
        expect(std::memcmp(after.data() + 0x40, handmade.data() + 0x40, 0x2000) == 0, "unwritten block kept");
        expect(std::memcmp(after.data() + 0x2040, buffer, 0x2000) == 0, "written block");
    }
    CARDClose(&info);

    char private_name[] = "Private";
    expect_result(CARDOpen(0, private_name, &info), CARD_RESULT_NOFILE, "another game's file does not open");
    int private_files = 0, shared_files = 0;
    for (s32 fileNo = 0; fileNo < CARD_MAX_FILE; ++fileNo) {
        const s32 result = CARDGetStatus(0, fileNo, &stat);
        private_files += result == CARD_RESULT_NOPERM;
        shared_files += result == CARD_RESULT_READY && std::strcmp(stat.fileName, "Shared") == 0;
    }
    expect(private_files == 1 && shared_files == 1, "other games' files: private hidden, public readable");
}

// 127 files fill the directory; a 128th is not on the card.
void full_directory() {
    const std::string full = root + "/full";
    starlet::card::initialize_cards(full);
    write_bytes(full + "/USA/Card A/card.txt", {'j', 'u', 'n', 'k'});
    for (int i = 0; i < 128; ++i) {
        char file[16];
        std::snprintf(file, sizeof file, "f%03d", i);
        write_bytes(full + "/USA/Card A/01-GXXE-" + file + ".gci", other_game(file, 0x04));
    }
    expect_result(CARDMount(0, work_area, nullptr), CARD_RESULT_READY, "mount a full directory");
    free_space(0, 251 - 127, 0, "full directory");
    char name[] = "New";
    CARDFileInfo info;
    expect_result(CARDCreate(0, name, 0x2000, &info), CARD_RESULT_NOENT, "create in a full directory");
    u64 serial = 0;
    expect_result(CARDGetSerialNo(0, &serial), CARD_RESULT_READY, "serial of a damaged ID");
    expect(serial != first_serial, "a damaged ID gets a new serial");
}

// A card in another encoding mounts as ENCODING; formatting empties it.
void encoding_and_format() {
    const std::string japanese = "format_time 0123456789abcdef\nencoding 1\n";
    write_bytes(folder + "/card.txt", std::vector<u8>(japanese.begin(), japanese.end()));
    const int files = count_suffix(folder, ".gci");
    starlet::card::initialize_cards(saves);
    expect_result(CARDMount(0, work_area, nullptr), CARD_RESULT_ENCODING, "mount a Japanese card");
    expect_result(CARDCheck(0), CARD_RESULT_ENCODING, "check a Japanese card");
    CARDFileInfo info;
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_BROKEN, "open on a Japanese card");
    expect_result(CARDFreeBlocks(0, nullptr, nullptr), CARD_RESULT_BROKEN, "free blocks on a Japanese card");
    expect_result(CARDFormat(0), CARD_RESULT_READY, "format");
    free_space(0, 251, 127, "formatted card");
    u64 serial = 0;
    expect_result(CARDGetSerialNo(0, &serial), CARD_RESULT_READY, "serial after format");
    expect(serial != 0x0123456789abcdefULL && serial != first_serial, "format gives a new serial");
    expect(count_suffix(folder, ".gci") == 0, "format empties the card folder");
    int kept = 0;
    for (const auto& name : names_in(folder))
        if (name.rfind("formatted-", 0) == 0) kept += count_suffix(folder + "/" + name, ".gci");
    expect(kept == files, "format keeps the files out of the card");
    remount("mount after format");
    expect_result(CARDOpen(0, save_name, &info), CARD_RESULT_NOFILE, "no file after format");
}

int run_tests() {
    CARDInit();
    missing_card();
    fresh_card();
    create_write_read();
    async_read();
    persistence();
    rename_safe_writes();
    gci_round_trip();
    full_directory();
    starlet::card::initialize_cards(saves);
    encoding_and_format();
    return failures;
}

int remove_entry(const char* path, const struct stat*, int, struct FTW*) { return std::remove(path); }

}

int main() {
    const char* tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/p2-card-XXXXXX";
    if (!mkdtemp(pattern.data())) {
        std::perror("mkdtemp");
        return 1;
    }
    root = pattern;
    saves = root + "/SAVES";
    folder = saves + "/USA/Card A";
    static std::vector<unsigned char> arena(std::size_t{4} << 20);
    starlet::os::set_arena(arena.data(), arena.size());
    const std::uintptr_t hints[] = {0x10000000, 0x20000000, 0x30000000};
    starlet::os::set_stack_pool(std::size_t{64} << 20, 0x40000000, hints, 3);
    starlet::os::OsConfig config;
    std::memcpy(config.game_id, "GPVE01", 6);
    starlet::os::initialize_os(config);
    const int result = starlet::os::run_main_thread(run_tests);
    chmod(folder.c_str(), 0700);
    nftw(root.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    std::printf("%s\n", result ? "card: failures" : "card: ok");
    std::fflush(stdout);
    std::_Exit(result ? 1 : 0);
}
