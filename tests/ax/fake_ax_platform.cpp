#include "../../src/ax/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace fake_ax {
std::vector<std::uint8_t> aram(1 << 20);
starlet::sdk::OSTime now = 0;
}

namespace starlet::ax::platform {

starlet::sdk::BOOL disable_interrupts() { return 1; }
void restore_interrupts(starlet::sdk::BOOL) {}
starlet::sdk::OSTime time() { return ++fake_ax::now; }
void panic(const char* message) {
    std::fprintf(stderr, "panic: %s\n", message);
    std::abort();
}
const std::uint8_t* aram() { return fake_ax::aram.data(); }
std::size_t aram_bytes() { return fake_ax::aram.size(); }

}
