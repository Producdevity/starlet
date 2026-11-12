#ifndef STARLET_COMPAT_GX_FIFO_H
#define STARLET_COMPAT_GX_FIFO_H

/* The write-gather pipe at 0xCC008000. Writes append big-endian command bytes
 * to the current CPU FIFO target: a staging chunk the GPU thread decodes, or a
 * display list being recorded. */

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StarletGxStage {
    uint8_t* cur;
    uint8_t* end;
} StarletGxStage;

extern StarletGxStage starlet_gx_stage;

/* Arrays of 32-bit numbers the game fills natively for GXSetArray (not file
 * data): the GX converts them to canonical order while the registration lasts.
 * Release before the memory is reused. */
void starlet_gx_host_order_array(const void* base, unsigned long bytes);
void starlet_gx_release_array(const void* base);

/* Makes room for bytes more bytes (submits the chunk, or flags list overflow). */
void starlet_gx_reserve(unsigned bytes);

static inline void starlet_gx_u8(uint8_t value)
{
    if (starlet_gx_stage.end - starlet_gx_stage.cur < 1) starlet_gx_reserve(1);
    *starlet_gx_stage.cur++ = value;
}

static inline void starlet_gx_u16(uint16_t value)
{
    if (starlet_gx_stage.end - starlet_gx_stage.cur < 2) starlet_gx_reserve(2);
    starlet_gx_stage.cur[0] = (uint8_t)(value >> 8);
    starlet_gx_stage.cur[1] = (uint8_t)value;
    starlet_gx_stage.cur += 2;
}

static inline void starlet_gx_u32(uint32_t value)
{
    if (starlet_gx_stage.end - starlet_gx_stage.cur < 4) starlet_gx_reserve(4);
    starlet_gx_stage.cur[0] = (uint8_t)(value >> 24);
    starlet_gx_stage.cur[1] = (uint8_t)(value >> 16);
    starlet_gx_stage.cur[2] = (uint8_t)(value >> 8);
    starlet_gx_stage.cur[3] = (uint8_t)value;
    starlet_gx_stage.cur += 4;
}

static inline void starlet_gx_f32(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    starlet_gx_u32(bits);
}

#ifdef __cplusplus
}
#endif

/* The SDK's matrix writers take the pipe's address as a destination they ignore. */
#define GXWGFifo starlet_gx_stage

#define GX_WRITE_U8(val)  starlet_gx_u8((uint8_t)(val))
#define GX_WRITE_S8(val)  starlet_gx_u8((uint8_t)(int8_t)(val))
#define GX_WRITE_U16(val) starlet_gx_u16((uint16_t)(val))
#define GX_WRITE_S16(val) starlet_gx_u16((uint16_t)(int16_t)(val))
#define GX_WRITE_U32(val) starlet_gx_u32((uint32_t)(val))
#define GX_WRITE_S32(val) starlet_gx_u32((uint32_t)(int32_t)(val))
#define GX_WRITE_F32(val) starlet_gx_f32((float)(val))

#endif
