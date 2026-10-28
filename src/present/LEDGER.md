# src/present: the viewer build's presentation code

Everything here compiles only with `MSL_GX_RECORD` (the viewer build, default off); without it every unit is empty and the
production and visual-track builds are unchanged (the flag-off proof of `tools/sim_check_branch.py`: production, and the visual build with the
track off, 92/92 games byte-identical with the base's output).

`decomp/<path>.c.inc` are the decomp's sources, copied by `tools/vendor.py` (`vendor_all.sh` redoes all)
at the pin in `../upstream.lock` (the particle display at the probe's revision 1f0499ba). Each `<dir>_<name>.c` wrapper includes its copy
and names, in `// present-include:`, the simulator directory the copy's own `#include "x.h"` lines resolve in; `include/` holds the decomp
headers the simulator tree lacks and shims for the MWCC-only ones.

## Vendored files and what vendor.py changes in them

| file | what changes | why |
|---|---|---|
| all | `GXWGFifo.<T> = x;` becomes `msl_gx_wg_<T>(x);` | the viewer build's write-gather pipe is a memory buffer (`gx/msl_gx.h`) |
| all | the `0` that ends an `lbArchive_LoadSections` call is `(void*) 0` | the function reads it with `va_arg(void**)`: 4 bytes of an int on a 64-bit host |
| `ifall`, `ifstock`, `iftime`, `ifnametag`, `ifmagnify`, `ifstatus`, `if_2F72`, `if_2FC93`, `ifhazard`, `gm_16AE`, `gm_1601` | `#include "gm_names.h"` first | the simulator also defines some of `gm_16AE`/`gm_1601`'s functions; the copies are renamed |
| `ifstatus` | `memzero` of the table by `sizeof`; the percent token animation takes `IfDamageState` (its real fields) instead of the PPC-offset overlay `UnkX`; `ifStatus_802F6194` walks JObjs by `child`/`next` | PPC-size memzero and struct-overlay punning (the 64-bit layout bug class) |
| `ifmagnify` | the player table addressed by field (`ifMagnify_802FC618`, `_802FC750`), the copies of the first player's image descriptor in an array of their own, `Player_80036978(i, (s32) &v)` becomes `ftLib_80086B90(fighter, &v)`, `ifMagnify_802FC998` not compiled (the simulator defines it) | same bug class; a pointer in an `s32` |
| `if_2F72` | `memzero` by `sizeof` | same |
| `gm_1601` | `// present-first: present/override` | the shim headers must come before the decomp's |

## Files written here

| file | what |
|---|---|
| `game_data.c` | preloads `IfAll.usd` (name "IfAll") and looks up the HUD's public roots before the DAT arena seals |
| `gm_state.c` | the match scene's state (`fn_8016DCC0`), the frame count and timer (`fn_8016CD98`), the HUD start and stop (`ifAll_802F390C`, `ifStatus_802F6EA4` READY/GO, `ifTime_CreateTimers`, `ifStatus_802F665C`); rules from `MSL_HUD_RULES` (default stock mode, 8:00) |
| `dol_data.c` | the retail `.data`/`.rodata` the decomp leaves as placeholders or externs, read with `tools/dol.py`: `ifMagnify_803F97E8` (its camera descriptor), the magnifier's constants, `ifStatus_HudInfo` |
| `hud.c` | no-ops for the coin-get notice (`ifcoget.c`); the name-tag table (`GetNameText`, restored from the replay as Slippi playback does) and the font glyph block the GX layer names |
| `audio_noop.c` | `lbAudioAx_*` and `HSD_PadRumble*` return 0 (no sound, no pad) |
| `ps_disp.c` | `psdisp.c` and `psdisptev.c` (on the particle probe's library), `efLib_render_callback`, the two objects of `efLib_Init` with their processes at link 0xF (`msl_present_ef_proc_main` runs the probe's update from the random state of that point of the frame) |
| `support.c` | functions `tools/mk_support.py` generates for the vendored files |
| `override/`, `include/` | shim headers |

## Edits in existing simulator files (all under `MSL_GX_RECORD`, or `!defined(MSL_CORE_HOSTED) || MSL_STAGE_PRESENTATION`)

| file | what |
|---|---|
| `platform/compat.h` | `MSL_STAGE_PRESENTATION` (1 with the viewer flag): marks the places where the hosted build leaves a stage's presentation out |
| `melee/gr/grizumi.c` | Fountain of Dreams runs the game's reflection graph, mirror object, stars and their generators (`grIzumi_801CBE64`, `_OnLoad`); the player table `IzumiUnkCC` overlay becomes the field `x18` |
| `runtime/scalar.c` | the hooks: efLib objects, HUD start/stop and per-frame state, game data; `configure_headless_ground_schedule` returns at once (the stage keeps its animations and post processes); replay players' name tag id is 120 (`Player_SetNametagSlotID`) |
| `Runtime/camera.c` | `msl_camera_publish_match_visibility` leaves `x221F_b0` to the fighters' draw (retail: the magnifier reads what the previous draw left) |
| `Runtime/viewer_track.c` | the particle probe's update is not run from the track's frame (it is a process of the efLib objects); `msl_particle_probe_frame_written` not called |
| `tools/replay/replay_drive.c` | the viewer track runs to the null device by default (the effect hooks are gated by `msl_viewer_track_on`) |
| `sysdolphin/baselib/ps_probe.c` | the probe defaults to the null device; the bank's texture groups are the bank's own |
| `platform/native_dat.c`, `.h` | texture translation for particle banks (all offsets relative to the bank); `HUD` and scene public roots; `_image_desc` public roots (`HSD_ImageDesc`); the particle scripts' bytes kept always |
| `stubs/*` | weak abort stubs; `grDisplay_801C5DB0`, `lbRefract_*` not defined (the decomp's are compiled) |
| `tools/build/*` | `-I src/present/include`, `// present-include:` hints, `INCREMENTAL=1`, enumerated sources, the viewer's PPC layout |

## Not restored yet

lbBgFlash display, lbRefract init (`LbRf.dat`, `lbl_803BB0B0`), Pokemon Stadium's jumbotron and transformations (`grpstadium.c` hosted regions), the
Slippi plates and the text engine (names on tags, coin-get), effect models as real GObjs (the particle library is the probe's copy).
