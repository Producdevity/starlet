// starlet::dvd on one installed disc: path resolution with the SDK's rules, the current
// directory and directory listings.
#include "starlet/dvd/dvd.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/sdk/dvd.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace starlet::sdk;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void expect_entry(const char* path, s32 expected) {
    std::string copy = path;
    const s32 entry = DVDConvertPathToEntrynum(copy.data());
    if (entry != expected) {
        std::printf("FAIL: \"%s\" resolved to %d, expected %d\n", path, int(entry), int(expected));
        ++failures;
    }
}

struct Record {
    bool directory;
    const char* name;
    std::uint32_t parent_or_offset, next_or_length;
};

// 0 /            4 /dir/Sub/
// 1 /b.bin       5 /dir/Sub/deep.bin
// 2 /dir/        6 /c.szs
// 3 /dir/x.bin
std::vector<std::uint8_t> table() {
    const Record records[] = {
        {true, "", 0, 7}, {false, "b.bin", 0x1000, 4}, {true, "dir", 0, 6}, {false, "x.bin", 0x2000, 4},
        {true, "Sub", 2, 6}, {false, "deep.bin", 0x3000, 4}, {false, "c.szs", 0x4000, 4},
    };
    std::vector<std::uint8_t> fst(sizeof records / sizeof *records * 12);
    std::string names;
    auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) fst[at + i] = std::uint8_t(value >> (24 - 8 * i));
    };
    for (std::size_t i = 0; i < sizeof records / sizeof *records; ++i) {
        const std::uint32_t name_offset = i ? std::uint32_t(names.size()) : 0;
        if (i) names += std::string(records[i].name) + '\0';
        put(i * 12, (records[i].directory ? 0x01000000u : 0u) | name_offset);
        put(i * 12 + 4, records[i].parent_or_offset);
        put(i * 12 + 8, records[i].next_or_length);
    }
    fst.insert(fst.end(), names.begin(), names.end());
    return fst;
}

int remove_entry(const char* path, const struct stat*, int, struct FTW*) { return std::remove(path); }

}

