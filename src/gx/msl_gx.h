#ifndef MSL_GX_H
#define MSL_GX_H

/* The viewer build's GX layer: the Dolphin SDK's GX library (src/gx/sdk, the decomp's source) writes the bytes the game
 * sends to the write-gather pipe into a memory buffer instead, in the order and form the GPU's command processor reads
 * them (BP / CP / XF register loads, primitives with their vertex data, display lists inlined), the format a Dolphin
 * FIFO recording (.dff) holds. Compiled only with MSL_GX_RECORD (the viewer build); the production build does not
 * contain it. */

#if MSL_GX_RECORD

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- the pipe: what GXWGFifo.u8 = x ... writes (big-endian, as the console's bus is) */
typedef struct MslGxPipe {
    uint8_t* data;
    size_t len;
    size_t cap;
} MslGxPipe;

extern MslGxPipe msl_gx_pipe;
void msl_gx_pipe_grow(size_t need);

static inline void msl_gx_put_u8(uint8_t v)
{
    if (msl_gx_pipe.len + 1 > msl_gx_pipe.cap) {
        msl_gx_pipe_grow(1);
    }
    msl_gx_pipe.data[msl_gx_pipe.len++] = v;
}

static inline void msl_gx_put_u16(uint16_t v)
{
    if (msl_gx_pipe.len + 2 > msl_gx_pipe.cap) {
        msl_gx_pipe_grow(2);
    }
    msl_gx_pipe.data[msl_gx_pipe.len++] = (uint8_t) (v >> 8);
    msl_gx_pipe.data[msl_gx_pipe.len++] = (uint8_t) v;
}

static inline void msl_gx_put_u32(uint32_t v)
{
    if (msl_gx_pipe.len + 4 > msl_gx_pipe.cap) {
        msl_gx_pipe_grow(4);
    }
    msl_gx_pipe.data[msl_gx_pipe.len++] = (uint8_t) (v >> 24);
    msl_gx_pipe.data[msl_gx_pipe.len++] = (uint8_t) (v >> 16);
    msl_gx_pipe.data[msl_gx_pipe.len++] = (uint8_t) (v >> 8);
    msl_gx_pipe.data[msl_gx_pipe.len++] = (uint8_t) v;
}

static inline void msl_gx_put_f32(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    msl_gx_put_u32(v);
}

void msl_gx_put_bytes(const void* bytes, size_t n);

/* GXWGFifo.<T> = x, by the type the assignment names */
#define msl_gx_wg_u8(x) msl_gx_put_u8((uint8_t) (x))
#define msl_gx_wg_s8(x) msl_gx_put_u8((uint8_t) (x))
#define msl_gx_wg_u16(x) msl_gx_put_u16((uint16_t) (x))
#define msl_gx_wg_s16(x) msl_gx_put_u16((uint16_t) (x))
#define msl_gx_wg_u32(x) msl_gx_put_u32((uint32_t) (x))
#define msl_gx_wg_s32(x) msl_gx_put_u32((uint32_t) (x))
#define msl_gx_wg_f32(x) msl_gx_put_f32((float) (x))

/* -- main memory: the console's addresses for the host memory the commands point at (textures, palettes, vertex
 * arrays). A host pointer gets a fixed address in a made-up 64 MiB main memory the first time it is named; the
 * recorder keeps the bytes behind it for the recording's memory updates. */
uint32_t msl_gx_phys(const void* host);
uint32_t msl_gx_phys_dest(const void* host, size_t bytes);
uint32_t msl_gx_phys_data(const void* host, size_t bytes);   /* data the GPU reads (a texture of that size): memory that is not a file's keeps its bytes */   /* a render target of that many bytes: it owns its range */
void msl_gx_known_block(const void* host, size_t size);

/* -- frames and recordings */
void msl_gx_init(void);                 /* GXInit on the pipe; once */
int msl_gx_ready(void);
void msl_gx_frame_begin(void);          /* a frame's commands start here */
void msl_gx_frame_end(void);            /* the frame ends (the swap): keep it when a recording is on */
int msl_gx_recording_start(const char* path);
/* Streaming (the browser build): a recording that keeps only the last frame. msl_gx_kept_* describe it until the next frame ends. */
void msl_gx_set_stream_mode(int on);
struct MslCoreMatch;
void msl_gx_stream_frame(struct MslCoreMatch* match, int first);   /* gx_display.c: one display pass into the streaming recording */
int msl_gx_kept(const uint8_t** data, size_t* len, int* update_count);
int msl_gx_kept_update(int i, uint32_t* addr, const uint8_t** host, uint32_t* size, int* type, uint32_t* pos);   /* pos: the stream offset the bytes must be in place by */
const uint32_t* msl_gx_initial_state(int which);   /* 0: BP (256 words), 1: CP (256), 2: XF memory (0x1000) then XF registers (0x60) at the recording's start */
void msl_gx_set_hash_mode(int on);          /* a recording keeps only a hash of each frame (stderr: GXHASH <n> <fnv1a64>), for long determinism checks */
int msl_gx_recording_finish(void);      /* writes the .dff; returns 0 on success */
size_t msl_gx_frame_bytes(void);

void msl_gx_video_init(void);
void msl_gx_video_end_frame(void);
void msl_gx_video_frame_tail(void);

/* the display pass of one simulated frame (gx_display.c); the replay driver calls it after each step */
struct MslCoreMatch;
void msl_gx_frame(struct MslCoreMatch* match, int index);

#ifdef __cplusplus
}
#endif

#endif /* MSL_GX_RECORD */
#endif
