#ifndef STARLET_COMPAT_GQR_H
#define STARLET_COMPAT_GQR_H

/* Gekko graphics quantization registers: how psq_l and psq_st convert between
 * memory and paired singles. A GQR holds the load type in bits 16-18 and the
 * load scale in bits 24-29, the store type in bits 0-2 and the store scale in
 * bits 8-13; types are 0 f32, 4 u8, 5 u16, 6 s8, 7 s16; scales are signed
 * 6-bit powers of two. Loads multiply by 2^-scale, stores by 2^scale, clamp to
 * the type's range and truncate. Quantized words in memory are big-endian, as
 * on the console. */

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline float gqr_scale_(unsigned field) { return __builtin_ldexpf(1.0f, (int)((field & 0x3f) ^ 0x20) - 0x20); }
static inline unsigned gqr_load_type(uint32_t gqr) { return (gqr >> 16) & 7; }
static inline unsigned gqr_store_type(uint32_t gqr) { return gqr & 7; }
static inline unsigned gqr_size(unsigned type) { return type == 0 ? 4 : (type == 4 || type == 6) ? 1 : 2; }

static inline float gqr_load(uint32_t gqr, const void* address)
{
    const uint8_t* p = (const uint8_t*)address;
    unsigned type = gqr_load_type(gqr);
    float value;
    switch (type) {
    case 4: value = p[0]; break;
    case 6: value = (int8_t)p[0]; break;
    case 5: value = (uint16_t)((p[0] << 8) | p[1]); break;
    case 7: value = (int16_t)((p[0] << 8) | p[1]); break;
    default: {
        uint32_t word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        memcpy(&value, &word, 4);
        return value;
    }
    }
    return value * (1.0f / gqr_scale_(gqr >> 24));
}

static inline void gqr_store(uint32_t gqr, void* address, float value)
{
    uint8_t* p = (uint8_t*)address;
    unsigned type = gqr_store_type(gqr);
    if (type == 0) {
        uint32_t word;
        memcpy(&word, &value, 4);
        p[0] = word >> 24, p[1] = word >> 16, p[2] = word >> 8, p[3] = word;
        return;
    }
    float scaled = value * gqr_scale_(gqr >> 8);
    float low = type == 4 || type == 5 ? 0.0f : type == 6 ? -128.0f : -32768.0f;
    float high = type == 4 ? 255.0f : type == 5 ? 65535.0f : type == 6 ? 127.0f : 32767.0f;
    scaled = scaled < low ? low : scaled > high ? high : scaled;
    int32_t integer = (int32_t)scaled;
    if (type == 4 || type == 6) {
        p[0] = (uint8_t)integer;
    } else {
        p[0] = (uint8_t)(integer >> 8), p[1] = (uint8_t)integer;
    }
}

#ifdef __cplusplus
}
#endif

#endif