// A caller that polls the drive status until it reports idle finds its block final and its
// callback run: the drive stays busy until the completion interrupt has delivered.
int async_callbacks = 0;
int async_reads() {
    // Other interrupts (the game's retraces) wake a waiting status poll before the read's own.
    static std::atomic<bool> stop{false};
    std::thread retraces([] {
        while (!stop) {
            starlet::os::raise_interrupt([] {});
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    int unfinished = 0;
    for (int i = 0; i < 300; ++i) {
        DVDFileInfo file;
        alignas(32) static u8 buffer[32];
        DVDFastOpen(1, &file);
        DVDReadAsyncPrio(&file, buffer, 32, 0, [](s32, DVDFileInfo*) { ++async_callbacks; }, 2);
        while (DVDGetDriveStatus() != DVD_STATE_END) {}
        if (file.cb.state != DVD_STATE_END) ++unfinished;
    }
    expect(unfinished == 0, "an idle drive has no block still busy");
    expect(async_callbacks == 300, "every callback ran before the drive reported idle");
    stop = true;
    retraces.join();
    return 0;
}

int main() {
    const char* tmp = std::getenv("TMPDIR");
    std::string root = std::string(tmp && *tmp ? tmp : "/tmp") + "/starlet-dvd-XXXXXX";
    if (!mkdtemp(root.data()) || mkdir((root + "/sys").c_str(), 0755) != 0) {
        std::perror("temporary disc folder");
        return 1;
    }
    {
        const auto fst = table();
        std::ofstream(root + "/sys/fst.bin", std::ios::binary).write(reinterpret_cast<const char*>(fst.data()), std::streamsize(fst.size()));
        // The disc header: game GTST, maker 01, disc 0.
        std::vector<char> boot(0x440, 0);
        std::memcpy(boot.data(), "GTST01", 6);
        std::ofstream(root + "/sys/boot.bin", std::ios::binary).write(boot.data(), std::streamsize(boot.size()));
        // The files, four bytes each as the table says.
        for (const char* dir : {"/files", "/files/dir", "/files/dir/Sub"}) mkdir((root + dir).c_str(), 0755);
        for (const char* file : {"/files/b.bin", "/files/dir/x.bin", "/files/dir/Sub/deep.bin", "/files/c.szs"})
            std::ofstream(root + file, std::ios::binary) << "data";
    }
    static std::vector<unsigned char> arena(std::size_t{4} << 20);
    starlet::os::set_arena(arena.data(), arena.size());
    starlet::os::initialize_os({});
    starlet::dvd::initialize({{root + "/sys", {root + "/files"}}});

    expect_entry("/b.bin", 1);
    expect_entry("b.bin", 1);
    expect_entry("/dir/x.bin", 3);
    expect_entry("/DIR/X.BIN", 3);
    expect_entry("/dir/sub/deep.bin", 5);
    expect_entry("/dir/sub/", 4);
    expect_entry("/dir", 2);
    expect_entry("/", 0);
    expect_entry("", 0);
    // Every '/' restarts at the root: the game builds "dir//full/path" names.
    expect_entry("/dir//b.bin", 1);
    expect_entry("/dir/Sub///dir/x.bin", 3);
    expect_entry("/dir/../c.szs", 6);
    expect_entry("/dir/./x.bin", 3);
    expect_entry("/dir/sub/..", 2);
    expect_entry("/dir/.", 2);
    // A component followed by '/' names a directory.
    expect_entry("/dir/x.bin/", -1);
    expect_entry("/b.bin/x", -1);
    expect_entry("/missing", -1);
    expect_entry("/dir/b.bin", -1);
    expect_entry("/b.bi", -1);

    char dir[] = "/dir";
    if (!DVDChangeDir(dir)) {
        std::printf("FAIL: DVDChangeDir(/dir)\n");
        ++failures;
    }
    expect_entry("x.bin", 3);
    expect_entry("Sub/deep.bin", 5);
    expect_entry("..", 0);
    expect_entry("../b.bin", 1);
    expect_entry("/b.bin", 1);
    char current[64];
    expect(DVDGetCurrentDir(current, sizeof current) && std::string(current) == "/dir", "DVDGetCurrentDir");

    // Listings: a directory's entries in table order, its subdirectories skipped over.
    DVDDir listing;
    DVDDirEntry item;
    std::string seen;
    char root_name[] = "/";
    expect(DVDOpenDir(root_name, &listing) == TRUE, "DVDOpenDir(/)");
    while (DVDReadDir(&listing, &item)) seen += std::to_string(item.entryNum) + (item.isDir ? "d" : "") + item.name + " ";
    expect(seen == "1b.bin 2ddir 6c.szs ", "the root lists its own entries");
    DVDCloseDir(&listing);
    seen.clear();
    expect(DVDFastOpenDir(2, &listing) == TRUE, "DVDFastOpenDir(2)");
    while (DVDReadDir(&listing, &item)) seen += std::to_string(item.entryNum) + (item.isDir ? "d" : "") + item.name + " ";
    expect(seen == "3x.bin 4dSub ", "a directory lists its files and subdirectories");
    expect(DVDFastOpenDir(1, &listing) == FALSE, "a file is not a directory");
    char missing[] = "/missing";
    expect(DVDOpenDir(missing, &listing) == FALSE, "no such directory");

    const std::uintptr_t hints[] = {0x10000000, 0x20000000, 0x30000000};
    starlet::os::set_stack_pool(std::size_t{16} << 20, 0x40000000, hints, 3);
    starlet::os::run_main_thread(async_reads);

    nftw(root.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    std::printf("%s\n", failures ? "dvd: failures" : "dvd: ok");
    std::fflush(stdout);
    // The arena is game memory: returning would hand its storage to the game heap's delete.
    std::_Exit(failures ? 1 : 0);
}
