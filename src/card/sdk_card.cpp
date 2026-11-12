// The SDK's CARD calls (starlet/sdk/card.h) on starlet::card and starlet::os
// (src/card/card.md). Slot A is a Dolphin GCI folder; slot B is empty.
//
// starlet::card::Card does the card work synchronously. This file adds the SDK's
// control-block rules (busy, attached) and its timing: requests that move data run in
// order on one service thread and complete as interrupts, as the EXI's do, and the
// synchronous calls wait for them as __CARDSync does. Guest memory is read when a
// request is made and written when it completes, on guest threads.
#include "starlet/card/sdk.hpp"

#include "starlet/card/card.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/os/thread_name.hpp"
#include "starlet/sdk/card.h"

#include <sys/stat.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace starlet::card {
namespace {

namespace sc = starlet::card;
using namespace starlet::sdk;
using starlet::os::host_allocation_scope;

struct Slot {
    std::unique_ptr<sc::Card> card;  // Null: no card in the slot.
    bool attached = false;
    s32 result = CARD_RESULT_NOCARD;
    CARDCallback detach_callback = nullptr;
    CARDCallback api_callback = nullptr;
    OSThreadQueue waiters{};
};

struct Cards {
    bool initialized = false;  // CARDInit has run.
    std::string saves_dir;     // Empty: no card in slot A.
    int size_mbits = 16;
    Slot slots[2];
    std::mutex engine;  // Serializes the Card objects between guest and service threads.
};

// Never destroyed: the service thread may still run at exit.
Cards& cards() {
    static Cards* state = [] {
        host_allocation_scope host;
        return new Cards;
    }();
    return *state;
}

// The EXI channel: requests run one at a time, in order.
class Controller {
public:
    void post(std::function<void()> work) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(work));
        ++pending_;
        if (!started_) {
            started_ = true;
            std::thread([this] { run(); }).detach();
        }
        wake_.notify_one();
    }
    bool finish(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return idle_.wait_for(lock, timeout, [&] { return pending_ == 0; });
    }

private:
    void run() {
        starlet::os::name_thread("starlet-card");
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return !queue_.empty(); });
            std::function<void()> work = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            work();
            work = nullptr;
            lock.lock();
            if (--pending_ == 0) idle_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<std::function<void()>> queue_;
    std::size_t pending_ = 0;
    bool started_ = false;
};

Controller& controller() {
    static Controller* channel = [] {
        host_allocation_scope host;
        return new Controller;
    }();
    return *channel;
}

// Runs f on the slot's card under the engine lock.
template <class F> auto with_card(Slot& slot, F f) {
    host_allocation_scope host;
    std::lock_guard<std::mutex> lock(cards().engine);
    return f(*slot.card);
}

// __CARDGetControlBlock: the slot is attached and not busy; it is busy now.
s32 acquire(s32 chan, Slot*& slot) {
    if (chan < 0 || chan >= 2 || !cards().initialized) return CARD_RESULT_FATAL_ERROR;
    Slot& s = cards().slots[chan];
    const BOOL enabled = OSDisableInterrupts();
    s32 result = CARD_RESULT_READY;
    if (!s.attached) {
        result = CARD_RESULT_NOCARD;
    } else if (s.result == CARD_RESULT_BUSY) {
        result = CARD_RESULT_BUSY;
    } else {
        s.result = CARD_RESULT_BUSY;
        s.api_callback = nullptr;
        slot = &s;
    }
    OSRestoreInterrupts(enabled);
    return result;
}

// __CARDPutControlBlock.
s32 release(Slot& slot, s32 result) {
    const BOOL enabled = OSDisableInterrupts();
    if (slot.attached || slot.result == CARD_RESULT_BUSY) slot.result = result;
    OSRestoreInterrupts(enabled);
    return result;
}

// __CARDSync.
s32 wait(s32 chan) {
    Slot& slot = cards().slots[chan];
    const BOOL enabled = OSDisableInterrupts();
    while (slot.result == CARD_RESULT_BUSY) OSSleepThread(&slot.waiters);
    OSRestoreInterrupts(enabled);
    return slot.result;
}

