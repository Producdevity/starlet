// Auxiliary RAM (16 MiB) and its DMA engine. Transfers copy at once and complete
// as interrupts, which is one timing the hardware's queued DMA can produce.
#include "starlet/ar/ar.hpp"

#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/os_core.hpp"
#include "starlet/sdk/ar.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace starlet::sdk;
using namespace starlet::os;

namespace starlet::ar {
namespace {

constexpr u32 aram_size = 16u << 20;
std::vector<std::uint8_t>& storage() {
    static std::vector<std::uint8_t>* m = [] {
        host_allocation_scope host;
        return new std::vector<std::uint8_t>(aram_size);
    }();
    return *m;
}

bool initialized = false;
u32* stack_base = nullptr;
u32* stack_top = nullptr;
u32 stack_entries = 0;
u32 next_free = AR_ARAM_USR_BASE_ADDR;
ARCallback dma_callback = nullptr;

void transfer(u32 type, u32 main, u32 aram, u32 length) {
    auto* host = static_cast<std::uint8_t*>(hardware_to_native(main, length));
    if (!host) {
        char message[128];
        std::snprintf(message, sizeof message, "ARAM DMA %s main memory 0x%08x (0x%x bytes, ARAM 0x%x) outside the game's memory",
                      type == ARAM_DIR_MRAM_TO_ARAM ? "from" : "to", main, length, aram);
        OSPanic(__FILE__, __LINE__, "%s", message);
    }
    if (u64(aram) + length > aram_size) {
        std::fprintf(stderr, "starlet: ARAM DMA past the end (0x%x + 0x%x)\n", aram, length);
        std::abort();
    }
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        before_dma_read(host, length);
        std::memcpy(storage().data() + aram, host, length);
    } else {
        before_dma_write(host, length);
        std::memcpy(host, storage().data() + aram, length);
        report_cpu_write(host, length);
    }
}

}

std::uint8_t* memory() { return storage().data(); }
std::size_t bytes() { return aram_size; }

}

using namespace starlet::ar;

extern "C" {

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    if (initialized) return AR_ARAM_USR_BASE_ADDR;
    initialized = true;
    stack_base = stack_top = stack_index_addr;
    stack_entries = num_entries;
    next_free = AR_ARAM_USR_BASE_ADDR;
    storage();
    return AR_ARAM_USR_BASE_ADDR;
}

BOOL ARCheckInit() { return initialized; }
void ARReset() { initialized = false; }
u32 ARGetBaseAddress() { return AR_ARAM_USR_BASE_ADDR; }
u32 ARGetSize() { return aram_size; }
u32 ARGetInternalSize() { return aram_size; }
void ARSetSize() {}

u32 ARAlloc(u32 length) {
    const BOOL enabled = OSDisableInterrupts();
    if (u32(stack_top - stack_base) >= stack_entries || u64(next_free) + length > aram_size) {
        OSRestoreInterrupts(enabled);
        OSPanic(__FILE__, __LINE__, "ARAlloc(): out of ARAM or allocation entries");
    }
    const u32 block = next_free;
    *stack_top++ = length;
    next_free += length;
    OSRestoreInterrupts(enabled);
    return block;
}

u32 ARFree(u32* length) {
    const BOOL enabled = OSDisableInterrupts();
    if (stack_top == stack_base) {
        OSRestoreInterrupts(enabled);
        return next_free;
    }
    const u32 size = *--stack_top;
    next_free -= size;
    if (length) *length = size;
    OSRestoreInterrupts(enabled);
    return next_free;
}

void ARClear(u32) {}

ARCallback ARRegisterDMACallback(ARCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    ARCallback previous = dma_callback;
    dma_callback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}

u32 ARGetDMAStatus() {
    checkpoint();
    return 0;
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length) {
    starlet::ar::transfer(type, mainmem_addr, aram_addr, length);
    raise_interrupt([] {
        if (dma_callback) dma_callback();
    });
}

void ARQInit() {}
// Transfers complete when posted, so no request is ever waiting in the queue.
void ARQFlushQueue() {}

void ARQPostRequest(ARQRequest* task, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback) {
    task->next = nullptr;
    task->owner = owner;
    task->type = type;
    task->priority = priority;
    task->source = source;
    task->dest = dest;
    task->length = length;
    task->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) starlet::ar::transfer(type, source, dest, length);
    else starlet::ar::transfer(type, dest, source, length);
    raise_interrupt([task] {
        if (task->callback) task->callback(static_cast<u32>(reinterpret_cast<std::uintptr_t>(task)));
    });
}

}
