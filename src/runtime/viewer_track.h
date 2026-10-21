#if MSL_VISUAL_TRACK_BUILD
#ifndef MSL_RUNTIME_VIEWER_TRACK_H
#define MSL_RUNTIME_VIEWER_TRACK_H

// The viewer track: state a replay viewer draws and a recording does not
// carry, one JSON array per line (agent_docs/VIEWER_TRACK.md). A match keeps
// one only when its config sets MSL_VISUAL_TRACK in camera_mode. Otherwise
// there is none, and every hook below is one untaken branch. The track reads
// simulation state; it never changes gameplay.

#include <stddef.h>

#include <platform.h>
#include <baselib/forward.h>
#include <dolphin/mtx.h>

struct MslCoreMatch;
typedef struct MslViewerTrack MslViewerTrack;

// The bound match's track, NULL when it has none (runtime/context.c).
extern _Thread_local MslViewerTrack* msl_core_context_viewer_track;
#define msl_viewer_track_on() (msl_core_context_viewer_track != NULL)

MslViewerTrack* msl_viewer_track_create(void);
void msl_viewer_track_destroy(MslViewerTrack* track);


// Per-frame state, at the end of each step (runtime/scalar.c).
void msl_viewer_track_frame(struct MslCoreMatch* match);

// The lines written since the last call (NUL-terminated, `size` bytes), or
// NULL when the match keeps no track. Valid until the match steps again.
const char* msl_core_match_viewer_track(struct MslCoreMatch* match,
                                        size_t* size);

#endif
#endif // MSL_VISUAL_TRACK_BUILD
