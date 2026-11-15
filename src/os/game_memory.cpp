#include "starlet/os/game_memory.hpp"

#include "starlet/os/host_allocation.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
extern "C" char __executable_start[];
extern "C" char _end[];
#endif

namespace starlet::os {
namespace {

MemoryMap map_{};
Arena game_arena;
std::mutex mutex;
std::uint8_t* stack_pool = nullptr;
std::size_t stack_pool_size = 0;
std::size_t stack_used = 0;
std::vector<std::pair<std::uint8_t*, std::size_t>> free_stacks;

struct Region {
    std::uintptr_t begin, end;
};
std::vector<Region> fixed_regions;

// The executable image's physical addresses [physical, physical + size), resolved to image.
struct ImageAlias {
    std::uint32_t physical = 0;
    std::size_t size = 0;
    std::uintptr_t image = 0;
};
ImageAlias image_alias_;

std::size_t page_size() { return static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }
std::size_t round_up(std::size_t value, std::size_t unit) { return (value + unit - 1) / unit * unit; }

// Linux honours a free hint without MAP_FIXED, and the H700 kernels (4.9) lack MAP_FIXED_NOREPLACE,
// so an address is requested as a hint and checked.
std::uint8_t* map_at(std::uintptr_t hint, std::size_t size) {
    void* result = mmap(reinterpret_cast<void*>(hint), size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (result == MAP_FAILED) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(result) == hint) return static_cast<std::uint8_t*>(result);
    munmap(result, size);
    return nullptr;
}

bool contains(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t address, std::size_t size) {
    return address >= begin && address + size <= end && address + size >= address;
}

bool overlaps(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t other_begin, std::uintptr_t other_end) {
    return begin < other_end && other_begin < end;
}

std::string hex(std::uintptr_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%lx", static_cast<unsigned long>(value));
    return text;
}

// Game memory may not lie where the image's physical addresses would point.
void check_outside_image_alias(std::uintptr_t begin, std::size_t size, const char* what) {
    std::uintptr_t alias_begin = 0, alias_end = 0;
    image_alias(alias_begin, alias_end);
    if (alias_begin == alias_end || !overlaps(begin, begin + size, alias_begin, alias_end)) return;
    throw std::runtime_error(std::string("the ") + what + " at " + hex(begin) + "-" + hex(begin + size) +
                             " overlaps " + hex(alias_begin) + "-" + hex(alias_end) +
                             ", where the executable image's physical addresses point; move it above " +
                             hex(alias_end));
}

}

void set_memory_map(MemoryMap map) {
    map_ = map;
    image_alias_ = {};
    std::uintptr_t begin = 0, end = 0;
    image_range(begin, end);
    // An image above 4 GiB (a position-independent test) has no 32-bit addresses to alias.
    if (begin == end || end > (std::uint64_t{1} << 32)) return;
    const std::uint32_t physical = native_to_physical(reinterpret_cast<const void*>(begin));
    if (map_.window_base + physical == begin) return;
    if (std::uint64_t{physical} + (end - begin) > 0x40000000u)
        throw std::runtime_error("the executable image's physical addresses pass 1 GiB");
    image_alias_ = {physical, end - begin, begin};
}

const MemoryMap& memory_map() { return map_; }

void image_alias(std::uintptr_t& begin, std::uintptr_t& end) {
    begin = end = 0;
    if (!image_alias_.size) return;
    begin = map_.window_base + image_alias_.physical;
    end = begin + image_alias_.size;
}

std::uint32_t native_to_physical(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) - map_.window_base) & 0x3FFFFFFF;
}

Arena set_arena(void* base, std::size_t size) {
    if (game_arena.base) throw std::logic_error("the game arena is already set");
    check_outside_image_alias(reinterpret_cast<std::uintptr_t>(base), size, "game arena");
    game_arena.base = static_cast<std::uint8_t*>(base);
    game_arena.size = size;
    return game_arena;
}

