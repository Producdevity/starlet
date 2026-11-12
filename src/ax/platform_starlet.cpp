// The AX model on starlet::os and starlet::ar.
#include "platform.hpp"

#include "starlet/ar/ar.hpp"

#include <cstdlib>

namespace starlet::ax::platform {

starlet::sdk::BOOL disable_interrupts() { return OSDisableInterrupts(); }
void restore_interrupts(starlet::sdk::BOOL level) { OSRestoreInterrupts(level); }
starlet::sdk::OSTime time() { return OSGetTime(); }
void panic(const char* message) {
    OSPanic(__FILE__, __LINE__, "%s", message);
    std::abort();
}
const std::uint8_t* aram() { return starlet::ar::memory(); }
std::size_t aram_bytes() { return starlet::ar::bytes(); }

}
