// DVD file services over the installed disc trees. Entry numbers, offsets and lengths are each
// disc's own, so FST-based game code sees the disc's file system. Reads run in order on one drive
// thread and complete as interrupts, like the drive's.
#include "starlet/dvd/dvd.hpp"

#include "starlet/disc/fst.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/os/thread_name.hpp"
#include "starlet/sdk/dvd.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace starlet::sdk;

namespace starlet::dvd {
namespace {

struct Entry {
    std::string host_path;
    int fd = -1;
};

struct DiscState {
    std::vector<std::uint8_t> fst;
    std::unique_ptr<disc::FileTable> table;
    std::vector<Entry> entries;
    std::unordered_map<u32, u32> entry_at;  // Disc offset of a file -> its entry.
    DVDDiskID id{};
};

std::vector<DiscState> discs;
int current = 0;
u32 current_dir = 0;
std::uint8_t* fst_memory = nullptr;
OSThreadQueue sync_waiters;

DiscState& disc_now() { return discs[std::size_t(current)]; }

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool exists(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

void install_fst() {
    const DiscState& d = disc_now();
    os::before_dma_write(fst_memory, d.fst.size());
    std::memcpy(fst_memory, d.fst.data(), d.fst.size());
    os::report_cpu_write(fst_memory, d.fst.size());
}

std::optional<u32> entry_of(const DVDFileInfo* file) {
    const auto found = disc_now().entry_at.find(file->startAddr);
    if (found == disc_now().entry_at.end()) return std::nullopt;
    return found->second;
}

struct Request {
    DVDCommandBlock* block;
    DVDFileInfo* file;  // Null for a disc change.
    int disc;
    u32 entry;
    void* address;
    s32 length;
    s32 offset;
};

void complete(const Request& r, s32 result);

class Drive {
public:
    void submit(const Request& request) {
        os::host_allocation_scope host;
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(request);
        if (!started_) {
            started_ = true;
            std::thread([this] { run(); }).detach();
        }
        wake_.notify_one();
    }
    bool cancel(DVDCommandBlock* block) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = std::find_if(queue_.begin(), queue_.end(), [&](const Request& r) { return r.block == block; });
        if (found == queue_.end()) return false;
        queue_.erase(found);
        return true;
    }
    std::vector<Request> cancel_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Request> cancelled(queue_.begin(), queue_.end());
        queue_.clear();
        return cancelled;
    }
    bool busy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return !queue_.empty() || active_;
    }

private:
    void run() {
        os::name_thread("guest-dvd");
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return !queue_.empty(); });
            Request r = queue_.front();
            queue_.pop_front();
            active_ = true;
            lock.unlock();
            const s32 result = r.file ? read(r) : DVD_RESULT_GOOD;
            lock.lock();
            active_ = false;
            os::raise_interrupt([r, result] { complete(r, result); });
        }
    }
    static s32 read(const Request& r) {
        Entry& e = discs[std::size_t(r.disc)].entries[r.entry];
        if (e.fd < 0) {
            e.fd = open(e.host_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (e.fd < 0) {
                std::fprintf(stderr, "starlet: dvd: cannot open %s\n", e.host_path.c_str());
                return DVD_RESULT_FATAL_ERROR;
            }
        }
        // Reads past the end of a file return the bytes that exist; the drive would read whatever
        // follows on the disc, which no correct caller uses.
        auto* out = static_cast<std::uint8_t*>(r.address);
        if (r.length > 0) os::before_dma_write(out, std::size_t(r.length));
        s32 done = 0;
        while (done < r.length) {
            const ssize_t n = pread(e.fd, out + done, std::size_t(r.length - done), off_t(r.offset) + done);
            if (n <= 0) break;
            done += static_cast<s32>(n);
        }
        if (done < r.length) std::memset(out + done, 0, std::size_t(r.length - done));
        return r.length;
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> queue_;
    bool started_ = false;
    bool active_ = false;
};

Drive& drive() {
    static Drive* d = [] {
        os::host_allocation_scope host;
        return new Drive;
    }();
    return *d;
}

void complete(const Request& r, s32 result) {
    if (!r.file) {
        current = r.disc;
        current_dir = 0;
        install_fst();
    } else if (result > 0) {
        os::report_cpu_write(r.address, std::size_t(result));
    }
    r.block->transferredSize = result < 0 ? 0 : u32(result);
    r.block->state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    if (r.file && r.file->callback) r.file->callback(result, r.file);
    else if (!r.file && r.block->callback) r.block->callback(result, r.block);
    OSWakeupThread(&sync_waiters);
}

void wait_for(const DVDCommandBlock* block) {
    BOOL level = OSDisableInterrupts();
    while (block->state == DVD_STATE_BUSY) OSSleepThread(&sync_waiters);
    OSRestoreInterrupts(level);
}

}

