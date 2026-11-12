#pragma once

#include <cstddef>

namespace starlet::os {

// The game replaces global operator new with its own heap allocator, as on the
// console. Port and library code (Starlet, SDL callbacks, the C++ library)
// must not take memory from game heaps, so allocations are routed by thread:
// guest threads allocate from the game's current heap once the game set one up,
// everything else (and code inside a host_allocation_scope) from malloc.
// Deletion is routed by address: arena memory goes back to the game's heap.
void set_guest_allocation(bool guest);

// The game's allocator, installed once the game is linked in (by the port).
// Before the game has a current heap (static construction), guest threads use malloc.
struct GameHeap {
    bool (*ready)() = nullptr;
    void* (*allocate)(std::size_t size) = nullptr;
    void (*release)(void* pointer) = nullptr;
};
void set_game_heap(GameHeap heap);

class host_allocation_scope {
public:
    host_allocation_scope();
    ~host_allocation_scope();
    host_allocation_scope(const host_allocation_scope&) = delete;
    host_allocation_scope& operator=(const host_allocation_scope&) = delete;

private:
    bool previous_;
};

}
