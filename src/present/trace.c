/* Viewer build only (MSL_GX_RECORD): a debugging aid. With MSL_GX_TRACE=<file> every draw callback the display pass runs is listed with the
 * write-gather pipe's length before and after it, so a primitive of a recorded frame (a dff token's `pos`) can be traced to the object that
 * drew it: "frame <index> pass <p> gobj <class> link <gx_link> prio <render_priority> cb <function address> pos <before> <after>".
 * The frame index is the replay's; the pipe length is the frame's own (it restarts at each frame). */

#if MSL_GX_RECORD

#include <stdio.h>
#include <stdlib.h>

#include <baselib/gobj.h>

#include "gx/msl_gx.h"

void msl_gx_trace_render(HSD_GObj* gobj, int pass, int after);
static FILE* trace_file;
static int trace_ready;
static size_t trace_before[64];
static int trace_depth;

void msl_gx_trace_frame_mark(int index)
{
    if (!trace_ready) {
        const char* path = getenv("MSL_GX_TRACE");
        trace_ready = 1;
        if (path != NULL && path[0] != '\0') {
            trace_file = fopen(path, "w");
            if (trace_file != NULL) {
                /* the run's load address of a known function, to put names to the callbacks with `nm` */
                fprintf(trace_file, "anchor %p msl_gx_trace_render\n", (void*) msl_gx_trace_render);
            }
        }
    }
    if (trace_file != NULL) {
        fprintf(trace_file, "frame %d\n", index);
    }
}

void msl_gx_trace_render(HSD_GObj* gobj, int pass, int after)
{
    if (trace_file == NULL) {
        return;
    }
    if (!after) {
        if (trace_depth < 64) {
            trace_before[trace_depth] = msl_gx_frame_bytes();
        }
        trace_depth++;
        return;
    }
    trace_depth--;
    if (trace_depth >= 0 && trace_depth < 64 && msl_gx_frame_bytes() != trace_before[trace_depth]) {
        fprintf(trace_file, "  pass %d gobj %u link %u prio %u cb %p pos %zu %zu depth %d\n", pass, (unsigned) gobj->classifier,
                (unsigned) gobj->gx_link, (unsigned) gobj->render_priority, (void*) gobj->render_cb, trace_before[trace_depth],
                msl_gx_frame_bytes(), trace_depth);
    }
}

#endif
