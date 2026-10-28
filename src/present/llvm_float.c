// Viewer build only (MSL_GX_RECORD): the square roots the float rules (tools/build/floatxform.py) put in place of llvm.sqrt / sqrtf / sqrt in the functions
// they select: the game's inline sqrtf (MWCC's math_ppc.h), a frsqrte seed refined by three double Newton steps whose (3 - x * g * g) is one fused fnmsub,
// then x * g (and frsp for the float form) - GALE01 0x800C28B8..0x800C28F0 and every other inline of it. libm's correctly rounded result is not the game's.
// refs/melee/src/MSL/math_ppc.h
#if MSL_GX_RECORD
extern double __frsqrte(double);

double msl_llvm_sqrt(double x)
{
    if (x > 0.0) {
        double g = __frsqrte(x);
        g = (0.5 * g) * __builtin_fma(-x, g * g, 3.0);
        g = (0.5 * g) * __builtin_fma(-x, g * g, 3.0);
        g = (0.5 * g) * __builtin_fma(-x, g * g, 3.0);
        return x * g;
    }
    return x;
}

float msl_llvm_sqrtf(float x)
{
    if (x > 0.0F) {
        double d = (double) x;
        double g = __frsqrte(d);
        g = (0.5 * g) * __builtin_fma(-d, g * g, 3.0);
        g = (0.5 * g) * __builtin_fma(-d, g * g, 3.0);
        g = (0.5 * g) * __builtin_fma(-d, g * g, 3.0);
        return (float) (d * g);
    }
    return x;
}
#endif
