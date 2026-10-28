#ifndef MSL_GX_HW_H
#define MSL_GX_HW_H

/* The viewer build runs the SDK's GX library with no GameCube under it: the write-gather pipe is a buffer
 * (src/gx/msl_gx.h) and the hardware registers it would read or set are stand-ins. This header holds those
 * stand-ins; the SDK's own sources are changed only where they touch the hardware (marked MSL). */

#include <dolphin/types.h>

#include "gx/msl_gx.h"

/* main memory addresses: a host pointer has no console address, the recorder hands out one */
#define MSL_GX_PHYS(p) msl_gx_phys((const void*) (p))

/* the bus clock the retail console runs at (GXInit's timing registers are made from it) */
#define __OSBusClock 162000000u

#define PPCSync() ((void) 0)
#define PPCMfwpar() 0
#define PPCMtwpar(x) ((void) (x))
#define PPCMfhid2() 0
#define PPCMthid2(x) ((void) (x))
#define __sync() ((void) 0)

#include <dolphin/os.h>

#undef OSCachedToPhysical
#undef OSUncachedToPhysical
#define OSCachedToPhysical(p) MSL_GX_PHYS(p)
#define OSUncachedToPhysical(p) MSL_GX_PHYS(p)
#define OSDisableInterrupts() 0
#define OSRestoreInterrupts(x) ((void) (x))

#define __cntlzw(x) ((x) == 0 ? 32 : __builtin_clz((unsigned) (x)))

#endif
