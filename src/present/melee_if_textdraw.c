// Viewer build only (MSL_GX_RECORD): the decomp's src/melee/if/textdraw.c, verbatim (decomp/melee/if/textdraw.c.inc), compiled so the game's own
// drawing code runs. An empty translation unit without the flag.
// refs/melee/src/melee/if/textdraw.c
// the debug text's camera and draw callback (DevText_Setup is called by msl_present_hud_start as gm_801A4BD4's un_802FF78C does at boot)
// changed: #include "textdraw.h" ...
// present-include: melee/if
#if MSL_GX_RECORD
#include "gm_names.h"
#include "decomp/melee/if/textdraw.c.inc"
#endif