const Arena& arena() { return game_arena; }

void* map_fixed(std::uintptr_t address, std::size_t size, const char* what) {
    size = round_up(size, page_size());
    check_outside_image_alias(address, size, what);
    std::uint8_t* memory = map_at(address, size);
    if (!memory) throw std::runtime_error(std::string("cannot map the ") + what + " at its console address");
    std::lock_guard<std::mutex> lock(mutex);
    fixed_regions.push_back({address, address + size});
    return memory;
}

void set_stack_pool(std::size_t size, std::uintptr_t limit, const std::uintptr_t* hints, std::size_t hint_count) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stack_pool) throw std::logic_error("the stack pool is already mapped");
    size = round_up(size, page_size());
    std::uintptr_t alias_begin = 0, alias_end = 0;
    image_alias(alias_begin, alias_end);
    for (std::size_t i = 0; i < hint_count; ++i) {
        if (hints[i] + size > limit) continue;
        if (alias_begin != alias_end && overlaps(hints[i], hints[i] + size, alias_begin, alias_end)) continue;
        if (std::uint8_t* memory = map_at(hints[i], size)) {
            stack_pool = memory;
            stack_pool_size = size;
            return;
        }
    }
    throw std::runtime_error("no address space for the guest thread stacks");
}

void* allocate_stack(std::size_t size) {
    const std::size_t page = page_size();
    size = round_up(size, page);
    std::lock_guard<std::mutex> lock(mutex);
    if (!stack_pool) throw std::logic_error("no stack pool: call set_stack_pool first");
    for (auto it = free_stacks.begin(); it != free_stacks.end(); ++it)
        if (it->second == size) {
            std::uint8_t* stack = it->first;
            free_stacks.erase(it);
            return stack;
        }
    if (stack_used + page + size > stack_pool_size) throw std::runtime_error("guest thread stack pool exhausted");
    std::uint8_t* guard = stack_pool + stack_used;
    mprotect(guard, page, PROT_NONE);
    stack_used += page + size;
    return guard + page;
}

void release_stack(void* stack, std::size_t size) {
    if (!stack) return;
    size = round_up(size, page_size());
    host_allocation_scope host;
    std::lock_guard<std::mutex> lock(mutex);
    free_stacks.emplace_back(static_cast<std::uint8_t*>(stack), size);
}

void image_range(std::uintptr_t& begin, std::uintptr_t& end) {
#if defined(__linux__)
    begin = reinterpret_cast<std::uintptr_t>(__executable_start);
    end = reinterpret_cast<std::uintptr_t>(_end);
#else
    begin = end = 0;
#endif
}

bool game_visible(const void* pointer, std::size_t size) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto base = reinterpret_cast<std::uintptr_t>(game_arena.base);
    if (contains(base, base + game_arena.size, address, size)) return true;
    std::uintptr_t begin = 0, end = 0;
    image_range(begin, end);
    if (contains(begin, end, address, size)) return true;
    std::lock_guard<std::mutex> lock(mutex);
    const auto pool = reinterpret_cast<std::uintptr_t>(stack_pool);
    if (stack_pool && contains(pool, pool + stack_used, address, size)) return true;
    for (const Region& region : fixed_regions)
        if (contains(region.begin, region.end, address, size)) return true;
    return false;
}

void* physical_to_native(std::uint32_t address) {
    const std::uint32_t physical = address & 0x3FFFFFFF;
    if (physical - image_alias_.physical < image_alias_.size)
        return reinterpret_cast<void*>(image_alias_.image + (physical - image_alias_.physical));
    return reinterpret_cast<void*>(map_.window_base + physical);
}

void* hardware_to_native(std::uint32_t address, std::size_t size) {
    // The address as a pointer, or its physical reading (cached and uncached pointers included).
    auto* pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    if (game_visible(pointer, size)) return pointer;
    void* native = physical_to_native(address);
    return game_visible(native, size) ? native : nullptr;
}

}
