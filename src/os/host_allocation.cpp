#include "starlet/os/host_allocation.hpp"

#include "starlet/os/game_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace starlet::os {
namespace {

thread_local bool guest_allocation = false;
GameHeap game_heap;

bool in_arena(const void* pointer) {
    const auto& game = arena();
    const auto* bytes = static_cast<const std::uint8_t*>(pointer);
    return game.base && bytes >= game.base && bytes < game.base + game.size;
}

void* allocate(std::size_t size) {
    // As on the console, an exhausted game heap returns null to the game.
    if (guest_allocation && game_heap.ready && game_heap.ready()) return game_heap.allocate(size);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    std::fprintf(stderr, "starlet: out of host memory (%zu bytes)\n", size);
    std::abort();
}

void release(void* pointer) {
    if (!pointer) return;
    if (in_arena(pointer)) game_heap.release(pointer);
    else std::free(pointer);
}

}

void set_guest_allocation(bool guest) { guest_allocation = guest; }
void set_game_heap(GameHeap heap) { game_heap = heap; }

host_allocation_scope::host_allocation_scope() : previous_(guest_allocation) { guest_allocation = false; }
host_allocation_scope::~host_allocation_scope() { guest_allocation = previous_; }

}

using starlet::os::allocate;
using starlet::os::release;

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
