#pragma once

// The AX voice library types and functions starlet::ax implements, with the console SDK's layouts.
// Parameter blocks are native structs: every field keeps its console order and type, pointers are
// native. Sample data in ARAM stays big-endian as on the console.

#include "starlet/sdk/os.h"

namespace starlet::sdk {

struct AXPBMIX {
    u16 vL, vDeltaL;
    u16 vR, vDeltaR;
    u16 vAuxAL, vDeltaAuxAL;
    u16 vAuxAR, vDeltaAuxAR;
    u16 vAuxBL, vDeltaAuxBL;
    u16 vAuxBR, vDeltaAuxBR;
    u16 vAuxBS, vDeltaAuxBS;
    u16 vS, vDeltaS;
    u16 vAuxAS, vDeltaAuxAS;
};

struct AXPBITD {
    u16 flag;
    u16 bufferHi;
    u16 bufferLo;
    u16 shiftL;
    u16 shiftR;
    u16 targetShiftL;
    u16 targetShiftR;
};

struct AXPBUPDATE {
    u16 updNum[5];
    u16 dataHi;
    u16 dataLo;
};

struct AXPBDPOP {
    s16 aL;
    s16 aAuxAL;
    s16 aAuxBL;
    s16 aR;
    s16 aAuxAR;
    s16 aAuxBR;
    s16 aS;
    s16 aAuxAS;
    s16 aAuxBS;
};

struct AXPBVE {
    u16 currentVolume;
    s16 currentDelta;
};

struct AXPBFIR {
    u16 numCoefs;
    u16 coefsHi;
    u16 coefsLo;
};

struct AXPBADDR {
    u16 loopFlag;
    u16 format;
    u16 loopAddressHi;
    u16 loopAddressLo;
    u16 endAddressHi;
    u16 endAddressLo;
    u16 currentAddressHi;
    u16 currentAddressLo;
};

struct AXPBADPCM {
    u16 a[8][2];
    u16 gain;
    u16 pred_scale;
    u16 yn1;
    u16 yn2;
};

struct AXPBSRC {
    u16 ratioHi;
    u16 ratioLo;
    u16 currentAddressFrac;
    u16 last_samples[4];
};

struct AXPBADPCMLOOP {
    u16 loop_pred_scale;
    u16 loop_yn1;
    u16 loop_yn2;
};

struct AXPBLPF {
    u16 on;
    u16 yn1;
    u16 a0;
    u16 b0;
};

struct AXPB {
    u16 nextHi;
    u16 nextLo;
    u16 currHi;
    u16 currLo;
    u16 srcSelect;
    u16 coefSelect;
    u16 mixerCtrl;
    u16 state;
    u16 type;
    AXPBMIX mix;
    AXPBITD itd;
    AXPBUPDATE update;
    AXPBDPOP dpop;
    AXPBVE ve;
    AXPBFIR fir;
    AXPBADDR addr;
    AXPBADPCM adpcm;
    AXPBSRC src;
    AXPBADPCMLOOP adpcmLoop;
    AXPBLPF lpf;
    u16 pad[25];
};
static_assert(sizeof(AXPB) == 0xF4, "the DSP's parameter block");

struct AXVPB {
    void* next;
    void* prev;
    void* next1;
    u32 priority;
    void (*callback)(void*);
    u32 userContext;
    u32 index;
    u32 sync;
    u32 depop;
    u32 updateMS;
    u32 updateCounter;
    u32 updateTotal;
    u16* updateWrite;
    u16 updateData[128];
    void* itdBuffer;
    AXPB pb;
};

struct AXPBITDBUFFER {
    s16 data[32];
};

struct AXPBU {
    u16 data[128];
};

struct AXSPB {
    u16 dpopLHi, dpopLLo;
    s16 dpopLDelta;
    u16 dpopRHi, dpopRLo;
    s16 dpopRDelta;
    u16 dpopSHi, dpopSLo;
    s16 dpopSDelta;
    u16 dpopALHi, dpopALLo;
    s16 dpopALDelta;
    u16 dpopARHi, dpopARLo;
    s16 dpopARDelta;
    u16 dpopASHi, dpopASLo;
    s16 dpopASDelta;
    u16 dpopBLHi, dpopBLLo;
    s16 dpopBLDelta;
    u16 dpopBRHi, dpopBRLo;
    s16 dpopBRDelta;
    u16 dpopBSHi, dpopBSLo;
    s16 dpopBSDelta;
};

struct AXPROFILE {
    u64 axFrameStart;
    u64 auxProcessingStart;
    u64 auxProcessingEnd;
    u64 userCallbackStart;
    u64 userCallbackEnd;
    u64 axFrameEnd;
    u32 axNumVoices;
};

// What an aux callback's first argument points at: 160 samples per channel.
struct AX_AUX_DATA {
    s32* l;
    s32* r;
    s32* s;
};

struct AX_AUX_DATA_DPL2 {
    s32* l;
    s32* r;
    s32* ls;
    s32* rs;
};

using AXCallback = void (*)();
using AXAuxCallback = void (*)(void* data, void* context);
using AXVoiceCallback = void (*)(void* voice);

constexpr u32 AX_MAX_VOICES = 64;
constexpr u32 AX_PRIORITY_STACKS = 32;
constexpr u32 AX_PRIORITY_FREE = 0;
constexpr u32 AX_PRIORITY_NODROP = 31;

constexpr u32 AX_SRC_TYPE_NONE = 0;
constexpr u32 AX_SRC_TYPE_LINEAR = 1;
constexpr u32 AX_SRC_TYPE_4TAP_8K = 2;
constexpr u32 AX_SRC_TYPE_4TAP_12K = 3;
constexpr u32 AX_SRC_TYPE_4TAP_16K = 4;

constexpr u16 AX_PB_STATE_STOP = 0;
constexpr u16 AX_PB_STATE_RUN = 1;
constexpr u16 AX_PB_TYPE_NORMAL = 0;
constexpr u16 AX_PB_TYPE_STREAM = 1;
constexpr u16 AX_PB_FORMAT_ADPCM = 0x00;
constexpr u16 AX_PB_FORMAT_PCM16 = 0x0A;
constexpr u16 AX_PB_FORMAT_PCM8 = 0x19;

constexpr u32 AX_MODE_STEREO = 0;
constexpr u32 AX_MODE_SURROUND = 1;
constexpr u32 AX_MODE_DPL2 = 2;

constexpr u32 AX_SYNC_FLAG_COPYALL = 1u << 31;
constexpr u32 AX_SYNC_FLAG_COPYLPFCOEFS = 1u << 22;
constexpr u32 AX_SYNC_FLAG_COPYLPF = 1u << 21;
constexpr u32 AX_SYNC_FLAG_COPYADPCMLOOP = 1u << 20;
constexpr u32 AX_SYNC_FLAG_COPYRATIO = 1u << 19;
constexpr u32 AX_SYNC_FLAG_COPYSRC = 1u << 18;
constexpr u32 AX_SYNC_FLAG_COPYADPCM = 1u << 17;
constexpr u32 AX_SYNC_FLAG_COPYCURADDR = 1u << 16;
constexpr u32 AX_SYNC_FLAG_COPYENDADDR = 1u << 15;
constexpr u32 AX_SYNC_FLAG_COPYLOOPADDR = 1u << 14;
constexpr u32 AX_SYNC_FLAG_COPYLOOP = 1u << 13;
constexpr u32 AX_SYNC_FLAG_COPYADDR = 1u << 12;
constexpr u32 AX_SYNC_FLAG_COPYFIR = 1u << 11;
constexpr u32 AX_SYNC_FLAG_SWAPVOL = 1u << 10;
constexpr u32 AX_SYNC_FLAG_COPYVOL = 1u << 9;
constexpr u32 AX_SYNC_FLAG_COPYDPOP = 1u << 8;
constexpr u32 AX_SYNC_FLAG_COPYUPDATE = 1u << 7;
constexpr u32 AX_SYNC_FLAG_COPYTSHIFT = 1u << 6;
constexpr u32 AX_SYNC_FLAG_COPYITD = 1u << 5;
constexpr u32 AX_SYNC_FLAG_COPYAXPBMIX = 1u << 4;
constexpr u32 AX_SYNC_FLAG_COPYTYPE = 1u << 3;
constexpr u32 AX_SYNC_FLAG_COPYSTATE = 1u << 2;
constexpr u32 AX_SYNC_FLAG_COPYMXRCTRL = 1u << 1;
constexpr u32 AX_SYNC_FLAG_COPYSELECT = 1u << 0;

} // namespace starlet::sdk

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {

// AX
void AXInit();
void AXInitEx(starlet::sdk::u32 outputBufferMode);
void AXQuit();

// AXAlloc
void AXFreeVoice(starlet::sdk::AXVPB* p);
starlet::sdk::AXVPB* AXAcquireVoice(starlet::sdk::u32 priority, starlet::sdk::AXVoiceCallback callback, starlet::sdk::u32 userContext);
void AXSetVoicePriority(starlet::sdk::AXVPB* p, starlet::sdk::u32 priority);

// AXAux
void AXRegisterAuxACallback(starlet::sdk::AXAuxCallback callback, void* context);
void AXRegisterAuxBCallback(starlet::sdk::AXAuxCallback callback, void* context);

// AXCL
void AXSetMode(starlet::sdk::u32 mode);
starlet::sdk::u32 AXGetMode();
void AXSetCompressor(starlet::sdk::u32 on);

// AXOut
extern starlet::sdk::AXPROFILE __AXLocalProfile;
void AXSetStepMode(starlet::sdk::u32 i);
starlet::sdk::AXCallback AXRegisterCallback(starlet::sdk::AXCallback callback);

// AXProf
void AXInitProfile(starlet::sdk::AXPROFILE* profile, starlet::sdk::u32 maxProfiles);
starlet::sdk::u32 AXGetProfile();

// AXVPB
void AXSetVoiceSrcType(starlet::sdk::AXVPB* p, starlet::sdk::u32 type);
void AXSetVoiceState(starlet::sdk::AXVPB* p, starlet::sdk::u16 state);
void AXSetVoiceType(starlet::sdk::AXVPB* p, starlet::sdk::u16 type);
void AXSetVoiceMix(starlet::sdk::AXVPB* p, starlet::sdk::AXPBMIX* mix);
void AXSetVoiceItdOn(starlet::sdk::AXVPB* p);
void AXSetVoiceItdTarget(starlet::sdk::AXVPB* p, starlet::sdk::u16 lShift, starlet::sdk::u16 rShift);
void AXSetVoiceUpdateIncrement(starlet::sdk::AXVPB* p);
void AXSetVoiceUpdateWrite(starlet::sdk::AXVPB* p, starlet::sdk::u16 param, starlet::sdk::u16 data);
void AXSetVoiceDpop(starlet::sdk::AXVPB* p, starlet::sdk::AXPBDPOP* dpop);
void AXSetVoiceVe(starlet::sdk::AXVPB* p, starlet::sdk::AXPBVE* ve);
void AXSetVoiceVeDelta(starlet::sdk::AXVPB* p, starlet::sdk::s16 delta);
void AXSetVoiceFir(starlet::sdk::AXVPB* p, starlet::sdk::AXPBFIR* fir);
void AXSetVoiceAddr(starlet::sdk::AXVPB* p, starlet::sdk::AXPBADDR* addr);
void AXSetVoiceLoop(starlet::sdk::AXVPB* p, starlet::sdk::u16 loop);
void AXSetVoiceLoopAddr(starlet::sdk::AXVPB* p, starlet::sdk::u32 addr);
void AXSetVoiceEndAddr(starlet::sdk::AXVPB* p, starlet::sdk::u32 addr);
void AXSetVoiceCurrentAddr(starlet::sdk::AXVPB* p, starlet::sdk::u32 addr);
void AXSetVoiceAdpcm(starlet::sdk::AXVPB* p, starlet::sdk::AXPBADPCM* adpcm);
void AXSetVoiceSrc(starlet::sdk::AXVPB* p, starlet::sdk::AXPBSRC* src);
void AXSetVoiceSrcRatio(starlet::sdk::AXVPB* p, starlet::sdk::f32 ratio);
void AXSetVoiceAdpcmLoop(starlet::sdk::AXVPB* p, starlet::sdk::AXPBADPCMLOOP* adpcmloop);
void AXSetMaxDspCycles(starlet::sdk::u32 cycles);
starlet::sdk::u32 AXGetMaxDspCycles();
starlet::sdk::u32 AXGetDspCycles();
void AXSetVoiceLpf(starlet::sdk::AXVPB* p, starlet::sdk::AXPBLPF* lpf);
void AXSetVoiceLpfCoefs(starlet::sdk::AXVPB* p, starlet::sdk::u16 a0, starlet::sdk::u16 b0);
void AXGetLpfCoefs(starlet::sdk::u16 freq, starlet::sdk::u16* a0, starlet::sdk::u16* b0);

// The compressor's gain table (the SDK's AXComp.c), which the port links in: 21 frames of 160
// gains, 11 attack frames then 10 release frames.
extern starlet::sdk::u16 __AXCompressorTable[3360];
}
#endif
