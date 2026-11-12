#pragma once

// The ARAM (AR, ARQ) SDK types and functions starlet::ar implements, with the console SDK's layouts.

#include "starlet/sdk/os.h"

namespace starlet::sdk {

using ARCallback = void (*)();
using ARQCallback = void (*)(u32 pointerToARQRequest);

struct ARQRequest {
    ARQRequest* next;
    u32 owner;
    u32 type;
    u32 priority;
    u32 source;
    u32 dest;
    u32 length;
    ARQCallback callback;
};

constexpr u32 ARAM_DIR_MRAM_TO_ARAM = 0;
constexpr u32 ARAM_DIR_ARAM_TO_MRAM = 1;
constexpr u32 ARQ_TYPE_MRAM_TO_ARAM = ARAM_DIR_MRAM_TO_ARAM;
constexpr u32 AR_ARAM_USR_BASE_ADDR = 0x4000;

} // namespace starlet::sdk
