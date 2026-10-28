/* The C-stick fit of tools/replay/replay_drive.py (drive_fitted, reconstructed_cstick, cstick_gate_segments), as a resumable planner for a caller that
 * runs the drives itself (the browser page: whole-tape drives in the wasm core, its own per-frame comparison against the recording).
 *
 * A recording before Slippi 3.17 has no raw C-stick bytes. UCF 1.0's cardinal snap reads them (raw 80 on one axis with the other within 6 snaps to
 * exactly 1.0), and the processed value cannot say whether it applied: +-0.9875 with the other axis 0 is a raw 79 (no snap) or a raw 80 past the gate.
 * Every other value is unambiguous (trunc(value * 80)). The fit starts every ambiguous run unsnapped (79) and, at each first difference from the
 * recording, tries the held runs that reach it (the whole run toggled, then its frames next to the difference one by one), keeping a toggle that
 * moves the first difference later.
 *
 * Use:  f = cfit_new(lanes, count); cfit_lane(f, lane, cx, cy) for each lane lacking raw bytes (the processed C-stick floats per frame);
 *   cfit_raw(f, lane, 0 | 1) are the raw x / y byte arrays (int16, count entries) to write into the tape's C-stick fields;
 *   drive the tape; first = the first frame that differs from the recording (-1 none);
 *   loop { n = cfit_next(f, first, near_only); if (!n) break; drive the tape again (n == 1 plain, n == 2 resynced: replay_drive.drive_resynced);
 *          cfit_result(f, first difference of that drive, -1 none); first = cfit_first(f); }
 * near_only matters on the first call only: positions_near_only(replay, plain blob, first) (replay_drive.py), true when the plain drive's first
 * difference is only a position or an item velocity within RESYNC_NEAR of the recording. Then the fit runs on resynced drives (the resync drive):
 * cfit_next returns 2 for the first call (drive resynced with the current bytes, then call cfit_next(first of that drive, 0) to go on), and 2 for
 * every trial. When it returns 0 and cfit_resync_mode() is 1, the caller drives plain once more with the settled bytes and judges that drive.
 * cfit_next returns 1 when raw bytes changed and the tape must be driven again plain, 2 resynced, 0 when the fit is done (also when first is -1).
 * The decisions are the python's exactly (tries 4, rounds 64, reach 2). */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CFIT_REACH 2
#define CFIT_TRIES 4
#define CFIT_ROUNDS 64
#define CFIT_MAX_SPANS 8
#define CFIT_MAX_LANES 8

typedef struct { int first, last, axis, lane; } CfitSeg;
typedef struct { int lane, axis, lo, hi, at; } CfitKey;

typedef struct Cfit {
    int lanes, count;
    int16_t* raw[CFIT_MAX_LANES][2];     /* per lane: x, y */
    CfitSeg* seg;
    int nseg;
    CfitKey* tried;
    int ntried, captried;
    int first, round, flips, done, started, resync, await_first;
    int cand[CFIT_TRIES], ncand, ci;     /* the round's candidates, latest first */
    int spans[CFIT_MAX_SPANS][2], nspans, si;
    int in_trial, trial_lane, trial_axis;
    int16_t* saved;                      /* the lane array before the trial */
} Cfit;

static int cfit_cmp_seg(const void* a, const void* b)
{
    const CfitSeg* x = a;
    const CfitSeg* y = b;
    if (x->first != y->first) return x->first < y->first ? -1 : 1;
    if (x->last != y->last) return x->last < y->last ? -1 : 1;
    if (x->axis != y->axis) return x->axis < y->axis ? -1 : 1;
    return x->lane < y->lane ? -1 : x->lane > y->lane;
}

Cfit* cfit_new(int lanes, int count)
{
    Cfit* f = calloc(1, sizeof *f);
    f->lanes = lanes > CFIT_MAX_LANES ? CFIT_MAX_LANES : lanes;
    f->count = count;
    f->first = -1;
    f->saved = malloc(sizeof(int16_t) * (size_t) (count ? count : 1));
    return f;
}

void cfit_free(Cfit* f)
{
    int l;
    for (l = 0; l < CFIT_MAX_LANES; ++l) { free(f->raw[l][0]); free(f->raw[l][1]); }
    free(f->seg); free(f->tried); free(f->saved); free(f);
}

static int cfit_clip(double v)
{
    double t = trunc(v * 80.0);
    return (int) (t < -80 ? -80 : t > 80 ? 80 : t);
}

void cfit_lane(Cfit* f, int lane, const float* cx, const float* cy)
{
    int i, start = -1, startv = 0;
    int* at = malloc(sizeof(int) * (size_t) (f->count + 1));
    f->raw[lane][0] = malloc(sizeof(int16_t) * (size_t) f->count);
    f->raw[lane][1] = malloc(sizeof(int16_t) * (size_t) f->count);
    for (i = 0; i < f->count; ++i) {
        double x = cx[i], y = cy[i];
        f->raw[lane][0][i] = (int16_t) cfit_clip(x);
        f->raw[lane][1][i] = (int16_t) cfit_clip(y);
        at[i] = (fabs(fabs(x) - 0.9875) < 1e-6 && y == 0 ? (x > 0) - (x < 0) : 0) +
                (fabs(fabs(y) - 0.9875) < 1e-6 && x == 0 ? 2 * ((y > 0) - (y < 0)) : 0);
    }
    at[f->count] = 0;
    for (i = 0; i <= f->count; ++i) {
        int v = at[i];
        if (start >= 0 && v != startv) {
            f->seg = realloc(f->seg, sizeof(CfitSeg) * (size_t) (f->nseg + 1));
            f->seg[f->nseg].first = start; f->seg[f->nseg].last = i - 1;
            f->seg[f->nseg].axis = startv; f->seg[f->nseg].lane = lane;
            ++f->nseg;
            start = -1;
        }
        if (start < 0 && v != 0) { start = i; startv = v; }
    }
    free(at);
    qsort(f->seg, (size_t) f->nseg, sizeof(CfitSeg), cfit_cmp_seg);
}

