// The CPU side of AX as the SDK implements it (AX, AXAlloc, AXAux, AXCL, AXOut, AXProf, AXSPB,
// AXVPB): voice stacks and priorities, the parameter-block setters and their sync flags, the
// per-frame sync with its DSP cycle budget, depop, aux buffers and callbacks, and the AI interrupt
// that runs a frame. The DSP's frame (dsp.cpp) runs inside that interrupt, right after the sync.
#include "starlet/ax/ax.hpp"

#include "ax_internal.hpp"
#include "platform.hpp"
#include "starlet/ai/ai.hpp"
#include "starlet/sdk/ai.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

using namespace starlet::sdk;
using namespace starlet::ax::detail;

namespace {

namespace platform = starlet::ax::platform;

class Interrupts {
public:
    Interrupts() : level_(platform::disable_interrupts()) {}
    ~Interrupts() { platform::restore_interrupts(level_); }
    Interrupts(const Interrupts&) = delete;
    Interrupts& operator=(const Interrupts&) = delete;

private:
    BOOL level_;
};

// The SDK's DSP cycle estimates (AXVPB.c, AXCL.c).
constexpr u32 src_cycles[5] = {0xDF8, 0xF78, 0x14B8, 0x19F8, 0x19F8};
constexpr u32 main_mix_cycles[16] = {0x000, 0x2F8, 0x2F8, 0x5BE, 0x2F8, 0x5F0, 0x5F0, 0x8B6,
                                     0x000, 0x4F1, 0x4F1, 0x9A6, 0x4F1, 0x9E2, 0x9E2, 0xE97};
constexpr u32 aux_mix_cycles[32] = {0x000, 0x2F8, 0x2F8, 0x5BE, 0x000, 0x4F1, 0x4F1, 0x9A6, 0x2F8, 0x5F0, 0x5F0,
                                    0x8B6, 0x2F8, 0x7E9, 0x7E9, 0xC9E, 0x000, 0x2F8, 0x2F8, 0x5BE, 0x000, 0x4F1,
                                    0x4F1, 0x9A6, 0x4F1, 0x7E9, 0x7E9, 0xAAF, 0x4F1, 0x9E2, 0x9E2, 0xE97};
constexpr u32 default_sync = AX_SYNC_FLAG_COPYLPF | AX_SYNC_FLAG_COPYUPDATE | AX_SYNC_FLAG_COPYITD | AX_SYNC_FLAG_COPYSTATE;

// AXAlloc
AXVPB* stack_head[AX_PRIORITY_STACKS];
AXVPB* stack_tail[AX_PRIORITY_STACKS];
AXVPB* callback_stack;

// AXVPB
AXVPB voices[AX_MAX_VOICES];
AXPBITDBUFFER itd_buffers[AX_MAX_VOICES];
u32 max_dsp_cycles;
u32 recorded_dsp_cycles;
u32 voice_count;

// AXAux
AuxBuffers aux;
AXAuxCallback aux_callback_a, aux_callback_b;
void* aux_context_a;
void* aux_context_b;
u32 aux_write, aux_read, aux_cpu, dpl2_write, dpl2_read;

// AXCL
u32 cl_mode;
u32 compressor;
u32 cl_cycles;

// AXSPB: depop sums of dropped voices, faded out over the next frames.
s32 depop_sum[bus_count];

// AXOut: the DSP mixes into these; the AI DMA plays them, right channel first.
alignas(32) s16 output_buffers[3][2 * frame_samples];
u32 output_buffer_mode;
u32 output_frame;
u32 ai_dma_frame;
bool running;
AXCallback user_callback;

// AXProf
AXPROFILE* profiles;
u32 profile_count;
u32 current_profile;
bool profiles_initialized;

// The DSP and the command lists between the two sides.
Dsp dsp;
FrameCommand next_command;   // built at the end of a frame interrupt (__AXNextFrame)
FrameCommand frame_command;  // the list the DSP runs for the current frame

struct Counters {
    std::atomic<std::uint64_t> frames{0}, dropped{0};
    std::atomic<unsigned> voices{0}, cycles{0}, max_cycles{0};
};
Counters counters;

AXVPB* next_of(AXVPB* p) { return static_cast<AXVPB*>(p->next); }
AXVPB* prev_of(AXVPB* p) { return static_cast<AXVPB*>(p->prev); }

// --- AXAlloc ---

void init_voice_stacks() {
    callback_stack = nullptr;
    for (u32 i = 0; i < AX_PRIORITY_STACKS; ++i) stack_head[i] = stack_tail[i] = nullptr;
}

void push_free_stack(AXVPB* p) {
    p->next = stack_head[0];
    stack_head[0] = p;
    p->priority = 0;
}

AXVPB* pop_free_stack() {
    AXVPB* p = stack_head[0];
    if (p) stack_head[0] = next_of(p);
    return p;
}

void push_callback_stack(AXVPB* p) {
    p->next1 = callback_stack;
    callback_stack = p;
}

AXVPB* pop_callback_stack() {
    AXVPB* p = callback_stack;
    if (p) callback_stack = static_cast<AXVPB*>(p->next1);
    return p;
}

// As the SDK: a stack whose head is its tail is emptied whichever voice is removed.
void remove_from_stack(AXVPB* p) {
    const u32 i = p->priority;
    AXVPB* head = stack_head[i];
    AXVPB* tail = stack_tail[i];
    if (head == tail) {
        stack_head[i] = stack_tail[i] = nullptr;
        return;
    }
    if (p == head) {
        stack_head[i] = next_of(p);
        stack_head[i]->prev = nullptr;
        return;
    }
    if (p == tail) {
        stack_tail[i] = prev_of(p);
        stack_tail[i]->next = nullptr;
        return;
    }
    prev_of(p)->next = p->next;
    next_of(p)->prev = p->prev;
}

void push_stack_head(AXVPB* p, u32 priority) {
    p->next = stack_head[priority];
    p->prev = nullptr;
    if (p->next) {
        stack_head[priority]->prev = p;
        stack_head[priority] = p;
    } else {
        stack_tail[priority] = p;
        stack_head[priority] = p;
    }
    p->priority = priority;
}

AXVPB* pop_stack_from_bottom(u32 priority) {
    AXVPB* p = nullptr;
    if (stack_head[priority]) {
        if (stack_head[priority] == stack_tail[priority]) {
            p = stack_head[priority];
            stack_head[priority] = stack_tail[priority] = nullptr;
        } else if (stack_tail[priority]) {
            p = stack_tail[priority];
            stack_tail[priority] = prev_of(p);
            stack_tail[priority]->next = nullptr;
        }
    }
    return p;
}

void service_callback_stack() {
    for (AXVPB* p = pop_callback_stack(); p; p = pop_callback_stack()) {
        if (p->priority != 0) {
            if (p->callback) p->callback(p);
            remove_from_stack(p);
            push_free_stack(p);
        }
    }
}

// --- AXSPB ---

void depop_voice(const AXPB& pb) {
    depop_sum[main_l] += pb.dpop.aL;
    depop_sum[auxa_l] += pb.dpop.aAuxAL;
    depop_sum[auxb_l] += pb.dpop.aAuxBL;
    depop_sum[main_r] += pb.dpop.aR;
    depop_sum[auxa_r] += pb.dpop.aAuxAR;
    depop_sum[auxb_r] += pb.dpop.aAuxBR;
    depop_sum[main_s] += pb.dpop.aS;
    depop_sum[auxa_s] += pb.dpop.aAuxAS;
    depop_sum[auxb_s] += pb.dpop.aAuxBS;
}

// A bus starts the frame at the sum and steps toward zero by at most 20 per sample.
void depop_fade(FrameCommand& command) {
    for (int b = 0; b < bus_count; ++b) {
        s32& sum = depop_sum[b];
        if (sum / frame_samples) {
            const s32 delta = std::clamp<s32>(sum / frame_samples, -20, 20);
            command.studio_value[b] = sum;
            sum -= delta * frame_samples;
            command.studio_delta[b] = s16(-delta);
        } else {
            sum = 0;
            command.studio_value[b] = 0;
            command.studio_delta[b] = 0;
        }
    }
}

// --- AXVPB ---

void set_pb_default(AXVPB* p) {
    p->pb.state = AX_PB_STATE_STOP;
    p->pb.itd.flag = 0;
    p->sync = default_sync;
    p->updateMS = p->updateCounter = 0;
    p->updateWrite = p->updateData;
    std::memset(p->pb.update.updNum, 0, sizeof p->pb.update.updNum);
    p->pb.lpf.on = 0;
}

void clear_dsp_updates(AXPB& pb) { std::memset(pb.update.updNum, 0, sizeof pb.update.updNum); }

// Copies what the game changed into the DSP's block and what the DSP changed back, by sync flag.
void service_vpb(AXVPB* p) {
    ++voice_count;
    AXPB& to = dsp.pb[p->index];
    AXPB& user = p->pb;
    const u32 sync = p->sync;
    if (sync == 0) {
        user.state = to.state;
        user.ve.currentVolume = to.ve.currentVolume;
        user.addr.currentAddressHi = to.addr.currentAddressHi;
        user.addr.currentAddressLo = to.addr.currentAddressLo;
        return;
    }
    if (sync & AX_SYNC_FLAG_COPYALL) {
        to = user;
        std::memcpy(dsp.updates[p->index], p->updateData, std::min<u32>(p->updateCounter, update_words) * sizeof(u16));
        return;
    }
    if (sync & AX_SYNC_FLAG_COPYSELECT) {
        to.srcSelect = user.srcSelect;
        to.coefSelect = user.coefSelect;
    }
    if (sync & AX_SYNC_FLAG_COPYMXRCTRL) to.mixerCtrl = user.mixerCtrl;
    if (sync & AX_SYNC_FLAG_COPYSTATE)
        to.state = user.state;
    else
        user.state = to.state;
    if (sync & AX_SYNC_FLAG_COPYTYPE) to.type = user.type;
    if (sync & AX_SYNC_FLAG_COPYAXPBMIX) to.mix = user.mix;
    if (sync & AX_SYNC_FLAG_COPYTSHIFT) {
        to.itd.targetShiftL = user.itd.targetShiftL;
        to.itd.targetShiftR = user.itd.targetShiftR;
    } else if (sync & AX_SYNC_FLAG_COPYITD) {
        to.itd = user.itd;
        std::memset(p->itdBuffer, 0, sizeof(AXPBITDBUFFER));
    }
    if (sync & AX_SYNC_FLAG_COPYUPDATE) {
        to.update = user.update;
        std::memcpy(dsp.updates[p->index], p->updateData, std::min<u32>(p->updateCounter, update_words) * sizeof(u16));
    }
    if (sync & AX_SYNC_FLAG_COPYDPOP) to.dpop = user.dpop;
    if (sync & AX_SYNC_FLAG_SWAPVOL) {
        user.ve.currentVolume = to.ve.currentVolume;
        to.ve.currentDelta = user.ve.currentDelta;
    } else if (sync & AX_SYNC_FLAG_COPYVOL) {
        to.ve = user.ve;
    }
    if (sync & AX_SYNC_FLAG_COPYFIR) to.fir = user.fir;
    if (sync & (AX_SYNC_FLAG_COPYLOOP | AX_SYNC_FLAG_COPYLOOPADDR | AX_SYNC_FLAG_COPYENDADDR | AX_SYNC_FLAG_COPYCURADDR)) {
        if (sync & AX_SYNC_FLAG_COPYLOOP) to.addr.loopFlag = user.addr.loopFlag;
        if (sync & AX_SYNC_FLAG_COPYLOOPADDR) {
            to.addr.loopAddressHi = user.addr.loopAddressHi;
            to.addr.loopAddressLo = user.addr.loopAddressLo;
        }
        if (sync & AX_SYNC_FLAG_COPYENDADDR) {
            to.addr.endAddressHi = user.addr.endAddressHi;
            to.addr.endAddressLo = user.addr.endAddressLo;
        }
        if (sync & AX_SYNC_FLAG_COPYCURADDR) {
            to.addr.currentAddressHi = user.addr.currentAddressHi;
            to.addr.currentAddressLo = user.addr.currentAddressLo;
        } else {
            user.addr.currentAddressHi = to.addr.currentAddressHi;
            user.addr.currentAddressLo = to.addr.currentAddressLo;
        }
    } else if (sync & AX_SYNC_FLAG_COPYADDR) {
        to.addr = user.addr;
    } else {
        user.addr.currentAddressHi = to.addr.currentAddressHi;
        user.addr.currentAddressLo = to.addr.currentAddressLo;
    }
    if (sync & AX_SYNC_FLAG_COPYADPCM) to.adpcm = user.adpcm;
    if (sync & AX_SYNC_FLAG_COPYRATIO) {
        to.src.ratioHi = user.src.ratioHi;
        to.src.ratioLo = user.src.ratioLo;
    } else if (sync & AX_SYNC_FLAG_COPYSRC) {
        to.src = user.src;
    }
    if (sync & AX_SYNC_FLAG_COPYADPCMLOOP) to.adpcmLoop = user.adpcmLoop;
    // As the SDK: new coefficients skip a whole-filter copy requested in the same frame.
    if (sync & AX_SYNC_FLAG_COPYLPFCOEFS) {
        to.lpf.a0 = user.lpf.a0;
        to.lpf.b0 = user.lpf.b0;
        return;
    }
    if (sync & AX_SYNC_FLAG_COPYLPF) to.lpf = user.lpf;
}

// Over the DSP budget: the voice stops now and its callback runs from the frame interrupt.
void dump_vpb(AXVPB* p) {
    AXPB& to = dsp.pb[p->index];
    if (to.state == AX_PB_STATE_RUN) depop_voice(to);
    p->pb.state = to.state = AX_PB_STATE_STOP;
    clear_dsp_updates(to);
    push_callback_stack(p);
    counters.dropped.fetch_add(1, std::memory_order_relaxed);
}

void sync_pbs(u32 less_dsp_cycles) {
    voice_count = 0;
    u32 cycles = cl_cycles + 0x10000 - 0x55F0 + less_dsp_cycles;
    for (u32 i = AX_PRIORITY_STACKS - 1; i; --i) {
        for (AXVPB* p = stack_head[i]; p; p = next_of(p)) {
            if (p->depop) depop_voice(dsp.pb[p->index]);
            if (p->pb.state == AX_PB_STATE_RUN || p->updateCounter != 0) {
                if (p->pb.srcSelect != 2) cycles += src_cycles[std::min<u16>(p->pb.src.ratioHi, 4)];
                if (p->pb.lpf.on) cycles += 555;
                const u16 control = p->pb.mixerCtrl;
                cycles += main_mix_cycles[control & 0xF] + aux_mix_cycles[(control >> 4) & 0x1F] +
                          aux_mix_cycles[(control >> 9) & 0x1F] + 0x8C;
                if (max_dsp_cycles > cycles)
                    service_vpb(p);
                else
                    dump_vpb(p);
            } else {
                service_vpb(p);
            }
            p->sync = 0;
            p->depop = 0;
            p->updateMS = p->updateCounter = 0;
            p->updateWrite = p->updateData;
        }
    }
    recorded_dsp_cycles = cycles;
    for (AXVPB* p = stack_head[0]; p; p = next_of(p)) {
        AXPB& to = dsp.pb[p->index];
        if (p->depop) depop_voice(to);
        p->depop = 0;
        to.state = AX_PB_STATE_STOP;
        clear_dsp_updates(to);
    }
}

void vpb_init() {
    max_dsp_cycles = OS_BUS_CLOCK / 400;
    recorded_dsp_cycles = 0;
    std::memset(itd_buffers, 0, sizeof itd_buffers);
    std::memset(voices, 0, sizeof voices);
    for (u32 i = 0; i < AX_MAX_VOICES; ++i) {
        AXVPB* p = &voices[i];
        p->index = i;
        p->updateWrite = p->updateData;
        p->itdBuffer = &itd_buffers[i];
        set_pb_default(p);
        p->priority = 1;
        push_free_stack(p);
    }
}

// --- AXAux ---

void aux_init() {
    aux_callback_a = aux_callback_b = nullptr;
    aux_context_a = aux_context_b = nullptr;
    aux_write = 0;
    aux_read = 1;
    dpl2_write = 0;
    dpl2_read = 1;
    aux_cpu = 2;
    std::memset(&aux, 0, sizeof aux);
}

void process_aux() {
    constexpr int n = frame_samples;
    if (aux_callback_a) {
        s32* a = aux.a[aux_cpu];
        if (cl_mode == AX_MODE_DPL2) {
            AX_AUX_DATA_DPL2 data{a, a + n, a + 2 * n, aux.b[aux_cpu] + 2 * n};
            aux_callback_a(&data, aux_context_a);
        } else {
            AX_AUX_DATA data{a, a + n, a + 2 * n};
            aux_callback_a(&data, aux_context_a);
        }
    }
    if (aux_callback_b && cl_mode != AX_MODE_DPL2) {
        s32* b = aux.b[aux_cpu];
        AX_AUX_DATA data{b, b + n, b + 2 * n};
        aux_callback_b(&data, aux_context_b);
    }
    aux_write = (aux_write + 1) % 3;
    aux_read = (aux_read + 1) % 3;
    dpl2_write = (dpl2_write + 1) & 1;
    dpl2_read = (dpl2_read + 1) & 1;
    aux_cpu = (aux_cpu + 1) % 3;
}

// --- AXCL ---

void build_next_command() {
    u32 cycles = 0x1A9 + 0x2E44;
    if (cl_mode == AX_MODE_STEREO) cycles += 0x546;
    if (cl_mode == AX_MODE_SURROUND) cycles += 0x5E6;
    FrameCommand& c = next_command;
    c.mode = cl_mode;
    c.aux_a = aux_callback_a != nullptr;
    c.aux_b = cl_mode != AX_MODE_DPL2 && aux_callback_b != nullptr;
    if (cl_mode == AX_MODE_DPL2) {
        if (c.aux_a) cycles += 0xDED;
        cycles += 0xDED;
    } else {
        if (c.aux_a) cycles += 0xDED;
        if (c.aux_b) cycles += 0xDED;
    }
    c.compressor = compressor != 0;
    if (c.compressor) cycles += 0xBB8;
    cycles += 0x2710 + 2;
    c.aux_write = aux_write;
    c.aux_read = aux_read;
    c.dpl2_write = dpl2_write;
    c.dpl2_read = dpl2_read;
    c.output = output_frame;
    cl_cycles = cycles;
}

// --- AXProf ---

AXPROFILE* next_profile() {
    if (!profiles_initialized) return nullptr;
    AXPROFILE* profile = &profiles[current_profile];
    current_profile = (current_profile + 1) % profile_count;
    return profile;
}

// --- AXOut ---

void program_dma(u32 buffer) { starlet::ai::init_dma(output_buffers[buffer], sizeof output_buffers[buffer]); }

void new_frame() {
    __AXLocalProfile.axFrameStart = u64(platform::time());
    sync_pbs(0);
    frame_command = next_command;
    depop_fade(frame_command);
    const Memory memory{platform::aram(), platform::aram_bytes()};
    run_frame(dsp, frame_command, aux, memory, __AXCompressorTable, output_buffers[frame_command.output]);
    counters.frames.fetch_add(1, std::memory_order_relaxed);
    service_callback_stack();
    __AXLocalProfile.auxProcessingStart = u64(platform::time());
    process_aux();
    __AXLocalProfile.auxProcessingEnd = u64(platform::time());
    __AXLocalProfile.userCallbackStart = u64(platform::time());
    if (user_callback) user_callback();
    __AXLocalProfile.userCallbackEnd = u64(platform::time());
    build_next_command();
    output_frame += 1;
    if (output_buffer_mode == 1) {
        output_frame %= 3;
    } else {
        output_frame &= 1;
        program_dma(output_frame);
    }
    __AXLocalProfile.axFrameEnd = u64(platform::time());
    __AXLocalProfile.axNumVoices = voice_count;
    if (AXPROFILE* profile = next_profile()) *profile = __AXLocalProfile;
    counters.voices.store(voice_count, std::memory_order_relaxed);
    counters.cycles.store(recorded_dsp_cycles, std::memory_order_relaxed);
    counters.max_cycles.store(max_dsp_cycles, std::memory_order_relaxed);
}

// The AI interrupt. The DSP finishes a frame within the interrupt, so it is always ready.
void ai_callback() {
    if (!running) return;
    new_frame();
    if (output_buffer_mode == 1) {
        program_dma(ai_dma_frame);
        ai_dma_frame = (ai_dma_frame + 1) % 3;
    }
}

} // namespace

