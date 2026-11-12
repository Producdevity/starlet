#ifndef STARLET_COMPAT_PPC_PAIRED_H
#define STARLET_COMPAT_PPC_PAIRED_H

/* Gekko paired-single registers and the instructions the game's assembly uses,
 * as lane-wise single-precision operations in the same order. Multiply-adds are
 * fused, as on the hardware. Scalar single-precision instructions (fadds, fmuls,
 * fmadds, frsp, lfs, ...) write both lanes, as Gekko does in paired-single mode;
 * fmr writes ps0 only. Quantized loads and stores cover GQR0 (unscaled f32) only;
 * the quantized integer cases are converted at their call sites. */

#include "starlet/compat/ppc_float.h"
#include "starlet/compat/gqr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	float ps0;
	float ps1;
} ps_reg;

static inline ps_reg ps_pair(float ps0, float ps1)
{
	ps_reg r;
	r.ps0 = ps0;
	r.ps1 = ps1;
	return r;
}

static inline ps_reg ps_fill(float v) { return ps_pair(v, v); }

static inline const float* ps_addr_(const void* base, long offset) { return (const float*)((const char*)base + offset); }
static inline float* ps_waddr_(void* base, long offset) { return (float*)((char*)base + offset); }

/* psq_l fD, offset(base), 0, 0 */
static inline ps_reg psq_l(const void* base, long offset)
{
	const float* p = ps_addr_(base, offset);
	return ps_pair(p[0], p[1]);
}

/* psq_l fD, offset(base), 1, 0: ps1 becomes 1.0 */
static inline ps_reg psq_l1(const void* base, long offset) { return ps_pair(*ps_addr_(base, offset), 1.0f); }

/* psq_st fS, offset(base), 0, 0 */
static inline void psq_st(ps_reg r, void* base, long offset)
{
	float* p = ps_waddr_(base, offset);
	p[0]     = r.ps0;
	p[1]     = r.ps1;
}

/* psq_st fS, offset(base), 1, 0 */
static inline void psq_st1(ps_reg r, void* base, long offset) { *ps_waddr_(base, offset) = r.ps0; }

/* lfs / stfs */
static inline ps_reg ps_lfs(const void* base, long offset) { return ps_fill(*ps_addr_(base, offset)); }
static inline void ps_stfs(ps_reg r, void* base, long offset) { *ps_waddr_(base, offset) = r.ps0; }

/* Paired arithmetic: fD = fA op fB per lane, multiply-adds as fA * fC +/- fB. */
static inline ps_reg ps_add(ps_reg a, ps_reg b) { return ps_pair(a.ps0 + b.ps0, a.ps1 + b.ps1); }
static inline ps_reg ps_sub(ps_reg a, ps_reg b) { return ps_pair(a.ps0 - b.ps0, a.ps1 - b.ps1); }
static inline ps_reg ps_mul(ps_reg a, ps_reg c) { return ps_pair(a.ps0 * c.ps0, a.ps1 * c.ps1); }
static inline ps_reg ps_div(ps_reg a, ps_reg b) { return ps_pair(a.ps0 / b.ps0, a.ps1 / b.ps1); }
static inline ps_reg ps_neg(ps_reg b) { return ps_pair(-b.ps0, -b.ps1); }
static inline ps_reg ps_abs(ps_reg b) { return ps_pair(__builtin_fabsf(b.ps0), __builtin_fabsf(b.ps1)); }
static inline ps_reg ps_madd(ps_reg a, ps_reg c, ps_reg b)
{
	return ps_pair(__builtin_fmaf(a.ps0, c.ps0, b.ps0), __builtin_fmaf(a.ps1, c.ps1, b.ps1));
}
static inline ps_reg ps_msub(ps_reg a, ps_reg c, ps_reg b)
{
	return ps_pair(__builtin_fmaf(a.ps0, c.ps0, -b.ps0), __builtin_fmaf(a.ps1, c.ps1, -b.ps1));
}
static inline ps_reg ps_nmadd(ps_reg a, ps_reg c, ps_reg b) { return ps_neg(ps_madd(a, c, b)); }
static inline ps_reg ps_nmsub(ps_reg a, ps_reg c, ps_reg b) { return ps_neg(ps_msub(a, c, b)); }

/* Scalar-lane forms: muls0/madds0 use fC.ps0 for both lanes, the 1 forms use fC.ps1. */
static inline ps_reg ps_muls0(ps_reg a, ps_reg c) { return ps_pair(a.ps0 * c.ps0, a.ps1 * c.ps0); }
static inline ps_reg ps_muls1(ps_reg a, ps_reg c) { return ps_pair(a.ps0 * c.ps1, a.ps1 * c.ps1); }
static inline ps_reg ps_madds0(ps_reg a, ps_reg c, ps_reg b)
{
	return ps_pair(__builtin_fmaf(a.ps0, c.ps0, b.ps0), __builtin_fmaf(a.ps1, c.ps0, b.ps1));
}
static inline ps_reg ps_madds1(ps_reg a, ps_reg c, ps_reg b)
{
	return ps_pair(__builtin_fmaf(a.ps0, c.ps1, b.ps0), __builtin_fmaf(a.ps1, c.ps1, b.ps1));
}

