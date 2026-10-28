/* Viewer build: the match scene's functions the simulator also defines (src/Runtime/match.c, an adaptation for gameplay: constants for
 * the supported rules, the standings recomputed from live Player data). The HUD asks the game's own (decomp/melee/gm/gm_16AE.c and
 * gm_1601.c, reading lbl_8046B6A0, the scene's state, which src/present/gm_state.c keeps); both copies exist, the HUD's under these
 * names, so gameplay keeps the simulator's. Included before the vendored gm files and the HUD files. */
#ifndef MSL_PRESENT_GM_NAMES_H
#define MSL_PRESENT_GM_NAMES_H
#define gm_80167320 msl_pgm_gm_80167320
#define gm_80167470 msl_pgm_gm_80167470
#define gm_801693BC msl_pgm_gm_801693BC
#define gm_8016AEDC msl_pgm_gm_8016AEDC
#define gm_8016AEEC msl_pgm_gm_8016AEEC
#define gm_8016B014 msl_pgm_gm_8016B014
#define gm_8016B094 msl_pgm_gm_8016B094
#define gm_8016B0B4 msl_pgm_gm_8016B0B4
#define gm_8016B0D4 msl_pgm_gm_8016B0D4
#define gm_8016B0E8 msl_pgm_gm_8016B0E8
#define gm_8016B0FC msl_pgm_gm_8016B0FC
#define gm_8016B110 msl_pgm_gm_8016B110
#define gm_8016B14C msl_pgm_gm_8016B14C
#define gm_8016B168 msl_pgm_gm_8016B168
#define gm_8016B1C4 msl_pgm_gm_8016B1C4
#define gm_8016B1D8 msl_pgm_gm_8016B1D8
#define gm_8016B204 msl_pgm_gm_8016B204
#define gm_8016B248 msl_pgm_gm_8016B248
#define gm_8016B3D8 msl_pgm_gm_8016B3D8
#define gm_8016B41C msl_pgm_gm_8016B41C
#define gm_8016B6E8 msl_pgm_gm_8016B6E8
#define gm_8016C5C0 msl_pgm_gm_8016C5C0
#define gm_8016C6C0 msl_pgm_gm_8016C6C0
#define gm_8016C75C msl_pgm_gm_8016C75C
#endif
