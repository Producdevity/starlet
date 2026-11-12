#ifndef STARLET_COMPAT_PPC_INTRINSICS_H
#define STARLET_COMPAT_PPC_INTRINSICS_H

/* Portable definitions of the Metrowerks PowerPC integer and cache intrinsics.
 * The floating-point ones are in ppc_float.h, which the game's math.h includes
 * after the host's. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int __cntlzw(uint32_t x) { return x ? __builtin_clz(x) : 32; }

/* rlwimi: insert rotl(value, shift) into target under the mask MB..ME (big-endian bit numbering). */
static inline uint32_t __rlwimi(uint32_t target, uint32_t value, int shift, int mb, int me)
{
    uint32_t rotated = shift ? (value << shift) | (value >> (32 - shift)) : value;
    uint32_t begin = 0xffffffffu >> mb;
    uint32_t end = me == 31 ? 0xffffffffu : ~(0xffffffffu >> (me + 1));
    uint32_t mask = mb <= me ? begin & end : begin | end;
    return (rotated & mask) | (target & ~mask);
}

static inline void __sync(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void __eieio(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
/* Cache maintenance has no effect on coherent host memory. */
static inline void __dcbf(void* base, int offset) { (void)base; (void)offset; }
static inline void __dcbst(void* base, int offset) { (void)base; (void)offset; }
static inline void __dcbi(void* base, int offset) { (void)base; (void)offset; }

/* dcbz zeroes the 32-byte cache block containing base + offset. */
static inline void __dcbz(void* base, int offset)
{
    uintptr_t address = ((uintptr_t)base + (intptr_t)offset) & ~(uintptr_t)31;
    __builtin_memset((void*)address, 0, 32);
}

/* Prefetch hints. */
static inline void __dcbt(void* base, int offset) { (void)base; (void)offset; }
static inline void __dcbtst(void* base, int offset) { (void)base; (void)offset; }

/* rlwinm: rotl(value, shift) under the mask MB..ME (big-endian bit numbering). */
static inline uint32_t __rlwinm(uint32_t value, int shift, int mb, int me) { return __rlwimi(0, value, shift, mb, me); }

/* Byte-reversed loads and stores: the word at base + offset in little-endian byte order. */
static inline uint32_t __lwbrx(const void* base, int offset)
{
    const uint8_t* p = (const uint8_t*)base + offset;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t __lhbrx(const void* base, int offset)
{
    const uint8_t* p = (const uint8_t*)base + offset;
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void __stwbrx(uint32_t value, void* base, int offset)
{
    uint8_t* p = (uint8_t*)base + offset;
    p[0] = (uint8_t)value, p[1] = (uint8_t)(value >> 8), p[2] = (uint8_t)(value >> 16), p[3] = (uint8_t)(value >> 24);
}
static inline void __sthbrx(uint16_t value, void* base, int offset)
{
    uint8_t* p = (uint8_t*)base + offset;
    p[0] = (uint8_t)value, p[1] = (uint8_t)(value >> 8);
}

#ifdef __cplusplus
}
#endif

#endif