void initialize(const std::vector<Disc>& list) {
    os::host_allocation_scope host;
    if (list.empty()) throw std::invalid_argument("starlet::dvd needs a disc");
    discs.clear();
    std::size_t largest = 0;
    for (const Disc& source : list) {
        DiscState d;
        d.fst = read_file(source.sys + "/fst.bin");
        const auto boot = read_file(source.sys + "/boot.bin");
        if (boot.size() < sizeof d.id) throw std::runtime_error("short " + source.sys + "/boot.bin");
        std::memcpy(&d.id, boot.data(), sizeof d.id);
        d.table = std::make_unique<disc::FileTable>(d.fst, 0);
        d.entries.resize(d.table->size());
        for (u32 i = 0; i < d.table->size(); ++i) {
            const auto& e = d.table->entry(i);
            if (e.directory) continue;
            const std::string relative = d.table->host_path(i);
            for (const std::string& root : source.roots) {
                if (exists(root + relative)) {
                    d.entries[i].host_path = root + relative;
                    break;
                }
            }
            if (d.entries[i].host_path.empty())
                throw std::runtime_error("the installed disc " + std::to_string(discs.size() + 1) + " lacks " + relative);
            if (e.length && !d.entry_at.count(u32(e.offset))) d.entry_at[u32(e.offset)] = i;
        }
        largest = std::max(largest, d.fst.size());
        discs.push_back(std::move(d));
    }
    // The apploader leaves the FST at the top of the arena and lowers the arena's top below it.
    const auto top = reinterpret_cast<std::uintptr_t>(OSGetArenaHi());
    const std::uintptr_t base = (top - largest) & ~std::uintptr_t{31};
    fst_memory = reinterpret_cast<std::uint8_t*>(base);
    OSSetArenaHi(fst_memory);
    current = 0;
    install_fst();
    OSInitThreadQueue(&sync_waiters);
}

int current_disc() { return current; }

}

using namespace starlet::dvd;