// Runs work (the card, on the service thread), then finish on a guest thread as the
// EXI interrupt: the result, the callback, the sync waiters.
void transfer(s32 chan, std::function<s32()> work, std::function<void(s32)> finish = nullptr) {
    host_allocation_scope host;
    controller().post([chan, work = std::move(work), finish = std::move(finish)] {
        s32 result;
        {
            host_allocation_scope scope;
            std::lock_guard<std::mutex> lock(cards().engine);
            result = work();
        }
        starlet::os::raise_interrupt([chan, result, finish] {
            Slot& slot = cards().slots[chan];
            if (finish) finish(result);
            const CARDCallback callback = slot.api_callback;
            slot.api_callback = nullptr;
            release(slot, result);
            if (callback) callback(chan, result);
            OSWakeupThread(&slot.waiters);
        });
    });
}

void to_file_info(const sc::Card::FileInfo& in, s32 chan, CARDFileInfo* out) {
    out->chan = in.file_no < 0 ? -1 : chan;
    out->fileNo = in.file_no;
    out->offset = in.offset;
    out->iBlock = in.block;
}

sc::Card::FileInfo from_file_info(const CARDFileInfo* in) { return {in->fileNo, in->offset, in->iBlock}; }

void to_stat(const sc::Card::Stat& in, CARDStat* out) {
    std::memcpy(out->fileName, in.file_name, CARD_FILENAME_MAX);
    out->length = in.length;
    out->time = in.time;
    std::memcpy(out->gameName, in.game, 4);
    std::memcpy(out->company, in.company, 2);
    out->bannerFormat = in.banner_format;
    out->iconAddr = in.icon_address;
    out->iconFormat = in.icon_format;
    out->iconSpeed = in.icon_speed;
    out->commentAddr = in.comment_address;
    out->offsetBanner = in.offset_banner;
    out->offsetBannerTlut = in.offset_banner_tlut;
    for (int i = 0; i < CARD_ICON_MAX; ++i) out->offsetIcon[i] = in.offset_icon[i];
    out->offsetIconTlut = in.offset_icon_tlut;
    out->offsetData = in.offset_data;
}

sc::Card::Stat from_stat(const CARDStat* in) {
    sc::Card::Stat s{};
    std::memcpy(s.file_name, in->fileName, CARD_FILENAME_MAX);
    s.banner_format = in->bannerFormat;
    s.icon_address = in->iconAddr;
    s.icon_format = in->iconFormat;
    s.icon_speed = in->iconSpeed;
    s.comment_address = in->commentAddr;
    return s;
}

void report(const char* request, s32 result) {
    if (result == CARD_RESULT_FATAL_ERROR) std::fprintf(stderr, "starlet: card: %s: invalid arguments\n", request);
}

// Slot A: Dolphin's GCI folder for the boot disc's region. Card state belongs to guest
// threads; this runs before the game starts or, in tests, between requests.
void insert_card_a() {
    Cards& state = cards();
    host_allocation_scope host;
    Slot& slot = state.slots[0];
    slot.card.reset();
    slot.attached = false;
    slot.result = CARD_RESULT_NOCARD;
    if (state.saves_dir.empty()) return;
    const OSBootInfo& disk = *starlet::os::boot_info_block();
    sc::Console console;
    const sc::Region region = sc::region_of(char(disk.gameName[3]));
    console.encoding = region == sc::Region::japan ? sc::Encoding::shift_jis : sc::Encoding::ansi;
    const std::string folder = sc::folder_for(state.saves_dir, region, 0);
    auto storage = std::make_shared<sc::Folder>(folder, 0, console, state.size_mbits, [](const std::string& line) {
        std::fprintf(stderr, "starlet: card: %s\n", line.c_str());
    });
    if (storage->ok())
        slot.card = std::make_unique<sc::Card>(storage, disk.gameName, disk.company);
}

}

void initialize_cards(const std::string& saves_dir, int size_mbits) {
    host_allocation_scope host;
    cards().saves_dir = saves_dir;
    cards().size_mbits = size_mbits;
    if (cards().initialized) insert_card_a();
}

bool finish_card_writes(std::chrono::milliseconds timeout) { return controller().finish(timeout); }

}

using namespace starlet::card;

