#ifndef STARLET_COMPAT_PPC_FLOAT_H
#define STARLET_COMPAT_PPC_FLOAT_H

/* Metrowerks PowerPC floating-point intrinsics. Macros, because glibc's math.h
 * already declares __fabs and friends as functions. Estimates (frsqrte, fres)
 * return the exact value: the game refines them with Newton steps, which leave
 * an exact input unchanged. fsel selects b for a >= 0 (including -0), c for
 * negative a or NaN. */

#define __fabs(x)          __builtin_fabs(x)
#define __fabsf(x)         __builtin_fabsf(x)
#define __fnabs(x)         (-__builtin_fabs(x))
#define __fnabsf(x)        (-__builtin_fabsf(x))
#define __fmadd(a, b, c)   __builtin_fma((a), (b), (c))
#define __fmsub(a, b, c)   __builtin_fma((a), (b), -(c))
#define __fnmadd(a, b, c)  (-__builtin_fma((a), (b), (c)))
#define __fnmsub(a, b, c)  (-__builtin_fma((a), (b), -(c)))
#define __fmadds(a, b, c)  __builtin_fmaf((a), (b), (c))
#define __fmsubs(a, b, c)  __builtin_fmaf((a), (b), -(c))
#define __fnmadds(a, b, c) (-__builtin_fmaf((a), (b), (c)))
#define __fnmsubs(a, b, c) (-__builtin_fmaf((a), (b), -(c)))
#define __fsel(a, b, c)    ((double)(a) >= 0.0 ? (double)(b) : (double)(c))
#define __fsels(a, b, c)   ((float)(a) >= 0.0f ? (float)(b) : (float)(c))
#define __frsqrte(x)       (1.0 / __builtin_sqrt((double)(x)))
#define __fres(x)          (1.0f / (float)(x))
#define __fsqrt(x)         __builtin_sqrt(x)
#define __fsqrts(x)        __builtin_sqrtf(x)

#endif