AXPROFILE __AXLocalProfile;

namespace starlet::ax {

Stats stats() noexcept {
    Stats s;
    s.frames = counters.frames.load(std::memory_order_relaxed);
    s.dropped_voices = counters.dropped.load(std::memory_order_relaxed);
    s.voices = counters.voices.load(std::memory_order_relaxed);
    s.dsp_cycles = counters.cycles.load(std::memory_order_relaxed);
    s.max_dsp_cycles = counters.max_cycles.load(std::memory_order_relaxed);
    return s;
}

}

extern "C" {

void AXInit() { AXInitEx(0); }

// Mode 1 triple-buffers the AI DMA (the DMA plays the buffer mixed one interrupt earlier), mode 0
// double-buffers it (the buffer just mixed). The game calls AIInit first.
void AXInitEx(u32 outputBufferMode) {
    init_voice_stacks();
    reset(dsp);
    vpb_init();
    std::memset(depop_sum, 0, sizeof depop_sum);
    aux_init();
    cl_mode = AX_MODE_STEREO;
    compressor = 1;
    output_buffer_mode = outputBufferMode;
    output_frame = 0;
    ai_dma_frame = 0;
    std::memset(output_buffers, 0, sizeof output_buffers);
    next_command = FrameCommand{};
    frame_command = FrameCommand{};
    AIRegisterDMACallback(ai_callback);
    output_frame = output_buffer_mode == 1 ? 2 : 1;
    build_next_command();
    output_frame = 0;
    user_callback = nullptr;
    running = true;
    if (output_buffer_mode == 1) {
        program_dma(ai_dma_frame);
        ai_dma_frame = (ai_dma_frame + 1) & 1;
    } else {
        program_dma(output_frame);
    }
    AIStartDMA();
}

void AXQuit() {
    Interrupts disabled;
    init_voice_stacks();
    aux_callback_a = aux_callback_b = nullptr;
    user_callback = nullptr;
    running = false;
    AIStopDMA();
}

void AXFreeVoice(AXVPB* p) {
    Interrupts disabled;
    remove_from_stack(p);
    if (p->pb.state == AX_PB_STATE_RUN) p->depop = 1;
    set_pb_default(p);
    push_free_stack(p);
}

AXVPB* AXAcquireVoice(u32 priority, AXVoiceCallback callback, u32 userContext) {
    Interrupts disabled;
    AXVPB* p = pop_free_stack();
    if (!p) {
        for (u32 i = 1; i < priority; ++i) {
            p = pop_stack_from_bottom(i);
            if (p) {
                if (p->pb.state == AX_PB_STATE_RUN) p->depop = 1;
                if (p->callback) p->callback(p);
                break;
            }
        }
    }
    if (p) {
        push_stack_head(p, priority);
        p->callback = callback;
        p->userContext = userContext;
        set_pb_default(p);
    }
    return p;
}

void AXSetVoicePriority(AXVPB* p, u32 priority) {
    Interrupts disabled;
    remove_from_stack(p);
    push_stack_head(p, priority);
}

void AXRegisterAuxACallback(AXAuxCallback callback, void* context) {
    aux_callback_a = callback;
    aux_context_a = context;
}

void AXRegisterAuxBCallback(AXAuxCallback callback, void* context) {
    aux_callback_b = callback;
    aux_context_b = context;
}

void AXSetMode(u32 mode) { cl_mode = mode; }
u32 AXGetMode() { return cl_mode; }
void AXSetCompressor(u32 on) { compressor = on; }

// Single-stepping is a debug-build DSP feature; release builds store the flag and never read it.
void AXSetStepMode(u32) {}

AXCallback AXRegisterCallback(AXCallback callback) {
    const AXCallback previous = user_callback;
    Interrupts disabled;
    user_callback = callback;
    return previous;
}

void AXInitProfile(AXPROFILE* profile, u32 maxProfiles) {
    profiles = profile;
    profile_count = maxProfiles;
    current_profile = 0;
    profiles_initialized = true;
}

u32 AXGetProfile() {
    Interrupts disabled;
    u32 n = current_profile;
    if (n) n -= 1;
    current_profile = 0;
    return n;
}

void AXSetVoiceSrcType(AXVPB* p, u32 type) {
    Interrupts disabled;
    switch (type) {
    case AX_SRC_TYPE_NONE: p->pb.srcSelect = 2; break;
    case AX_SRC_TYPE_LINEAR: p->pb.srcSelect = 1; break;
    case AX_SRC_TYPE_4TAP_8K: p->pb.srcSelect = 0; p->pb.coefSelect = 0; break;
    case AX_SRC_TYPE_4TAP_12K: p->pb.srcSelect = 0; p->pb.coefSelect = 1; break;
    case AX_SRC_TYPE_4TAP_16K: p->pb.srcSelect = 0; p->pb.coefSelect = 2; break;
    }
    p->sync |= AX_SYNC_FLAG_COPYSELECT;
}

void AXSetVoiceState(AXVPB* p, u16 state) {
    Interrupts disabled;
    p->pb.state = state;
    p->sync |= AX_SYNC_FLAG_COPYSTATE;
    if (state == AX_PB_STATE_STOP) p->depop = 1;
}

void AXSetVoiceType(AXVPB* p, u16 type) {
    Interrupts disabled;
    p->pb.type = type;
    p->sync |= AX_SYNC_FLAG_COPYTYPE;
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix) {
    // Each nonzero word turns on its bus or that bus's ramp, in AXPBMIX order.
    constexpr u16 bits[18] = {0x1, 0x9, 0x2, 0xA, 0x10, 0x50, 0x20, 0x60, 0x200,
                              0xA00, 0x400, 0xC00, 0x1000, 0x3000, 0x4, 0xC, 0x80, 0x180};
    u16 words[18];
    std::memcpy(words, mix, sizeof words);
    u16 control = 0;
    for (int i = 0; i < 18; ++i)
        if (words[i]) control |= bits[i];
    Interrupts disabled;
    p->pb.mix = *mix;
    p->pb.mixerCtrl = control;
    p->sync |= AX_SYNC_FLAG_COPYAXPBMIX | AX_SYNC_FLAG_COPYMXRCTRL;
}

void AXSetVoiceItdOn(AXVPB* p) {
    Interrupts disabled;
    p->pb.itd.flag = 1;
    p->pb.itd.shiftL = p->pb.itd.shiftR = p->pb.itd.targetShiftL = p->pb.itd.targetShiftR = 0;
    p->sync &= ~AX_SYNC_FLAG_COPYTSHIFT;
    p->sync |= AX_SYNC_FLAG_COPYITD;
}

void AXSetVoiceItdTarget(AXVPB* p, u16 lShift, u16 rShift) {
    Interrupts disabled;
    p->pb.itd.targetShiftL = lShift;
    p->pb.itd.targetShiftR = rShift;
    p->sync |= AX_SYNC_FLAG_COPYTSHIFT;
}

void AXSetVoiceUpdateIncrement(AXVPB* p) {
    Interrupts disabled;
    p->updateMS++;
    p->sync |= AX_SYNC_FLAG_COPYUPDATE;
}

void AXSetVoiceUpdateWrite(AXVPB* p, u16 param, u16 data) {
    Interrupts disabled;
    if (p->updateCounter + 2 > update_words) platform::panic("PB update block exceeded 128 words");
    p->updateCounter += 2;
    *p->updateWrite++ = param;
    *p->updateWrite++ = data;
    p->sync |= AX_SYNC_FLAG_COPYUPDATE;
}

void AXSetVoiceDpop(AXVPB* p, AXPBDPOP* dpop) {
    Interrupts disabled;
    p->pb.dpop = *dpop;
    p->sync |= AX_SYNC_FLAG_COPYDPOP;
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve) {
    Interrupts disabled;
    p->pb.ve = *ve;
    p->sync |= AX_SYNC_FLAG_COPYVOL;
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta) {
    Interrupts disabled;
    p->pb.ve.currentDelta = delta;
    p->sync |= AX_SYNC_FLAG_SWAPVOL;
}

void AXSetVoiceFir(AXVPB* p, AXPBFIR* fir) {
    Interrupts disabled;
    p->pb.fir = *fir;
    p->sync |= AX_SYNC_FLAG_COPYFIR;
}

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr) {
    Interrupts disabled;
    p->pb.addr = *addr;
    // PCM: no prediction, unity gain for the format's gain scale.
    if (addr->format == AX_PB_FORMAT_PCM16 || addr->format == AX_PB_FORMAT_PCM8) {
        std::memset(p->pb.adpcm.a, 0, sizeof p->pb.adpcm.a);
        p->pb.adpcm.gain = addr->format == AX_PB_FORMAT_PCM16 ? 0x0800 : 0x0100;
        p->pb.adpcm.pred_scale = 0;
        p->pb.adpcm.yn1 = p->pb.adpcm.yn2 = 0;
    }
    p->sync &= ~(AX_SYNC_FLAG_COPYLOOP | AX_SYNC_FLAG_COPYLOOPADDR | AX_SYNC_FLAG_COPYENDADDR | AX_SYNC_FLAG_COPYCURADDR);
    p->sync |= AX_SYNC_FLAG_COPYADDR | AX_SYNC_FLAG_COPYADPCM;
}

