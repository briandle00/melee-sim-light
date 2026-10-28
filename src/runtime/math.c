#include <MSL/math.h>
#include <MSL/math_ppc.h>

#include <MetroTRK/intrinsics.h>

#include "match.h"

// Gameplay math projection of refs/melee/src/melee/lb/lbrefract.c:742-961.
// The refraction renderer is excluded, but these global libm replacements are
// called throughout fighter, item, stage, and collision source.
#define SIGN_BIT (1 << 31)
#define BITWISE(f) (*(u32*) &(f))
#define SIGNED_BITWISE(f) ((s32) BITWISE(f))
#define GET_SIGN_BIT(f) (SIGNED_BITWISE(f) & SIGN_BIT)
#define BITWISE_PI_2 0x3FC90FDB

extern float MSL_TrigF_80400770[], MSL_TrigF_80400774[];
#define NAN MSL_TrigF_80400770[0]
#define INF MSL_TrigF_80400774[0]

static inline float lbRefract_80022DF8(float x);

#ifndef MSL_NATIVE_INLINE_FMADDS
float __fmadds(float a, float b, float c)
{
#ifdef MSL_CORE_NATIVE
    return __builtin_fmaf(a, b, c);
#else
    float result;
    __asm__("fmadds %0,%1,%2,%3" : "=f"(result) : "f"(a), "f"(b), "f"(c));
    return result;
#endif
}
#endif

// MetroWerks emits a scalar-single fnmsubs instruction for this intrinsic.
float __fnmsubs(float a, float b, float c)
{
#ifdef MSL_CORE_NATIVE
    // Retail fnmsubs negates a fused (a*b-c), including the result sign of an
    // exact zero. refs/melee/build/GALE01/asm/melee/ft/fighter.s.
    return -__builtin_fmaf(a, b, -c);
#else
    float result;
    __asm__("fnmsubs %0,%1,%2,%3" : "=f"(result) : "f"(a), "f"(b), "f"(c));
    return result;
#endif
}

float msl_dolphin_fnmsubs(float a, float b, float c)
{
    float result = __fnmsubs(a, b, c);

    // Legacy Dolphin JIT captures can use fused c - a*b, including offline
    // games. This recording capability is independent of scene, CPU slots,
    // and BrawlOffscreenDamage; retail/corrected JIT uses negate-after-subtract.
    if (!msl_core_uses_online_fnmsubs_zero()) {
        return result;
    }

    // A product of two binary32 values is exact in binary64, so this test
    // distinguishes true cancellation from a small nonzero result that
    // merely underflowed when rounded back to binary32.  The latter already
    // has the correct sign in the retail instruction result.
    if (result == 0.0F && (double) c == (double) a * (double) b) {
        u32 product_sign = (BITWISE(a) ^ BITWISE(b)) & SIGN_BIT;
        u32 c_sign = BITWISE(c) & SIGN_BIT;

        // The DOL sites are scalar-single fnmsubs instructions
        // (refs/melee/build/GALE01/asm/melee/ft/fighter.s at
        // 0x8006B9C8/0x8006B9E4 and 0x8006BB34/0x8006BB50). Slippi Dolphin's
        // JIT implements nmsub as the equivalent fused c - a*b expression.
        // PPC's negate-after-subtract form differs only in the sign of an
        // exact zero. Preserve a same-sign zero sum such as -0 - +0; exact
        // cancellation uses the JIT's round-to-nearest +0.
        if (c == 0.0F && c_sign != product_sign) {
            return c;
        }
        return 0.0F;
    }
    return result;
}

float __fmsubs(float a, float b, float c)
{
#ifdef MSL_CORE_NATIVE
    return __builtin_fmaf(a, b, -c);
#else
    float result;
    __asm__("fmsubs %0,%1,%2,%3" : "=f"(result) : "f"(a), "f"(b), "f"(c));
    return result;
#endif
}

