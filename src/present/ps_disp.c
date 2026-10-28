// Viewer build only (MSL_GX_RECORD): the particle display, the decomp's psdisp.c and psdisptev.c verbatim (decomp/sysdolphin/baselib/*.c.inc, at
// the revision of the particle library beside them, ps_lib.c).
//
// refs/melee/src/sysdolphin/baselib/{psdisp.c,psdisptev.c}
//
// present-flags: -ffp-contract=fast -mfma -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-atan2f
// present-include: sysdolphin/baselib/probe_ps
// present-include: sysdolphin/baselib
#if MSL_GX_RECORD

#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>

#include "probe_ps/psstructs.h"
#include "probe_ps/particle.h"
#include "probe_ps/psappsrt.h"
#include "probe_ps/generator.h"
#include "probe_ps/ps_fp.h"

#include <baselib/gobjgxlink.h>
#include <baselib/gobjproc.h>

/* macros of the decomp revision these files are from (libs/dolphin/include/dolphin/gx/GXGeometry.h, Runtime/platform.h) */
/* the decomp's GXSETARRAY names the table's extent (DCStoreRange's length on the console); the stream needs it too: a table of the game's static data is host memory
 * here, and the GPU reads its bytes (the particle texture coordinates, HSD_PSDisp_8040C340), so the recorder is told the whole table before the array is set */
void msl_gx_known_block(const void* host, size_t size);
#define GXSETARRAY(attr, data, size, stride, le) (msl_gx_known_block((data), (size)), GXSetArray((attr), (data), (stride)))
#define ASSERT_SIZE(expr, size) _Static_assert(sizeof(expr) == (size), "size of " #expr)

/* MWCC inlines sqrtf as frsqrte plus three fused Newton steps (psDispParticles, GALE01 0x803A1298 and seven more); libc rounds correctly */
#include <MSL/math_ppc.h>

#include "decomp/sysdolphin/baselib/psdisp.c.inc"
#include "decomp/sysdolphin/baselib/psdisptev.c.inc"

#endif