void AXSetVoiceLoop(AXVPB* p, u16 loop) {
    Interrupts disabled;
    p->pb.addr.loopFlag = loop;
    p->sync |= AX_SYNC_FLAG_COPYLOOP;
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr) {
    Interrupts disabled;
    p->pb.addr.loopAddressHi = u16(addr >> 16);
    p->pb.addr.loopAddressLo = u16(addr);
    p->sync |= AX_SYNC_FLAG_COPYLOOPADDR;
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr) {
    Interrupts disabled;
    p->pb.addr.endAddressHi = u16(addr >> 16);
    p->pb.addr.endAddressLo = u16(addr);
    p->sync |= AX_SYNC_FLAG_COPYENDADDR;
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr) {
    Interrupts disabled;
    p->pb.addr.currentAddressHi = u16(addr >> 16);
    p->pb.addr.currentAddressLo = u16(addr);
    p->sync |= AX_SYNC_FLAG_COPYCURADDR;
}

void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm) {
    Interrupts disabled;
    p->pb.adpcm = *adpcm;
    p->sync |= AX_SYNC_FLAG_COPYADPCM;
}

void AXSetVoiceSrc(AXVPB* p, AXPBSRC* src) {
    Interrupts disabled;
    p->pb.src = *src;
    p->sync &= ~AX_SYNC_FLAG_COPYRATIO;
    p->sync |= AX_SYNC_FLAG_COPYSRC;
}

