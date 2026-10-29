/* The browser build's per-frame API (WebAssembly, MSL_GX_RECORD): open a tape, step one frame, read that frame's GX stream and the
 * memory it references. Built by tools/build/build_wasm_gx.sh as the module gx-core; the replay driver's own source is included so the
 * tape format and the way a match is driven are the one the native driver (and the comparison against it) uses.
 *
 *   msl_gxw_open(tape, size, data_root)   0 on success; the game data is read from data_root (the module's file system)
 *   msl_gxw_step()                        the next tape frame: 0 stepped, 1 end of tape, <0 error. Steps the match, then runs the
 *                                         display pass into the GX layer
 *   msl_gxw_step_frame(frame)             the same with a packed TapeFrame (msl_gxw_tape_frame_size() bytes) of the caller's own
 *   msl_gxw_stream_ptr / _len             the frame's FIFO bytes (valid until the next step)
 *   msl_gxw_region_count / _addr / _ptr / _size / _type / _pos(i)
 *                                         the memory blocks the frame references that the previous frames did not: console
 *                                         address, where the bytes are in the module's memory, size, update type, and the stream
 *                                         offset they must be in place by (0: at the frame's start; more: the game rewrote the block mid-frame)
 *   msl_gxw_initial_state(which)          the GPU register files at the first frame (0 BP 256 words, 1 CP 256, 2 XF 0x1000+0x60)
 *   msl_gxw_frame_index()                 tape frames stepped so far
 *   msl_gxw_set_playback(name)            "slippi" (default) or "original": which game to follow. 0 set, -1 unknown name, -2 too late
 *                                         (a frame was stepped already). Call it before the first step (before or after open).
 *                                         slippi: Slippi Dolphin's playback hook stores at game frame -123 (src/runtime/scalar.c);
 *                                         original: the recording. msl_gxw_playback() returns the current name.
 *   msl_gxw_set_hud_layout(name)          "game" (default: each player's HUD at their port's slot) or "outer" (a two-player game's lower
 *                                         port in HUD slot 1, the higher in slot 4). 0 set, -1 unknown name, -2 too late. Call it BEFORE msl_gxw_open (the
 *                                         HUD binds its slots when the match opens; playback has no such limit).
 *                                         msl_gxw_hud_layout() returns the current name. Binds the displays to the slots in the
 *                                         game's own HUD code (src/present/gm_state.c); nothing is moved after drawing.
 */
#define main replay_drive_main
#include "replay_drive.c"
#undef main

#include <emscripten.h>
#include <unistd.h>

static TapeConfig api_tape;
static MslCoreGameData api_game_data;
static MslCoreMatch api_match;
static const uint8_t* api_frames;
static uint32_t api_frame_count;
static uint32_t api_index;
static int api_open;
/* msl_gxw_set_display(0): the steps run no display pass and record no GX stream (only the gameplay rows matter: the C-stick fit's drives), as the
   native driver steps when no recording is asked for (gx_display.c msl_gx_frame returns first without MSL_GX_RECORD_OUT) */
static int api_display = 1;
/* msl_gxw_restart_stream(): the next display pass starts the stream again, as the native recorder starts at MSL_GX_RECORD_FRAMES' first frame
   (msl_gx_recording_start: every register file as the game has set them so far is msl_gxw_initial_state, and every memory block a frame reads
   is sent again); the page draws from there on a renderer started fresh: after a fast seek, or after putting back a saved state */
static int api_restart;
static uint8_t* api_rec;           /* msl_gxw_set_recorded */
static int api_rec_row;
static void api_resync_frame(uint32_t index);

