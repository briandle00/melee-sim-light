/* Viewer build only (MSL_GX_RECORD): the game's sound and controller-rumble calls that the HUD, effects and stage code make while drawing.
 * The viewer plays no sound and has no pad; every call returns 0 at once. (The headless simulator reaches the same calls through its
 * own audio log; those are the Runtime/audio.c ones, not these.) Each is defined with the signature the decomp's header gives it
 * (tools/build/gen_audio_noop.py): a mismatch with the callers' signature traps in WebAssembly.
 * refs/melee/src/melee/lb/lbaudio_ax.c, refs/melee/src/sysdolphin/baselib/padrumble.c */

#if MSL_GX_RECORD

#include <melee/lb/lbaudio_ax.h>
#include <baselib/rumble.h>

void HSD_PadRumbleOn(u8 a0) { (void) a0; }
void HSD_PadRumblePauseAll(void) { }
void HSD_PadRumbleRemove(u8 a0) { (void) a0; }
void HSD_PadRumbleRemoveAll(void) { }
void HSD_PadRumbleUnpauseAll(void) { }
int lbAudioAx_8002305C(int a0, int a1) { (void) a0; (void) a1; return 0; }
int lbAudioAx_80023694(void) { return 0; }
int lbAudioAx_800236DC(void) { return 0; }
bool lbAudioAx_80023710(int a0) { (void) a0; return 0; }
void lbAudioAx_80024030(s32 a0) { (void) a0; }
s32 lbAudioAx_800243F4(int a0) { (void) a0; return 0; }
int lbAudioAx_800245F4(int a0) { (void) a0; return 0; }
void lbAudioAx_80024614(int a0) { (void) a0; }
void lbAudioAx_80024B1C(int a0, int a1) { (void) a0; (void) a1; }
void lbAudioAx_80024B58(int a0, int a1) { (void) a0; (void) a1; }
void lbAudioAx_80024C84(void) { }
void lbAudioAx_80024D50(void) { }
void lbAudioAx_80024E84(bool a0) { (void) a0; }
void lbAudioAx_80026F2C(s32 a0) { (void) a0; }
void lbAudioAx_8002702C(s32 a0, u64 a1) { (void) a0; (void) a1; }
void lbAudioAx_80027168(void) { }
void lbAudioAx_80027648(void) { }
s32 lbAudioAx_8002785C(void) { return 0; }

#endif