float atan2f(float y, float x)
{
    if (GET_SIGN_BIT(x) == GET_SIGN_BIT(y)) {
        if (GET_SIGN_BIT(x) != 0) {
            return x == -0.0f ? (float) -M_PI_2 : atanf(y / x) - (float) M_PI;
        }

        return x ? atanf(y / x) : (float) M_PI_2;
    }

    if (x < 0.0f) {
        return (float) M_PI + atanf(y / x);
    }

    if (x) {
        return atanf(y / x);
    }

    *(u32*) &y = GET_SIGN_BIT(y) + BITWISE_PI_2;

    return y;
}

#ifdef MSL_CORE_NATIVE
static float msl_acos_seed(float radicand)
{
    u32 bits = BITWISE(radicand);
    u32 exponent = bits >> 23;
    u32 interpolation = (bits & 0x7FFFFF) >> 8;
    // The table selector is four fraction bits plus the inverted exponent bit.
    u32 index = ((bits >> 19) & 31) ^ 16;
    u32 fraction = msl_frsqrte_base[index] -
                   msl_frsqrte_decrement[index] * (interpolation & 2047);
    float scale;
    float significand = (float) (s32) (fraction + 0x4000000U);

    // Round the Gekko 27-bit significand to binary32, then scale by an
    // exact power of two. Positive acos radicands are normal binary32.
    // refs/Ishiiruka/Source/Core/Common/MathUtil.cpp::ApproximateReciprocalSquareRoot
    BITWISE(scale) = ((328 - exponent) >> 1) << 23;
    return significand * scale;
}
#endif

#if MSL_GX_RECORD
/* One Newton step of the reciprocal square root as the retail code has it (GALE01 acosf 0x80022D1C, lbRefract_80022DF8 0x80022DF8):
 * fmuls g*g, fmuls 0.5*g, fnmsubs 3 - x*(g*g), fmuls. A fused multiply-subtract does not depend on the host compiler's contraction
 * (WebAssembly has none), so the viewer build, native or wasm, rounds as the console does. */
static inline float msl_rsqrt_step(float guess, float x)
{
    float g2 = guess * guess;
    float half = 0.5f * guess;
    return half * __fnmsubs(x, g2, 3.0f);
}
#endif

float acosf(float x)
{
#if MSL_GX_RECORD
    float result = __fnmsubs(x, x, 1.0F);   /* fnmsubs 1 - x*x */
#else
    float result = 1.0F - x * x;
#endif
    if (result > 0) {
        float guess;
#ifdef MSL_CORE_NATIVE
        guess = msl_acos_seed(result);
#else
        guess = __frsqrte(result);
#endif
#if MSL_GX_RECORD
        guess = msl_rsqrt_step(guess, result);
        guess = msl_rsqrt_step(guess, result);
        guess = msl_rsqrt_step(guess, result);
#else
        guess = 0.5f * guess * (3.0f - guess * guess * result);
        guess = 0.5f * guess * (3.0f - guess * guess * result);
        guess = 0.5f * guess * (3.0f - guess * guess * result);
#endif
        result = guess;
    } else if (result) {
        result = NAN;
    } else {
        result = INF;
    }
    return (float) M_PI_2 - atanf(x * result);
}

float asinf(float x)
{
#if MSL_GX_RECORD
    return atanf(x * lbRefract_80022DF8(__fnmsubs(x, x, 1.0f)));   /* fnmsubs 1 - x*x (0x80022DD4) */
#else
    return atanf(x * lbRefract_80022DF8(-(x * x - 1.0f)));
#endif
}

static inline float lbRefract_80022DF8(float x)
{
    if (x > 0.0f) {
#if MSL_GX_RECORD
        float guess = msl_acos_seed(x);   /* frsqrte, frsp */
        guess = msl_rsqrt_step(guess, x);
        guess = msl_rsqrt_step(guess, x);
        return msl_rsqrt_step(guess, x);
#elif defined(MSL_CORE_NATIVE)
        return 1.0F / sqrtf(x);
#else
        float guess;
        guess = __frsqrte(x);
        guess = 0.5f * guess * (3.0f - guess * guess * x);
        guess = 0.5f * guess * (3.0f - guess * guess * x);
        guess = 0.5f * guess * (3.0f - guess * guess * x);
        return guess;
#endif
    }

    if (x) {
        return NAN;
    }

    return INF;
}

