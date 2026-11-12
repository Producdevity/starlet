#pragma once

#include "starlet/sdk/os.h"

#include <cstddef>

namespace starlet::os {

struct OsConfig {
    char game_id[6] = {};  // the disc's game and maker code, as boot info gives it
    // The console's low memory (physical 0x0000-0x3100) when the port maps it where the game
    // addresses it directly; the boot block and the bus and core clocks are written there.
    void* low_memory = nullptr;
};

// Sets up what the console's IPL and OSInit leave behind for main(): arena bounds (from the arena the
// port gave game_memory), boot info and the calendar base.
void initialize_os(const OsConfig& config);

// The boot information block the console keeps at physical address 0.
starlet::sdk::OSBootInfo* boot_info_block();

// OSReport text is kept for crash reports; with print set it also goes to stdout.
void set_game_log(bool print);

// DCStoreRange/DCFlushRange report the CPU writes the GX may read.
using CpuWriteObserver = void (*)(const void* data, std::size_t bytes);
void set_cpu_write_observer(CpuWriteObserver);
// DMA engines (disc, ARAM) report the memory they wrote the same way.
void report_cpu_write(const void* data, std::size_t bytes);

// OSResetSystem hands the request to the application, which exits or restarts.
using ResetObserver = void (*)(int reset, unsigned code, bool force_menu);
void set_reset_observer(ResetObserver);

}