EMSCRIPTEN_KEEPALIVE int msl_gxw_open(const uint8_t* tape, size_t size, const char* data_root)
{
    MslCoreMatchConfig config;
    MslCoreInput previous;
    TapeFrame first;

    if (size < sizeof(api_tape) + sizeof(first)) {
        return -1;
    }
    memcpy(&api_tape, tape, sizeof(api_tape));
    if (api_tape.magic != 0x314C534DU) {
        return -2;
    }
    config_from_tape(&config, &api_tape);
#if MSL_GX_RECORD
    /* the name tags, display names and starting timer after the tape's last frame, read the native driver's way (replay_drive.c
       restore_nametags reads them from the tape file): a tape handed over whole is written to the module's file system for it */
    {
        FILE* f = fopen("/tmp/msl_gxw.tape", "wb");
        if (f != NULL) {
            fwrite(tape, 1, size, f);
            fclose(f);
        }
        restore_nametags("/tmp/msl_gxw.tape", &api_tape);
        remove("/tmp/msl_gxw.tape");
    }
#endif
    api_frames = tape + sizeof(api_tape);
    api_frame_count = (uint32_t) ((size - sizeof(api_tape)) / sizeof(TapeFrame));
    if (api_frame_count > api_tape.frame_count) {
        api_frame_count = api_tape.frame_count;
    }
    memcpy(&first, api_frames, sizeof(first));
    fill_input(&previous, &first, api_tape.num_players);
    if (msl_core_game_data_init(&api_game_data, data_root) != 0) {
        return -3;
    }
#if MSL_GX_RECORD
    /* The viewer build draws the particles and effect models the simulator's recorder hooks feed (msl_viewer_track_on gates them), so the
       hooks run always. Nothing is written: with no output file every writer returns first (and the track's text would only be formatted
       to be thrown away). */
    msl_viewer_track_begin(NULL);
    msl_viewer_track_on = 1;
#endif
    if (msl_core_match_storage_init(&api_match) != 0 || msl_core_match_reset(&api_match, &api_game_data, &config, &previous) != 0) {
        return -4;
    }
    api_index = 0;
    api_open = 1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int msl_gxw_step_frame(const uint8_t* packed)
{
    TapeFrame frame;
    MslCoreInput input;
    MslCoreStageEvents events;

    if (!api_open) {
        return -1;
    }
    memcpy(&frame, packed, sizeof(frame));
    fill_input(&input, &frame, api_tape.num_players);
    fill_events(&events, &frame);
    if (msl_core_match_step(&api_match, &input, frame.frame_seed, &events) != 0) {
        return -2;
    }
    api_rec_row = 0;
    if (api_rec != NULL) {
        api_resync_frame(api_index);
    }
    if (api_display) {
        msl_gx_stream_frame(&api_match, api_index == 0 || api_restart);
        api_restart = 0;
    }
    api_index++;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int msl_gxw_step(void)
{
    if (!api_open) {
        return -1;
    }
    if (api_index >= api_frame_count) {
        return 1;
    }
    return msl_gxw_step_frame(api_frames + (size_t) api_index * sizeof(TapeFrame));
}

EMSCRIPTEN_KEEPALIVE int msl_gxw_set_playback(const char* name)
{
    if (api_index > 0) {
        return -2;
    }
    return msl_playback_set_name(name);
}
EMSCRIPTEN_KEEPALIVE const char* msl_gxw_playback(void) { return msl_playback_get_name(); }

EMSCRIPTEN_KEEPALIVE int msl_gxw_set_hud_layout(const char* name)
{
    /* the HUD binds its slots when the match opens (msl_present_hud_start), so unlike the playback option this is too late once opened */
    if (api_open || api_index > 0) {
        return -2;
    }
    return msl_hud_layout_set_name(name);
}
EMSCRIPTEN_KEEPALIVE const char* msl_gxw_hud_layout(void) { return msl_hud_layout_get_name(); }

/* The C-stick fit of a recording without raw C-stick bytes (Slippi before 3.17), for playback "original" (tools/replay/cstick_fit.c; the python's
 * drive_fitted decisions). The page builds the tape with the raw bytes msl_gxw_cfit_raw gives, drives it, and feeds back the first frame that
 * differs from the recording:
 *   msl_gxw_cfit_new(lanes, count)           a planner for `lanes` players (0..7 index) and `count` tape frames; frees the previous one
 *   msl_gxw_cfit_lane(lane, cx, cy)          the lane's processed C-stick, count floats each (pointers into the module's memory)
 *   msl_gxw_cfit_raw(lane, axis)             pointer to count int16: the raw byte of axis 0 (x) / 1 (y) to put in each TapeFrame (c_x, c_y)
 *   msl_gxw_cfit_next(first, near_only)      first: first differing frame of the drive just made (-1 none); near_only (first call only): replay_drive.py's
 *                                            positions_near_only for that frame (1/0). Returns 1: raw bytes changed, drive plain again; 2: drive
 *                                            resynced (msl_gxw_set_recorded) - on the first call with the current bytes, then call again with that
 *                                            drive's first difference; after it, for every trial; 0: done (msl_gxw_cfit_raw is the fitted choice;
 *                                            if msl_gxw_cfit_mode() is 1, drive plain once more with it and judge that drive)
 *   msl_gxw_cfit_result(moved)               after a trial drive: its first differing frame (-1 none)
 *   msl_gxw_cfit_first() / _flips()          the current first difference / toggles kept
 * Playback "slippi" needs none of this: set the tape header's slippi_patches bit 7 (MSL_PATCH_RAW_CSTICK_ABSENT) and the core plays the recorded
 * C-stick as Slippi Dolphin does, whatever raw bytes the tape carries. */
#include "cstick_fit.c"
static Cfit* api_cfit;
EMSCRIPTEN_KEEPALIVE int msl_gxw_cfit_new(int lanes, int count)
{
    if (api_cfit != NULL) {
        cfit_free(api_cfit);
    }
    api_cfit = cfit_new(lanes, count);
    return api_cfit != NULL ? 0 : -1;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_cfit_lane(int lane, const float* cx, const float* cy)
{
    if (api_cfit == NULL || lane < 0 || lane >= CFIT_MAX_LANES) {
        return -1;
    }
    cfit_lane(api_cfit, lane, cx, cy);
    return api_cfit->nseg;
}
EMSCRIPTEN_KEEPALIVE const int16_t* msl_gxw_cfit_raw(int lane, int axis)
{
    return api_cfit != NULL && lane >= 0 && lane < CFIT_MAX_LANES && axis >= 0 && axis < 2 ? api_cfit->raw[lane][axis] : NULL;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_cfit_next(int first, int near_only) { return api_cfit != NULL ? cfit_next(api_cfit, first, near_only) : 0; }
EMSCRIPTEN_KEEPALIVE int msl_gxw_cfit_mode(void) { return api_cfit != NULL ? cfit_resync_mode(api_cfit) : 0; }
EMSCRIPTEN_KEEPALIVE void msl_gxw_cfit_result(int moved)
{
    if (api_cfit != NULL) {
        cfit_result(api_cfit, moved);
    }
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_cfit_first(void) { return api_cfit != NULL ? cfit_first(api_cfit) : -1; }
EMSCRIPTEN_KEEPALIVE int msl_gxw_cfit_flips(void) { return api_cfit != NULL ? cfit_flips(api_cfit) : 0; }

/* msl_gxw_set_recorded(buf, size): the recording's own positions, frame by frame, as replay_drive.py's recorded_positions writes them for
 * MSL_RECORDED (per tape frame: uint16 item count, 16 float32 = 4 ports' fighter x, y then 4 followers' x, y (NaN for none), then each item's
 * x, y, vel x, vel y). With it set, each step puts back a position (or an item's velocity) that is within RESYNC_MOST of the recording's but not
 * equal (replay_drive.c resync_one, the native driver's drive_resynced) before the frame is drawn, and msl_gxw_out_row is the resynced row.
 * Before the first step; (NULL, 0) clears it; the bytes are copied. 0 set, -2 too late. */
static size_t api_rec_size, api_rec_pos;
static OutFrame api_out_row;
EMSCRIPTEN_KEEPALIVE void msl_gxw_restart_stream(void) { api_restart = 1; }
/* msl_gxw_heap_top(): the end of the memory in use (sbrk(0)). Everything the module holds between calls is in memory[0, top) (the stack
   pointer is back at its base), so the page saves the state as those bytes and rewinds by writing them back, then restarts the stream */
EMSCRIPTEN_KEEPALIVE uintptr_t msl_gxw_heap_top(void) { return (uintptr_t) sbrk(0); }
EMSCRIPTEN_KEEPALIVE int msl_gxw_set_display(int on)
{
    if (api_index > 0) {
        return -2;
    }
    api_display = on != 0;
    return 0;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_set_recorded(const uint8_t* buf, size_t size)
{
    if (api_index > 0) {
        return -2;
    }
    free(api_rec);
    api_rec = NULL;
    api_rec_size = api_rec_pos = 0;
    if (buf != NULL && size > 0) {
        api_rec = malloc(size);
        if (api_rec == NULL) {
            return -1;
        }
        memcpy(api_rec, buf, size);
        api_rec_size = size;
    }
    return 0;
}
static void api_resync_frame(uint32_t index)
{
    uint16_t items;
    float fighters[16];
    float item_pos[MSL_CORE_MAX_ITEMS * 4];
    size_t n_items, got;
    int i;
    fill_output(&api_out_row, msl_core_match_output(&api_match));
    api_rec_row = 1;
    if (api_rec == NULL) {
        return;
    }
    /* (resync_frame's reads, from memory; a short record ends the resync as the file's short read does) */
    if (api_rec_pos + sizeof(items) + sizeof(fighters) > api_rec_size) {
        free(api_rec);
        api_rec = NULL;
        return;
    }
    memcpy(&items, api_rec + api_rec_pos, sizeof(items));
    api_rec_pos += sizeof(items);
    memcpy(fighters, api_rec + api_rec_pos, sizeof(fighters));
    api_rec_pos += sizeof(fighters);
    n_items = items < MSL_CORE_MAX_ITEMS ? items : MSL_CORE_MAX_ITEMS;
    got = 0;
    if (items > 0) {
        got = (api_rec_size - api_rec_pos) / (sizeof(float) * 4);
        if (got > n_items) {
            got = n_items;
        }
        if (got == 0) {
            free(api_rec);
            api_rec = NULL;
            return;
        }
        memcpy(item_pos, api_rec + api_rec_pos, got * sizeof(float) * 4);
        api_rec_pos += got * sizeof(float) * 4;
    }
    for (i = 0; i < 4; ++i) {
        resync_one(&api_match, index, 'f', i, 0, &api_out_row.pos_x[i], fighters[i * 2]);
        resync_one(&api_match, index, 'f', i, 1, &api_out_row.pos_y[i], fighters[i * 2 + 1]);
        resync_one(&api_match, index, 'n', i, 0, &api_out_row.follower_pos_x[i], fighters[8 + i * 2]);
        resync_one(&api_match, index, 'n', i, 1, &api_out_row.follower_pos_y[i], fighters[8 + i * 2 + 1]);
    }
    if (items != api_out_row.item_count) {
        return;
    }
    for (i = 0; i < (int) items && i < MSL_CORE_MAX_ITEMS; ++i) {
        resync_one(&api_match, index, 'i', i, 0, &api_out_row.items[i].pos_x, item_pos[i * 4]);
        resync_one(&api_match, index, 'i', i, 1, &api_out_row.items[i].pos_y, item_pos[i * 4 + 1]);
        resync_one(&api_match, index, 'i', i, 2, &api_out_row.items[i].vel_x, item_pos[i * 4 + 2]);
        resync_one(&api_match, index, 'i', i, 3, &api_out_row.items[i].vel_y, item_pos[i * 4 + 3]);
    }
}

EMSCRIPTEN_KEEPALIVE int msl_gxw_tape_frame_size(void) { return (int) sizeof(TapeFrame); }
EMSCRIPTEN_KEEPALIVE int msl_gxw_frame_index(void) { return (int) api_index; }
EMSCRIPTEN_KEEPALIVE int msl_gxw_frame_count(void) { return (int) api_frame_count; }

EMSCRIPTEN_KEEPALIVE const uint8_t* msl_gxw_stream_ptr(void)
{
    const uint8_t* d; size_t n; int u;
    return msl_gx_kept(&d, &n, &u) ? d : NULL;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_stream_len(void)
{
    const uint8_t* d; size_t n; int u;
    return msl_gx_kept(&d, &n, &u) ? (int) n : 0;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_region_count(void)
{
    const uint8_t* d; size_t n; int u;
    return msl_gx_kept(&d, &n, &u) ? u : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t msl_gxw_region_addr(int i)
{
    uint32_t a = 0, s = 0, p_; const uint8_t* h; int t;
    msl_gx_kept_update(i, &a, &h, &s, &t, &p_);
    return a;
}
EMSCRIPTEN_KEEPALIVE const uint8_t* msl_gxw_region_ptr(int i)
{
    uint32_t a, s, p_; const uint8_t* h = NULL; int t;
    msl_gx_kept_update(i, &a, &h, &s, &t, &p_);
    return h;
}
EMSCRIPTEN_KEEPALIVE uint32_t msl_gxw_region_size(int i)
{
    uint32_t a, s = 0, p_; const uint8_t* h; int t;
    msl_gx_kept_update(i, &a, &h, &s, &t, &p_);
    return s;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_region_type(int i)
{
    uint32_t a, s, p_; const uint8_t* h; int t = 0;
    msl_gx_kept_update(i, &a, &h, &s, &t, &p_);
    return t;
}
EMSCRIPTEN_KEEPALIVE uint32_t msl_gxw_region_pos(int i)
{
    uint32_t a, s, p_ = 0; const uint8_t* h; int t;
    msl_gx_kept_update(i, &a, &h, &s, &t, &p_);
    return p_;
}
EMSCRIPTEN_KEEPALIVE const uint32_t* msl_gxw_initial_state(int which) { return msl_gx_initial_state(which); }

/* The last step's output row (MslCoreCompare, wire.h) for a consumer that holds the match against the replay it plays (the viewer's
 * divergence check, src/sim/compare.ts). Valid until the next step. */
EMSCRIPTEN_KEEPALIVE const uint8_t* msl_gxw_output_ptr(void) { return api_open ? (const uint8_t*) msl_core_match_output(&api_match) : NULL; }
EMSCRIPTEN_KEEPALIVE int msl_gxw_output_size(void) { return (int) sizeof(MslCoreCompare); }
/* the step's row as the native driver writes it (replay_drive.c fill_output: OutFrame, items packed, misc_mask from item_projection.h), so the
   page's comparison with the recording is replay_drive.py frame_diff's on the same bytes (the C-stick fit's first difference) */
EMSCRIPTEN_KEEPALIVE const uint8_t* msl_gxw_out_row(void)
{
    if (!api_open) {
        return NULL;
    }
    if (!api_rec_row) {
        fill_output(&api_out_row, msl_core_match_output(&api_match));
    }
    return (const uint8_t*) &api_out_row;
}
EMSCRIPTEN_KEEPALIVE int msl_gxw_out_row_size(void) { return (int) sizeof(OutFrame); }

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
/* (a debugging build: keep going after a report) */
const char* __asan_default_options(void) { return "halt_on_error=0:detect_leaks=0"; }
#endif
#endif