#define SILVER_RATIO_1_CONJUGATE lbRefract3_804D7DD4

#define BITWISE_THRESHOLD_0 0x3F08D5B9 /* = 0.534511148929596f */
#define BITWISE_THRESHOLD_1 0x3F521801 /* = 0.8206787705421448f */
#define BITWISE_THRESHOLD_2 0x3F9BF7EC /* = 1.218503475189209f */
#define BITWISE_THRESHOLD_3 0x3FEF789E /* = 1.870868444442749f */

const float msl_atanf_lookup[] = {
    1.0,
    -0.3333333134651184,
    0.1999988704919815,
    -0.14281649887561798,
    0.11041180044412613,
    -0.08459755778312683,
    0.04714243486523628,
    6.828420162200928,
    3.239828109741211,
    2.0,
    1.4464620351791382,
    1.1715729236602783,
    1.039566159248352,
    7.1350000325764995e-06,
    8.200000252145401e-07,
    0.0,
    6.299999881775875e-07,
    0.0,
    0.0,
    0.0,
    0.3926900029182434,
    0.5890486240386963,
    0.7853981256484985,
    0.9817469716072083,
    1.1780970096588135,
    1.3744460344314575,
    0.0,
    9.081698408408556e-06,
    2.3000000126671694e-08,
    6.30000016599297e-08,
    7.040000014058023e-07,
    2.499999993688107e-07,
    7.900000014160469e-07,
    2.414212942123413,
    1.4966057538986206,
    1.0,
    0.6681786179542542,
    0.4142135679721832,
    0.1989123672246933,
    5.620000251838064e-07,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
};

float atanf(float x)
{
    float const silver_ratio = 2.4142136573791504f;
    float const silver_ratio_conjugate = 0.4142135679721832f;

    float result;
    const float* lookup_ptr;
    s32 lookup_index = -1;
    bool x_ge_ratio = false;
    s32 sign_bit_x = BITWISE(x) & SIGN_BIT;

    BITWISE(x) &= ~SIGN_BIT;

    if (x >= silver_ratio) {
        x_ge_ratio = true;
        result = 1.0f / x;
    } else if (silver_ratio_conjugate < x) {
        s32 x_bits = SIGNED_BITWISE(x);
        lookup_index = (x_bits >= BITWISE_THRESHOLD_0) +
                       (x_bits >= BITWISE_THRESHOLD_1) +
                       (x_bits >= BITWISE_THRESHOLD_2) +
                       (x_bits >= BITWISE_THRESHOLD_3);
        {
            float offset_39;
            float offset_33;
            lookup_ptr = &msl_atanf_lookup[lookup_index];
            offset_39 = lookup_ptr[39];
            offset_33 = lookup_ptr[33];

            result = 1.0f / (offset_33 + (x + offset_39));
            result = __fnmsubs(result, lookup_ptr[7], offset_33) +
                     __fnmsubs(result, lookup_ptr[13], offset_39);
        }
    } else {
        result = x;
    }

    {
        float result_squared = result * result;
        lookup_ptr = &msl_atanf_lookup[lookup_index];

        // clang-format off
#if MSL_GX_RECORD
        // GALE01 atanf 0x80022FB8..0x80022FEC: Horner with fmadds, then fmadds(result * result_squared, poly, result)
        {
            float cube = result * result_squared;
            float poly = __fmadds(result_squared, msl_atanf_lookup[6], msl_atanf_lookup[5]);
            poly = __fmadds(result_squared, poly, msl_atanf_lookup[4]);
            poly = __fmadds(result_squared, poly, msl_atanf_lookup[3]);
            poly = __fmadds(result_squared, poly, msl_atanf_lookup[2]);
            poly = __fmadds(result_squared, poly, msl_atanf_lookup[1]);
            result = __fmadds(cube, poly, result);
        }
#else
        result = result *
            result_squared * (
                result_squared * (
                    result_squared * (
                        result_squared * (
                            result_squared * (
                                result_squared * (
                                    msl_atanf_lookup[6]
                                ) + msl_atanf_lookup[5]
                            ) + msl_atanf_lookup[4]
                        ) + msl_atanf_lookup[3]
                    ) + msl_atanf_lookup[2]
                ) + msl_atanf_lookup[1]
            ) + result;
        // clang-format on
#endif

        result += lookup_ptr[27];
        result += lookup_ptr[20];
    }

    if (x_ge_ratio) {
        result -= (float) M_PI_2;
        return sign_bit_x ? result : -result;
    }

    BITWISE(result) |= sign_bit_x;
    return result;
}

