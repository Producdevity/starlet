#pragma once

// The Wii and GameCube SDK's THP movie types and the decoder calls that starlet::thp_sdk implements,
// with the console SDK's layouts. A port's decompilation declares the same types; it asserts at
// compile time that they match these. Functions have C linkage, so the game's calls bind to them by
// name.

#include <cstdint>

namespace starlet::sdk {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using f32 = float;
using BOOL = int;

struct THPHeader {
    char magic[4];
    u32 version;
    u32 bufSize;
    u32 audioMaxSamples;
    f32 frameRate;
    u32 numFrames;
    u32 firstFrameSize;
    u32 movieDataSize;
    u32 compInfoDataOffsets;
    u32 offsetDataOffsets;
    u32 movieDataOffsets;
    u32 finalFrameDataOffsets;
};

struct THPFrameCompInfo {
    u32 numComponents;
    u8 frameComp[16];
};

struct THPVideoInfo {
    u32 xSize;
    u32 ySize;
    u32 videoType;
};

struct THPAudioInfo {
    u32 sndChannels;
    u32 sndFrequency;
    u32 sndNumSamples;
    u32 sndNumTracks;
};

} // namespace starlet::sdk

#ifndef STARLET_SDK_TYPES_ONLY
extern "C" {

starlet::sdk::BOOL THPInit();
// The frame as read from the disc (big-endian); 0 or a nonzero error.
starlet::sdk::s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
// The audio record as read from the disc; returns the samples per channel.
starlet::sdk::u32 THPAudioDecode(starlet::sdk::s16* audioBuffer, starlet::sdk::u8* audioFrame, starlet::sdk::s32 flag);

} // extern "C"
#endif