const int16_t* cfit_raw(Cfit* f, int lane, int axis) { return f->raw[lane][axis]; }
int cfit_first(Cfit* f) { return f->first; }
int cfit_flips(Cfit* f) { return f->flips; }
int cfit_segments(Cfit* f) { return f->nseg; }

static int cfit_was_tried(Cfit* f, const CfitKey* k)
{
    int i;
    for (i = 0; i < f->ntried; ++i) {
        const CfitKey* t = &f->tried[i];
        if (t->lane == k->lane && t->axis == k->axis && t->lo == k->lo && t->hi == k->hi && t->at == k->at) return 1;
    }
    return 0;
}

static void cfit_round(Cfit* f)
{
    int i, n = 0;
    int tmp[256];
    for (i = 0; i < f->nseg && n < 256; ++i) {
        if (f->seg[i].first <= f->first && f->seg[i].last >= f->first - CFIT_REACH) tmp[n++] = i;
    }
    f->ncand = 0;
    for (i = n - 1; i >= 0 && f->ncand < CFIT_TRIES; --i) f->cand[f->ncand++] = tmp[i];
    f->ci = 0;
    f->nspans = 0;
    f->si = 0;
}

static void cfit_spans(Cfit* f, const CfitSeg* s)
{
    int tlo = s->first > f->first - CFIT_REACH ? s->first : f->first - CFIT_REACH;
    int thi = s->last < f->first ? s->last : f->first;
    int i, k;
    f->nspans = 0;
    f->spans[0][0] = s->first; f->spans[0][1] = s->last; f->nspans = 1;
    if (!(tlo == s->first && thi == s->last) && tlo <= thi) {
        f->spans[f->nspans][0] = tlo; f->spans[f->nspans][1] = thi; ++f->nspans;
    }
    for (i = thi; i >= tlo && f->nspans < CFIT_MAX_SPANS; --i) {
        int dup = 0;
        for (k = 0; k < f->nspans; ++k) if (f->spans[k][0] == i && f->spans[k][1] == i) dup = 1;
        if (!dup) { f->spans[f->nspans][0] = i; f->spans[f->nspans][1] = i; ++f->nspans; }
    }
    f->si = 0;
}

int cfit_resync_mode(Cfit* f) { return f->resync; }

int cfit_next(Cfit* f, int first, int near_only)
{
    if (!f->started) {
        f->started = 1;
        f->first = first;
        if (first < 0) { f->done = 1; return 0; }
        if (near_only) { f->resync = 1; f->await_first = 1; return 2; }
        cfit_round(f);
    } else if (f->await_first) {
        f->await_first = 0;
        f->first = first;
        if (first < 0) { f->done = 1; return 0; }
        cfit_round(f);
    }
    if (f->done) return 0;
    for (;;) {
        const CfitSeg* s;
        if (f->ci >= f->ncand) { f->done = 1; return 0; }          /* nothing improved: the fit ends */
        s = &f->seg[f->cand[f->ci]];
        if (f->nspans == 0) cfit_spans(f, s);
        while (f->si < f->nspans) {
            int lo = f->spans[f->si][0], hi = f->spans[f->si][1];
            CfitKey k;
            int axisx = (s->axis == 1 || s->axis == -1);
            int gate = axisx ? s->axis : s->axis / 2;
            int16_t* arr = f->raw[s->lane][axisx ? 0 : 1];
            int snapped, j;
            ++f->si;
            k.lane = s->lane; k.axis = s->axis; k.lo = lo; k.hi = hi; k.at = f->first;
            if (cfit_was_tried(f, &k)) continue;
            if (f->ntried == f->captried) {
                f->captried = f->captried ? f->captried * 2 : 64;
                f->tried = realloc(f->tried, sizeof(CfitKey) * (size_t) f->captried);
            }
            f->tried[f->ntried++] = k;
            memcpy(f->saved, arr, sizeof(int16_t) * (size_t) f->count);
            snapped = abs(arr[lo]) >= 80;
            for (j = lo; j <= hi; ++j) arr[j] = (int16_t) (gate * (snapped ? 79 : 80));
            f->trial_lane = s->lane; f->trial_axis = axisx ? 0 : 1; f->in_trial = 1;
            return f->resync ? 2 : 1;
        }
        ++f->ci; f->nspans = 0; f->si = 0;
    }
}

void cfit_result(Cfit* f, int moved)
{
    if (!f->in_trial) return;
    f->in_trial = 0;
    if (moved < 0 || moved > f->first) {
        f->first = moved;
        ++f->flips;
        ++f->round;
        if (moved < 0 || f->round >= CFIT_ROUNDS) { f->done = 1; return; }
        cfit_round(f);
    } else {
        memcpy(f->raw[f->trial_lane][f->trial_axis], f->saved, sizeof(int16_t) * (size_t) f->count);
    }
}
