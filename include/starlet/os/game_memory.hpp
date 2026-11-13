#pragma once

#include <cstddef>
#include <cstdint>

namespace starlet::os {

// Every address the game holds must fit in 32 bits: decompilations store pointers in u32 fields and
// cast them to u32, and GX, DMA and DSP addresses are 32-bit. A port links without PIE and keeps the
// game's memory (executable image, arena, guest stacks, fixed console regions) below 4 GiB.
//
// Hardware the game hands addresses to decodes physical addresses: the game converts with
// OSCachedToPhysical (pointer - 0x80000000) or OSCachedToUncached (pointer + 0x40000000). Physical
// address p is native address window_base + p. A port whose game memory sits at the console's own
// addresses uses window_base 0x80000000; one whose memory sits low uses 0.
//
// The GX's copy destination and TLUT load registers hold an address shifted right by 5 in 21 bits,
// so what the GX writes or loads that way must lie in the first 64 MiB of the window.
inline constexpr std::uint32_t gx_window_size = std::uint32_t{1} << 26;

struct MemoryMap {
    std::uintptr_t window_base = 0;
};
void set_memory_map(MemoryMap map);
const MemoryMap& memory_map();

// The physical address the hardware would see for a game pointer.
std::uint32_t native_to_physical(const void* pointer);

struct Arena {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
};
// The game arena (MEM1's free memory), in storage the port provides. Call once, before the game starts.
Arena set_arena(void* base, std::size_t size);
const Arena& arena();

// Maps size bytes of zeroed memory at exactly address (a console region the game addresses directly:
// low-memory globals, fixed buffers, the locked cache). Throws when the address is taken.
void* map_fixed(std::uintptr_t address, std::size_t size, const char* what);

// Guest thread stacks come from a pool mapped at the first free hint below limit, with a guard page
// under each stack. Call before the first allocate_stack.
void set_stack_pool(std::size_t size, std::uintptr_t limit, const std::uintptr_t* hints, std::size_t hint_count);
void* allocate_stack(std::size_t size);
// Returns a stack from allocate_stack once no thread runs on it; a later allocate_stack of the
// same rounded size reuses it.
void release_stack(void* stack, std::size_t size);

// The game memory (arena, executable image, guest stack or fixed region) that holds size bytes at
// the 32-bit address the game gave the hardware (a pointer, physical or uncached), or nullptr.
void* hardware_to_native(std::uint32_t address, std::size_t size);
// Physical address p as a native pointer: window_base + (p & 0x3FFFFFFF).
void* physical_to_native(std::uint32_t address);

// True when [pointer, pointer + size) lies in game memory.
bool game_visible(const void* pointer, std::size_t size);
// The executable image's address range (non-PIE).
void image_range(std::uintptr_t& begin, std::uintptr_t& end);

}