extern "C" {

void CARDInit() {
    Cards& state = cards();
    if (state.initialized) return;
    for (Slot& slot : state.slots) {
        slot.result = CARD_RESULT_NOCARD;
        OSInitThreadQueue(&slot.waiters);
    }
    insert_card_a();
    state.initialized = true;
}

BOOL CARDProbe(s32 channel) { return channel >= 0 && channel < 2 && cards().slots[channel].card; }

s32 CARDProbeEx(s32 channel, s32* memSize, s32* sectorSize) {
    if (channel < 0 || channel >= 2) return CARD_RESULT_FATAL_ERROR;
    Slot& slot = cards().slots[channel];
    if (!slot.card) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = slot.card->size_mbits();
    if (sectorSize) *sectorSize = s32(sc::block_size);
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 channel, void*, CARDCallback detachCallback, CARDCallback attachCallback) {
    if (channel < 0 || channel >= 2 || !cards().initialized) return CARD_RESULT_FATAL_ERROR;
    Slot& slot = cards().slots[channel];
    const BOOL enabled = OSDisableInterrupts();
    if (slot.result == CARD_RESULT_BUSY) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_BUSY;
    }
    if (!slot.card) {
        slot.result = CARD_RESULT_NOCARD;
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_NOCARD;
    }
    slot.result = CARD_RESULT_BUSY;
    slot.detach_callback = detachCallback;
    slot.api_callback = attachCallback;
    slot.attached = true;
    OSRestoreInterrupts(enabled);
    // A card that cannot be read leaves the slot, as a failed mount does.
    transfer(channel, [&slot] { return s32(slot.card->mount()); },
             [&slot](s32 mounted) { if (mounted == CARD_RESULT_IOERROR || mounted == CARD_RESULT_NOCARD) slot.attached = false; });
    return CARD_RESULT_READY;
}

s32 CARDMount(s32 channel, void* workArea, CARDCallback detachCallback) {
    const s32 result = CARDMountAsync(channel, workArea, detachCallback, nullptr);
    return result < 0 ? result : wait(channel);
}

s32 CARDUnmount(s32 channel) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    with_card(*slot, [](sc::Card& card) { card.unmount(); return 0; });
    slot->attached = false;
    slot->result = CARD_RESULT_NOCARD;
    return CARD_RESULT_READY;
}

s32 CARDCheckExAsync(s32 channel, s32* xferBytes, CARDCallback callback) {
    if (xferBytes) *xferBytes = 0;
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    transfer(channel, [slot] { return s32(slot->card->check()); });
    return CARD_RESULT_READY;
}

s32 CARDCheck(s32 channel) {
    s32 xferBytes = 0;
    const s32 result = CARDCheckExAsync(channel, &xferBytes, nullptr);
    return result < 0 ? result : wait(channel);
}

s32 CARDFormat(s32 channel) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    transfer(channel, [slot] { return s32(slot->card->format()); });
    return wait(channel);
}

s32 CARDFreeBlocks(s32 channel, s32* byteNotUsed, s32* filesNotUsed) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    std::int32_t bytes = 0, files = 0;
    const s32 r = with_card(*slot, [&](sc::Card& card) { return s32(card.free_blocks(bytes, files)); });
    if (r == CARD_RESULT_READY) {
        if (byteNotUsed) *byteNotUsed = bytes;
        if (filesNotUsed) *filesNotUsed = files;
    }
    return release(*slot, r);
}

s32 CARDGetSerialNo(s32 channel, u64* serialNo) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    std::uint64_t serial = 0;
    const s32 r = with_card(*slot, [&](sc::Card& card) { return s32(card.serial(serial)); });
    if (r == CARD_RESULT_READY) *serialNo = serial;
    return release(*slot, r);
}

s32 CARDOpen(s32 channel, const char* fileName, CARDFileInfo* fileInfo) {
    fileInfo->chan = -1;
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    sc::Card::FileInfo info;
    const s32 r = with_card(*slot, [&](sc::Card& card) { return s32(card.open(fileName, info)); });
    if (r == CARD_RESULT_READY) to_file_info(info, channel, fileInfo);
    return release(*slot, r);
}

s32 CARDClose(CARDFileInfo* fileInfo) {
    Slot* slot = nullptr;
    const s32 result = acquire(fileInfo->chan, slot);
    if (result < 0) return result;
    fileInfo->chan = -1;
    return release(*slot, CARD_RESULT_READY);
}