#ifdef MSL_CORE_NATIVE
static u32 msl_math_float_order_key(float value)
{
    u32 bits;

    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x80000000)) != 0 ? ~bits
                                              : bits ^ UINT32_C(0x80000000);
}

#if MSL_GX_RECORD
extern float msl_gx_len_sq(float x, float y, float z);
#define MSL_DYNAMICS_LEN_SQ(v) msl_gx_len_sq((v)->x, (v)->y, (v)->z)
extern float msl_w4_vec_len(Vec3* v);
// the console's inline sqrtf (frsqrte plus three fused Newton steps), as lbVector_Angle has it
#define MSL_DYNAMICS_LEN(v) msl_w4_vec_len(v)
#else
#define MSL_DYNAMICS_LEN_SQ(v) ((v)->x * (v)->x + (v)->y * (v)->y + (v)->z * (v)->z)
#define MSL_DYNAMICS_LEN(v) sqrtf(MSL_DYNAMICS_LEN_SQ(v))
#endif

#define MSL_DYNAMICS_ANGLE_COSINE(a_arg, b_arg, cosine_out, valid_out)        \
    do {                                                                      \
        Vec3* msl_a = (a_arg);                                                \
        Vec3* msl_b = (b_arg);                                                \
        float msl_lena = MSL_DYNAMICS_LEN(msl_a);                             \
        float msl_lenb = MSL_DYNAMICS_LEN(msl_b);                             \
        float msl_lena_lenb = msl_lena * msl_lenb;                            \
        (valid_out) = msl_lena_lenb > 0.0000000001F;                          \
        if (valid_out) {                                                      \
            (cosine_out) =                                                    \
                __fmadds(msl_a->z, msl_b->z,                                 \
                         __fmadds(msl_a->x, msl_b->x,                        \
                                  msl_a->y * msl_b->y)) /                    \
                msl_lena_lenb;                                               \
            if ((cosine_out) > 1.0F) {                                       \
                (cosine_out) = 1.0F;                                         \
            }                                                                \
            if ((cosine_out) < -1.0F) {                                      \
                (cosine_out) = -1.0F;                                        \
            }                                                                \
        }                                                                     \
    } while (0)

bool msl_dynamics_angle_greater(Vec3* a, Vec3* b, float threshold,
                                u32 cutoff)
{
    float cosine;
    bool valid;

    MSL_DYNAMICS_ANGLE_COSINE(a, b, cosine, valid);
    if (!valid) {
        return 0.0F > threshold;
    }
    return cosine == cosine && msl_math_float_order_key(cosine) < cutoff;
}

bool msl_dynamics_angle_less(Vec3* a, Vec3* b, float threshold,
                             u32 cutoff)
{
    float cosine;
    bool valid;

    MSL_DYNAMICS_ANGLE_COSINE(a, b, cosine, valid);
    if (!valid) {
        return 0.0F < threshold;
    }
    return cosine == cosine && msl_math_float_order_key(cosine) >= cutoff;
}

bool msl_dynamics_angle_greater_value(Vec3* a, Vec3* b, float threshold,
                                      u32 cutoff, float* angle)
{
    float cosine;
    bool valid;

    MSL_DYNAMICS_ANGLE_COSINE(a, b, cosine, valid);
    if (!valid) {
        *angle = 0.0F;
        return 0.0F > threshold;
    }
    if (cosine != cosine || msl_math_float_order_key(cosine) >= cutoff) {
        return false;
    }
    *angle = acosf(cosine);
    return true;
}
#undef MSL_DYNAMICS_ANGLE_COSINE
#endif