extern "C" {

void DVDInit() {}

BOOL DVDFastOpen(s32 entryNum, DVDFileInfo* fileInfo) {
    const auto& table = *disc_now().table;
    if (entryNum < 0 || u32(entryNum) >= table.size() || table.entry(u32(entryNum)).directory) return FALSE;
    const auto& e = table.entry(u32(entryNum));
    std::memset(fileInfo, 0, sizeof *fileInfo);
    fileInfo->startAddr = u32(e.offset);
    fileInfo->length = e.length;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

s32 DVDConvertPathToEntrynum(char* path) {
    const auto found = disc_now().table->resolve(path, current_dir);
    return found ? s32(*found) : -1;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo) {
    const s32 entry = DVDConvertPathToEntrynum(fileName);
    if (entry < 0) {
        std::fprintf(stderr, "starlet: dvd: no file %s on disc %d\n", fileName, current + 1);
        return FALSE;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    DVDCancel(&fileInfo->cb);
    return TRUE;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32) {
    const auto entry = entry_of(fileInfo);
    if (!entry || offset < 0 || u32(offset) > fileInfo->length) {
        std::fprintf(stderr, "starlet: dvd: read of an unknown file or outside it (offset %d)\n", offset);
        return FALSE;
    }
    fileInfo->callback = callback;
    fileInfo->cb.command = DVD_COMMAND_READ;
    fileInfo->cb.state = DVD_STATE_BUSY;
    fileInfo->cb.addr = addr;
    fileInfo->cb.length = u32(length);
    fileInfo->cb.offset = fileInfo->startAddr + u32(offset);
    fileInfo->cb.transferredSize = 0;
    drive().submit({&fileInfo->cb, fileInfo, current, *entry, addr, length, offset});
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    if (!DVDReadAsyncPrio(fileInfo, addr, length, offset, nullptr, prio)) return DVD_RESULT_FATAL_ERROR;
    wait_for(&fileInfo->cb);
    return fileInfo->cb.state == DVD_STATE_END ? s32(fileInfo->cb.transferredSize) : DVD_RESULT_FATAL_ERROR;
}

// Games poll these in loops until a transfer ends. The console's CPU took the drive's interrupt in the
// middle of such a loop; here a poll that finds the drive busy waits for the next interrupt.
s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    if (block->state == DVD_STATE_BUSY) starlet::os::wait_for_interrupt();
    else starlet::os::checkpoint();
    return block->state;
}
s32 DVDGetFileInfoStatus(const DVDFileInfo* fileInfo) { return DVDGetCommandBlockStatus(&fileInfo->cb); }
s32 DVDGetTransferredSize(DVDFileInfo* fileInfo) { return s32(fileInfo->cb.transferredSize); }
s32 DVDGetDriveStatus() {
    // Other interrupts (an ARAM transfer the same loop waits for) are delivered here too.
    if (drive().busy()) starlet::os::wait_for_interrupt();
    else starlet::os::checkpoint();
    return drive().busy() ? DVD_STATE_BUSY : DVD_STATE_END;
}

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    if (drive().cancel(block)) {
        block->state = DVD_STATE_CANCELED;
        if (callback) callback(DVD_RESULT_CANCELED, block);
        return TRUE;
    }
    // Not queued: idle, or on the drive, where the transfer completes (the SDK also waits for it).
    if (callback) callback(block->state == DVD_STATE_END ? DVD_RESULT_GOOD : DVD_RESULT_CANCELED, block);
    return TRUE;
}

s32 DVDCancel(DVDCommandBlock* block) {
    if (drive().cancel(block)) {
        block->state = DVD_STATE_CANCELED;
        return DVD_RESULT_CANCELED;
    }
    wait_for(block);
    return DVD_RESULT_GOOD;
}

s32 DVDCancelAll() {
    for (const Request& r : drive().cancel_all()) r.block->state = DVD_STATE_CANCELED;
    return DVD_RESULT_GOOD;
}

DVDDiskID* DVDGetCurrentDiskID() { return &disc_now().id; }

BOOL DVDCompareDiskID(const DVDDiskID* id1, const DVDDiskID* id2) {
    if (id1->gameName[0] && id2->gameName[0] && std::memcmp(id1->gameName, id2->gameName, 4) != 0) return FALSE;
    if (!id1->company[0] || !id2->company[0] || std::memcmp(id1->company, id2->company, 2) != 0) return FALSE;
    if (id1->diskNumber != 0xFF && id2->diskNumber != 0xFF && id1->diskNumber != id2->diskNumber) return FALSE;
    if (id1->gameVersion != 0xFF && id2->gameVersion != 0xFF && id1->gameVersion != id2->gameVersion) return FALSE;
    return TRUE;
}

DVDDiskID* DVDGenerateDiskID(DVDDiskID* id, const char* game, const char* company, u8 diskNumber, u8 version) {
    std::memset(id, 0, sizeof *id);
    if (game) std::memcpy(id->gameName, game, 4);
    if (company) std::memcpy(id->company, company, 2);
    id->diskNumber = diskNumber;
    id->gameVersion = version;
    return id;
}

// The requested disc is in the drive as soon as the change completes: the player never swaps discs.
BOOL DVDChangeDiskAsync(DVDCommandBlock* block, DVDDiskID* id, DVDCBCallback callback) {
    for (std::size_t i = 0; i < discs.size(); ++i) {
        if (!DVDCompareDiskID(&discs[i].id, id)) continue;
        block->command = DVD_COMMAND_CHANGE_DISK;
        block->state = DVD_STATE_BUSY;
        block->id = id;
        block->callback = callback;
        block->transferredSize = 0;
        drive().submit({block, nullptr, int(i), 0, nullptr, 0, 0});
        return TRUE;
    }
    std::fprintf(stderr, "starlet: dvd: no installed disc %u of %.4s\n", unsigned(id->diskNumber) + 1, id->gameName);
    return FALSE;
}

s32 DVDChangeDisk(DVDCommandBlock* block, DVDDiskID* id) {
    if (!DVDChangeDiskAsync(block, id, nullptr)) return DVD_RESULT_FATAL_ERROR;
    wait_for(block);
    return block->state == DVD_STATE_END ? DVD_RESULT_GOOD : DVD_RESULT_FATAL_ERROR;
}

void* DVDGetFSTLocation() { return fst_memory; }

// Directories of the disc in the drive: a directory's entries follow it up to its next index.
BOOL DVDFastOpenDir(s32 entryNum, DVDDir* dir) {
    const auto& table = *disc_now().table;
    if (entryNum < 0 || u32(entryNum) >= table.size() || !table.entry(u32(entryNum)).directory) return FALSE;
    dir->entryNum = u32(entryNum);
    dir->location = u32(entryNum) + 1;
    dir->next = table.entry(u32(entryNum)).next;
    return TRUE;
}

BOOL DVDOpenDir(char* dirName, DVDDir* dir) {
    const s32 entry = DVDConvertPathToEntrynum(dirName);
    return entry >= 0 && DVDFastOpenDir(entry, dir);
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirEntry) {
    const auto& table = *disc_now().table;
    const u32 i = dir->location;
    if (i <= dir->entryNum || i >= dir->next || i >= table.size()) return FALSE;
    const auto& e = table.entry(i);
    dirEntry->entryNum = i;
    dirEntry->isDir = e.directory;
    dirEntry->name = const_cast<char*>(e.name.c_str());
    dir->location = e.directory ? e.next : i + 1;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir*) { return TRUE; }

BOOL DVDChangeDir(char* dirName) {
    const s32 entry = DVDConvertPathToEntrynum(dirName);
    if (entry < 0 || !disc_now().table->entry(u32(entry)).directory) return FALSE;
    current_dir = u32(entry);
    return TRUE;
}

BOOL DVDGetCurrentDir(char* path, u32 maxLength) {
    const std::string out = disc_now().table->path(current_dir);
    if (out.size() + 1 > maxLength) return FALSE;
    std::memcpy(path, out.c_str(), out.size() + 1);
    return TRUE;
}

}