s32 CARDCreateAsync(s32 channel, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback) {
    if (std::strlen(fileName) > std::size_t(CARD_FILENAME_MAX)) return CARD_RESULT_NAMETOOLONG;
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    host_allocation_scope host;
    auto info = std::make_shared<sc::Card::FileInfo>();
    const std::string name = fileName;
    transfer(
        channel, [slot, name, size, info] { return s32(slot->card->create(name.c_str(), size, *info)); },
        [channel, info, fileInfo](s32 created) {
            report("CARDCreate", created);
            if (created == CARD_RESULT_READY) to_file_info(*info, channel, fileInfo);
        });
    return CARD_RESULT_READY;
}

s32 CARDCreate(s32 channel, const char* fileName, u32 size, CARDFileInfo* fileInfo) {
    const s32 result = CARDCreateAsync(channel, fileName, size, fileInfo, nullptr);
    return result < 0 ? result : wait(channel);
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* addr, s32 length, s32 offset, CARDCallback callback) {
    Slot* slot = nullptr;
    const s32 chan = fileInfo->chan;
    const s32 result = acquire(chan, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    host_allocation_scope host;
    auto info = std::make_shared<sc::Card::FileInfo>(from_file_info(fileInfo));
    auto data = std::make_shared<std::vector<u8>>(std::size_t(length > 0 ? length : 0));
    transfer(
        chan, [slot, info, data, length, offset] { return s32(slot->card->read(*info, data->data(), length, offset)); },
        [chan, info, data, fileInfo, addr](s32 read) {
            report("CARDRead", read);
            if (read != CARD_RESULT_READY) return;
            if (!data->empty()) {
                std::memcpy(addr, data->data(), data->size());
                starlet::os::report_cpu_write(addr, data->size());
            }
            to_file_info(*info, chan, fileInfo);
            fileInfo->length = s32(data->size());
        });
    return CARD_RESULT_READY;
}

s32 CARDRead(CARDFileInfo* fileInfo, void* addr, s32 length, s32 offset) {
    const s32 chan = fileInfo->chan;
    const s32 result = CARDReadAsync(fileInfo, addr, length, offset, nullptr);
    return result < 0 ? result : wait(chan);
}

s32 CARDWriteAsync(CARDFileInfo* fileInfo, const void* addr, s32 length, s32 offset, CARDCallback callback) {
    Slot* slot = nullptr;
    const s32 chan = fileInfo->chan;
    const s32 result = acquire(chan, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    host_allocation_scope host;
    auto info = std::make_shared<sc::Card::FileInfo>(from_file_info(fileInfo));
    const u8* bytes = static_cast<const u8*>(addr);
    auto data = std::make_shared<std::vector<u8>>(bytes, bytes + (length > 0 ? length : 0));
    transfer(
        chan, [slot, info, data, length, offset] { return s32(slot->card->write(*info, data->data(), length, offset)); },
        [chan, info, fileInfo, length](s32 written) {
            report("CARDWrite", written);
            if (written != CARD_RESULT_READY) return;
            to_file_info(*info, chan, fileInfo);
            fileInfo->length = length;
        });
    return CARD_RESULT_READY;
}

s32 CARDWrite(CARDFileInfo* fileInfo, const void* addr, s32 length, s32 offset) {
    const s32 chan = fileInfo->chan;
    const s32 result = CARDWriteAsync(fileInfo, addr, length, offset, nullptr);
    return result < 0 ? result : wait(chan);
}

s32 CARDGetStatus(s32 channel, s32 fileNo, CARDStat* state) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    sc::Card::Stat stat{};
    const s32 r = with_card(*slot, [&](sc::Card& card) { return s32(card.get_status(fileNo, stat)); });
    report("CARDGetStatus", r);
    if (r == CARD_RESULT_READY) to_stat(stat, state);
    return release(*slot, r);
}

s32 CARDSetStatusAsync(s32 channel, s32 fileNo, CARDStat* state, CARDCallback callback) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    host_allocation_scope host;
    auto stat = std::make_shared<sc::Card::Stat>(from_stat(state));
    transfer(
        channel, [slot, fileNo, stat] { return s32(slot->card->set_status(fileNo, *stat)); },
        [stat, state](s32 set) {
            report("CARDSetStatus", set);
            if (set != CARD_RESULT_READY) return;
            // The SDK fills in the offsets of the status it was given.
            sc::Entry entry;
            entry.set_banner_format(stat->banner_format);
            entry.set_icon_address(stat->icon_address);
            entry.set_icon_format(stat->icon_format);
            sc::Card::Stat offsets = *stat;
            sc::Card::icon_offsets(entry, offsets);
            state->offsetBanner = offsets.offset_banner;
            state->offsetBannerTlut = offsets.offset_banner_tlut;
            for (int i = 0; i < CARD_ICON_MAX; ++i) state->offsetIcon[i] = offsets.offset_icon[i];
            state->offsetIconTlut = offsets.offset_icon_tlut;
            state->offsetData = offsets.offset_data;
        });
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 channel, s32 fileNo, CARDStat* state) {
    const s32 result = CARDSetStatusAsync(channel, fileNo, state, nullptr);
    return result < 0 ? result : wait(channel);
}