void AXSetVoiceSrcRatio(AXVPB* p, f32 ratio) {
    Interrupts disabled;
    const f32 scaled = 65536.0f * ratio;
    u32 r = scaled > 0 ? (scaled < 4294967040.0f ? u32(scaled) : 0xFFFFFFFFu) : 0;
    if (r > 0x40000) r = 0x40000;
    p->pb.src.ratioHi = u16(r >> 16);
    p->pb.src.ratioLo = u16(r);
    p->sync |= AX_SYNC_FLAG_COPYRATIO;
}

void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* adpcmloop) {
    Interrupts disabled;
    p->pb.adpcmLoop = *adpcmloop;
    p->sync |= AX_SYNC_FLAG_COPYADPCMLOOP;
}

void AXSetMaxDspCycles(u32 cycles) { max_dsp_cycles = cycles; }
u32 AXGetMaxDspCycles() { return max_dsp_cycles; }
u32 AXGetDspCycles() { return recorded_dsp_cycles; }

void AXSetVoiceLpf(AXVPB* p, AXPBLPF* lpf) {
    Interrupts disabled;
    p->pb.lpf = *lpf;
    p->sync |= AX_SYNC_FLAG_COPYLPF;
}

void AXSetVoiceLpfCoefs(AXVPB* p, u16 a0, u16 b0) {
    Interrupts disabled;
    p->pb.lpf.a0 = a0;
    p->pb.lpf.b0 = b0;
    p->sync |= AX_SYNC_FLAG_COPYLPFCOEFS;
}

// One-pole low-pass with its corner at freq Hz (at most 16000), in single precision as the SDK.
void AXGetLpfCoefs(u16 freq, u16* a0, u16* b0) {
    const f32 pi = 3.14159265358979323846f;
    const f32 cc = 2.0f - std::cos((pi * 2) * f32(freq) / 32000.0f);
    const f32 bb = std::sqrt(cc * cc - 1.0f) - cc;
    *b0 = u16(s32(32768.0f * -bb));
    *a0 = u16(0x7FFF - *b0);
}

}
