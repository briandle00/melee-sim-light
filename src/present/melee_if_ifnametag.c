// Viewer build only (MSL_GX_RECORD): the decomp's src/melee/if/ifnametag.c, verbatim (decomp/melee/if/ifnametag.c.inc), compiled so the game's own
// drawing code runs. An empty translation unit without the flag.
// refs/melee/src/melee/if/ifnametag.c
// present-include: melee/if
#if MSL_GX_RECORD
#include "gm_names.h"
#include "decomp/melee/if/ifnametag.c.inc"
#endif
#if MSL_GX_RECORD
/* Slippi playback's Show Player Names draws its boxes with the name tag's model (slippi_names.c; 0x804A1ED0) */
HSD_Joint* msl_present_nametag_joint(void)
{
    return un_804A1ED0.joint;
}
#endif
