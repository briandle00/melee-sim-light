/* Float rules of the viewer build's particle library and display (shared by present/ps_lib.c and present/ps_disp.c). */
#ifndef PS_FP_H
#define PS_FP_H

#include <math.h>

extern double __frsqrte(double);

/* the decomp's math_ppc.h sqrtf, which MWCC inlines wherever the game calls it: a frsqrte seed, three double Newton steps (each 3 - x*g*g one
 * fused fnmsub), x * g, frsp (GALE01 0x8039DBA8, 0x8039E218, 0x803A1298, ...). libc's is correctly rounded, which the game's is not always. */
static inline float ps_gekko_sqrtf(float x)
{
    if (x > 0.0F) {
        double g = __frsqrte((double) x);
        g = (0.5 * g) * __builtin_fma(-(double) x, g * g, 3.0);
        g = (0.5 * g) * __builtin_fma(-(double) x, g * g, 3.0);
        g = (0.5 * g) * __builtin_fma(-(double) x, g * g, 3.0);
        return (float) ((double) x * g);
    }
    return x;
}
#define sqrtf(x) ps_gekko_sqrtf(x)

/* a product the game rounds (fmuls) before the add that would otherwise take it as a fused operand under -ffp-contract=fast */
static inline float ps_round(float v)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__("" : "+x"(v));
#else
    __asm__("" : "+r"(v)); /* (a general register off x86: the barrier is the point) */
#endif
    return v;
}

#endif
