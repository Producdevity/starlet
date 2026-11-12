// The AI model on starlet::os and starlet::host.
#include "platform.hpp"

#include "starlet/os/game_memory.hpp"
#include "starlet/os/guest_cpu.hpp"
#include "starlet/os/host_allocation.hpp"
#include "starlet/os/os_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace starlet::ai::platform {
namespace {

starlet::host::AudioOutput& output() {
    static starlet::host::AudioOutput* o = [] {
        starlet::os::host_allocation_scope host;
        return new starlet::host::AudioOutput;
    }();
    return *o;
}

}

starlet::sdk::BOOL disable_interrupts() { return OSDisableInterrupts(); }
void restore_interrupts(starlet::sdk::BOOL level) { OSRestoreInterrupts(level); }
void raise_interrupt(void (*handler)()) { starlet::os::raise_interrupt(handler); }
void* hardware_to_native(starlet::sdk::u32 address, starlet::sdk::u32 bytes) {
    return starlet::os::hardware_to_native(address, bytes);
}
void before_dma_read(const void* data, starlet::sdk::u32 bytes) { starlet::os::before_dma_read(data, bytes); }
void panic(const char* message) {
    OSPanic(__FILE__, __LINE__, "%s", message);
    std::abort();
}

void with_host_allocation(void (*fn)(void*), void* context) {
    starlet::os::host_allocation_scope host;
    fn(context);
}

void open_output(unsigned rate, starlet::host::AudioPull pull, void* context) {
    starlet::os::host_allocation_scope host;
    output().close();
    try {
        output().open_pull(rate, pull, context);
    } catch (const std::exception& error) {
        // AIInit is called from game code built without exceptions.
        std::fprintf(stderr, "starlet: ai: the audio output does not start: %s\n", error.what());
        std::fflush(nullptr);
        std::abort();
    }
}

starlet::host::AudioStats output_stats() { return output().stats(); }

std::string output_device_error() {
    starlet::os::host_allocation_scope host;
    return output().device_error();
}

}
