/* MSL_GX_RECORD: the decomp's Dolphin SDK source (extern/dolphin/src/dolphin/gx/GXDisplayList.c at the pinned decomp commit), compiled only in the viewer build. */
#if MSL_GX_RECORD
#include "__gx.h"

#include <macros.h>
#include <string.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>

/* MSL: GXBeginDisplayList / GXEndDisplayList (a display list made into a buffer through the fifo hardware) are not
 * here: the game's display lists come from its files, called with GXCallDisplayList. */

void GXCallDisplayList(void* list, u32 nbytes)
{
    CHECK_GXBEGIN(0xEC, "GXCallDisplayList");
    ASSERTMSGLINE(0xED, !gx->inDispList,
                  "GXCallDisplayList: display list already in progress");
    ASSERTMSGLINE(0xEE, (nbytes & 0x1F) == 0,
                  "GXCallDisplayList: nbytes is not 32 byte aligned");
    ASSERTMSGLINE(0xEF, ((u32) list & 0x1F) == 0,
                  "GXCallDisplayList: list is not 32 byte aligned");

    if (gx->dirtyState != 0) {
        __GXSetDirtyState();
    }
#if DEBUG
    __GXShadowDispList(list, nbytes);
#endif
    if (*(u32*) &gx->unk == 0) { // checks both unk and bpSent
        __GXSendFlushPrim();
    }
    /* MSL: a recording holds the list's commands, not the call (the console's address of the list and its length) */
    msl_gx_put_bytes(list, nbytes);
}
#endif /* MSL_GX_RECORD */