/* ps_sum0 fD, fA, fC, fB: (fA.ps0 + fB.ps1, fC.ps1); ps_sum1: (fC.ps0, fA.ps0 + fB.ps1) */
static inline ps_reg ps_sum0(ps_reg a, ps_reg c, ps_reg b) { return ps_pair(a.ps0 + b.ps1, c.ps1); }
static inline ps_reg ps_sum1(ps_reg a, ps_reg c, ps_reg b) { return ps_pair(c.ps0, a.ps0 + b.ps1); }

static inline ps_reg ps_merge00(ps_reg a, ps_reg b) { return ps_pair(a.ps0, b.ps0); }
static inline ps_reg ps_merge01(ps_reg a, ps_reg b) { return ps_pair(a.ps0, b.ps1); }
static inline ps_reg ps_merge10(ps_reg a, ps_reg b) { return ps_pair(a.ps1, b.ps0); }
static inline ps_reg ps_merge11(ps_reg a, ps_reg b) { return ps_pair(a.ps1, b.ps1); }

/* Scalar single-precision instructions; the result fills both lanes. */
static inline ps_reg fadds(ps_reg a, ps_reg b) { return ps_fill(a.ps0 + b.ps0); }
static inline ps_reg fsubs(ps_reg a, ps_reg b) { return ps_fill(a.ps0 - b.ps0); }
static inline ps_reg fmuls(ps_reg a, ps_reg c) { return ps_fill(a.ps0 * c.ps0); }
static inline ps_reg fdivs(ps_reg a, ps_reg b) { return ps_fill(a.ps0 / b.ps0); }
static inline ps_reg fmadds(ps_reg a, ps_reg c, ps_reg b) { return ps_fill(__builtin_fmaf(a.ps0, c.ps0, b.ps0)); }
static inline ps_reg fmsubs(ps_reg a, ps_reg c, ps_reg b) { return ps_fill(__builtin_fmaf(a.ps0, c.ps0, -b.ps0)); }
static inline ps_reg fnmadds(ps_reg a, ps_reg c, ps_reg b) { return ps_fill(-__builtin_fmaf(a.ps0, c.ps0, b.ps0)); }
static inline ps_reg fnmsubs(ps_reg a, ps_reg c, ps_reg b) { return ps_fill(-__builtin_fmaf(a.ps0, c.ps0, -b.ps0)); }

/* fres and frsqrte follow ppc_float.h; frsqrte is a double-precision instruction and writes ps0 only. */
static inline ps_reg fres(ps_reg b) { return ps_fill(__fres(b.ps0)); }
static inline ps_reg frsqrte(ps_reg d, ps_reg b)
{
	d.ps0 = (float)__frsqrte(b.ps0);
	return d;
}

/* ps_sel fD, fA, fC, fB: per lane fA >= 0 (including -0) ? fC : fB; a NaN fA selects fB. */
static inline ps_reg ps_sel(ps_reg a, ps_reg c, ps_reg b)
{
	return ps_pair(a.ps0 >= 0.0f ? c.ps0 : b.ps0, a.ps1 >= 0.0f ? c.ps1 : b.ps1);
}
static inline ps_reg ps_nabs(ps_reg b) { return ps_pair(-__builtin_fabsf(b.ps0), -__builtin_fabsf(b.ps1)); }
/* Lane-wise estimates, exact as in ppc_float.h. */
static inline ps_reg ps_res(ps_reg b) { return ps_pair(__fres(b.ps0), __fres(b.ps1)); }
static inline ps_reg ps_rsqrte(ps_reg b) { return ps_pair((float)__frsqrte(b.ps0), (float)__frsqrte(b.ps1)); }

/* Quantized psq_l / psq_st through the GQR value gqr (gqr.h); W=1 forms load ps1 = 1.0 or store ps0 only. */
static inline ps_reg psq_lq(const void* base, long offset, uint32_t gqr)
{
	const char* p = (const char*)base + offset;
	return ps_pair(gqr_load(gqr, p), gqr_load(gqr, p + gqr_size(gqr_load_type(gqr))));
}
static inline ps_reg psq_lq1(const void* base, long offset, uint32_t gqr) { return ps_pair(gqr_load(gqr, (const char*)base + offset), 1.0f); }
static inline void psq_stq(ps_reg r, void* base, long offset, uint32_t gqr)
{
	char* p = (char*)base + offset;
	gqr_store(gqr, p, r.ps0);
	gqr_store(gqr, p + gqr_size(gqr_store_type(gqr)), r.ps1);
}
static inline void psq_stq1(ps_reg r, void* base, long offset, uint32_t gqr) { gqr_store(gqr, (char*)base + offset, r.ps0); }

#ifdef __cplusplus
}
#endif

#endif
