#include "fake_ai_platform.hpp"

#include "../../src/ai/platform.hpp"

#include <cstdio>
#include <cstdlib>

namespace fake_ai {

bool immediate = true;
unsigned output_rate = 0;
int opens = 0;
GuestMemory memory;

namespace {
std::vector<void (*)()> queued;
starlet::host::AudioPull output_pull = nullptr;
void* output_context = nullptr;
}

void run_interrupts() {
    while (!queued.empty()) {
        auto handler = queued.front();
        queued.erase(queued.begin());
        handler();
    }
}

std::size_t queued_interrupts() { return queued.size(); }

std::vector<std::int16_t> pull(std::size_t frames) {
    std::vector<std::int16_t> out(frames * 2);
    if (output_pull) output_pull(output_context, out.data(), frames);
    return out;
}

}

namespace starlet::ai::platform {

starlet::sdk::BOOL disable_interrupts() { return 1; }
void restore_interrupts(starlet::sdk::BOOL) {}
void raise_interrupt(void (*handler)()) {
    if (fake_ai::immediate)
        handler();
    else
        fake_ai::queued.push_back(handler);
}
void* hardware_to_native(starlet::sdk::u32 address, starlet::sdk::u32 bytes) {
    const std::uint64_t offset = std::uint64_t(address) - 0x80000000u;
    if (address < 0x80000000u || offset + bytes > fake_ai::memory.size()) return nullptr;
    return fake_ai::memory.data() + offset;
}
void before_dma_read(const void*, starlet::sdk::u32) {}
void panic(const char* message) {
    std::fprintf(stderr, "panic: %s\n", message);
    std::abort();
}
void with_host_allocation(void (*fn)(void*), void* context) { fn(context); }
void open_output(unsigned rate, starlet::host::AudioPull pull, void* context) {
    fake_ai::output_rate = rate;
    fake_ai::output_pull = pull;
    fake_ai::output_context = context;
    ++fake_ai::opens;
}
starlet::host::AudioStats output_stats() { return {}; }
std::string output_device_error() { return {}; }

}