s32 CARDCheckAsync(s32 channel, CARDCallback callback) {
    s32 xferBytes = 0;
    return CARDCheckExAsync(channel, &xferBytes, callback);
}

s32 CARDGetSectorSize(s32 channel, u32* size) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    *size = u32(sc::block_size);
    return release(*slot, CARD_RESULT_READY);
}

s32 CARDFastOpen(s32 channel, s32 fileNo, CARDFileInfo* fileInfo) {
    fileInfo->chan = -1;
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    sc::Card::FileInfo info;
    const s32 r = with_card(*slot, [&](sc::Card& card) { return s32(card.fast_open(fileNo, info)); });
    report("CARDFastOpen", r);
    if (r == CARD_RESULT_READY) to_file_info(info, channel, fileInfo);
    return release(*slot, r);
}

s32 CARDGetAttributes(s32 channel, s32 fileNo, u8* attr) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    std::uint8_t value = 0;
    const s32 r = with_card(*slot, [&](sc::Card& card) { return s32(card.get_attributes(fileNo, value)); });
    report("CARDGetAttributes", r);
    if (r == CARD_RESULT_READY) *attr = value;
    return release(*slot, r);
}

s32 CARDSetAttributesAsync(s32 channel, s32 fileNo, u8 attr, CARDCallback callback) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    transfer(channel, [slot, fileNo, attr] { return s32(slot->card->set_attributes(fileNo, attr)); },
             [](s32 set) { report("CARDSetAttributes", set); });
    return CARD_RESULT_READY;
}

s32 CARDSetAttributes(s32 channel, s32 fileNo, u8 attr) {
    const s32 result = CARDSetAttributesAsync(channel, fileNo, attr, nullptr);
    return result < 0 ? result : wait(channel);
}

s32 CARDDeleteAsync(s32 channel, const char* fileName, CARDCallback callback) {
    if (std::strlen(fileName) > std::size_t(CARD_FILENAME_MAX)) return CARD_RESULT_NAMETOOLONG;
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    host_allocation_scope host;
    const std::string name = fileName;
    transfer(channel, [slot, name] { return s32(slot->card->erase(name.c_str())); });
    return CARD_RESULT_READY;
}

s32 CARDDelete(s32 channel, const char* fileName) {
    const s32 result = CARDDeleteAsync(channel, fileName, nullptr);
    return result < 0 ? result : wait(channel);
}

s32 CARDFastDeleteAsync(s32 channel, s32 fileNo, CARDCallback callback) {
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    transfer(channel, [slot, fileNo] { return s32(slot->card->erase(int(fileNo))); },
             [](s32 deleted) { report("CARDFastDelete", deleted); });
    return CARD_RESULT_READY;
}

s32 CARDFastDelete(s32 channel, s32 fileNo) {
    const s32 result = CARDFastDeleteAsync(channel, fileNo, nullptr);
    return result < 0 ? result : wait(channel);
}

s32 CARDRenameAsync(s32 channel, const char* oldName, const char* newName, CARDCallback callback) {
    if (std::strlen(oldName) > std::size_t(CARD_FILENAME_MAX) || std::strlen(newName) > std::size_t(CARD_FILENAME_MAX))
        return CARD_RESULT_NAMETOOLONG;
    Slot* slot = nullptr;
    const s32 result = acquire(channel, slot);
    if (result < 0) return result;
    slot->api_callback = callback;
    host_allocation_scope host;
    const std::string from = oldName, to = newName;
    transfer(channel, [slot, from, to] { return s32(slot->card->rename(from.c_str(), to.c_str())); });
    return CARD_RESULT_READY;
}

s32 CARDRename(s32 channel, const char* oldName, const char* newName) {
    const s32 result = CARDRenameAsync(channel, oldName, newName, nullptr);
    return result < 0 ? result : wait(channel);
}

}
