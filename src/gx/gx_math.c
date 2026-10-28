#include "gx/msl_gx.h"

#if MSL_GX_RECORD

/* Arithmetic the viewer build must round as the console does on every host, in a file compiled without contraction.
 * runtime/math.c is built with -ffp-contract=fast (its fused sites are meant to fuse), which lets GCC with -mfma fuse a sum of
 * squares that the retail code adds unfused (GALE01 lbVector_Angle 0x8000D634..0x8000D64C: fmuls, fmuls, fmuls, fadds, fadds) and
 * a WebAssembly build, which has no scalar fused multiply-add, never fuses: the two hosts rounded differently. */
float msl_gx_len_sq(float x, float y, float z)
{
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    return (xx + yy) + zz;
}

#endif
