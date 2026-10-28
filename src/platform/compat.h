#ifndef MSL_CORE_PLATFORM_COMPAT_H
#define MSL_CORE_PLATFORM_COMPAT_H

// The decomp's MWCC precompiled header provides this transitively. GCC does not.
#include <stdint.h>

#define MSL_CORE_HOSTED 1

// The hosted build drops what only the renderer reads: MSL_HEADLESS marks those places. The viewer build (MSL_GX_RECORD)
// draws, and keeps them.
#if !MSL_GX_RECORD
#define MSL_HEADLESS 1
#endif
// The native build animates the fighters with a compact pose engine (runtime/fighter_pose.c) and builds their models
// from the gameplay joints; MSL_POSE_ENGINE marks the code of the engine. The viewer build keeps the engine (its arithmetic is the
// game's, bit for bit) and runs every joint of every fighter through it (MSL_FULL_FIGHTER_MODELS).
#if defined(MSL_CORE_NATIVE)
#define MSL_POSE_ENGINE 1
#endif
#if MSL_GX_RECORD
#define MSL_FULL_FIGHTER_MODELS 1
#else
#define MSL_FULL_FIGHTER_MODELS 0
#endif
// The viewer build also runs what the hosted build leaves out of the stages for being presentation only (reflection graphs, screens,
// stars, models and materials of stage objects): the game's own code, where the hosted build has a gameplay-equivalent shortcut
// beside it. MSL_STAGE_PRESENTATION marks the places (`#if !defined(MSL_CORE_HOSTED) || MSL_STAGE_PRESENTATION`).
#if MSL_GX_RECORD
#define MSL_STAGE_PRESENTATION 1
#else
#define MSL_STAGE_PRESENTATION 0
#endif

// The decomp's MSL stddef supplies this spelling transitively. Hosted libc
// does not, while several exact translation units use it directly.
typedef unsigned int usize_t;

// baselib/debug.h names these MSL callback types, but the hosted build uses
// libc's stdio rather than placing the GameCube MSL headers ahead of libc.
typedef unsigned long __file_handle;
typedef void (*__idle_proc)(void);

float msl_dolphin_fnmsubs(float a, float b, float c);
#ifdef MSL_NATIVE_INLINE_FMADDS
#define __fmadds(a, b, c) __builtin_fmaf((a), (b), (c))
#else
float __fmadds(float a, float b, float c);
#endif
float __fmsubs(float a, float b, float c);
float __fnmsubs(float a, float b, float c);

// A double-precision PPC fmadd (and, with negated operands, fmsub/fnmadd/
// fnmsub) as the capturing JIT computed it: fused on a host with FMA3, a
// rounded product and then a rounded add without it (MSL_JIT_NO_HOST_FMA).
// For single-precision operands the product is exact and both agree.
// The flag is the bound match's, cached when the match is bound
// (context.c::msl_core_bind_match), so the default path is one predictable
// branch.
extern _Thread_local int msl_jit_no_host_fma;
static inline double msl_ppc_fma(double a, double b, double c)
{
    if (msl_jit_no_host_fma) {
        volatile double product = a * b;
        return product + c;
    }
    return __builtin_fma(a, b, c);
}

#endif
