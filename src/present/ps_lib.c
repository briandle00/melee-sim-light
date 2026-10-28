// Viewer build only (MSL_GX_RECORD): the particle library, the decomp's particle.c, generator.c and psappsrt.c (the particle probe's copies,
// at decomp revision 1f0499ba, in sysdolphin/baselib/probe_ps), compiled under the game's own names. The probe (ps_probe.c) compiled the same
// texts under pp_ names with its own heaps and joints; the viewer build runs the library as the game does.
//
// refs/melee/src/sysdolphin/baselib/{particle.c,generator.c,psappsrt.c}
//
// present-flags: -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-atan2f
// present-include: sysdolphin/baselib/probe_ps
// present-include: sysdolphin/baselib
#if MSL_GX_RECORD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/object.h>
#include <sysdolphin/baselib/random.h>

#include "probe_ps/psstructs.h"
#include "probe_ps/particle.h"
#include "probe_ps/psappsrt.h"
#include "probe_ps/generator.h"

#include "probe_ps/ps_fp.h"

/* MSL_EF_TRACE: the generator makings the library refuses (hsd_8039F05C: why, link, bank, id) */
#define PS_MISS_TRACE(why, link, bank, id)     do { if (getenv("MSL_EF_TRACE")) fprintf(stderr, "PSMISS %d link %d bank %d id %d (count %d)%c", (why), (int) (link), (int) (bank), (int) (id), (int) ((bank) < 0x41 ? psCmdListArray[bank] : -1), 10); } while (0)

/* the library's bank loaders relocate a 32-bit file image in place: not for this host (effects_glue.c has the loaders, from the simulator's
 * translated banks); the library's own are kept under other names */
#define psInitDataBank ps_unused_psInitDataBank
#define psInitDataBankLoad ps_unused_psInitDataBankLoad
#define psInitDataBankLocate ps_unused_psInitDataBankLocate
#include "probe_ps/psappsrt.c.inc"
#include "probe_ps/particle.c.inc"
#include "probe_ps/generator.c.inc"
#undef psInitDataBank
#undef psInitDataBankLoad
#undef psInitDataBankLocate

#endif
