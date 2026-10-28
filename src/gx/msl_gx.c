#include "gx/msl_gx.h"

#if MSL_GX_RECORD

#include <stdio.h>
#include <stdlib.h>

#include <dolphin/gx.h>

#include "platform/native_dat.h"

MslGxPipe msl_gx_pipe;

void msl_gx_pipe_grow(size_t need)
{
    size_t cap = msl_gx_pipe.cap ? msl_gx_pipe.cap : (size_t) 1 << 20;
    while (msl_gx_pipe.len + need > cap) {
        cap *= 2;
    }
    if (cap != msl_gx_pipe.cap) {
        uint8_t* data = realloc(msl_gx_pipe.data, cap);
        if (data == NULL) {
            fprintf(stderr, "msl_gx: out of memory growing the pipe to %zu bytes\n", cap);
            abort();
        }
        msl_gx_pipe.data = data;
        msl_gx_pipe.cap = cap;
    }
}

void msl_gx_put_bytes(const void* bytes, size_t n)
{
    msl_gx_pipe_grow(n);
    memcpy(msl_gx_pipe.data + msl_gx_pipe.len, bytes, n);
    msl_gx_pipe.len += n;
}

size_t msl_gx_frame_bytes(void)
{
    return msl_gx_pipe.len;
}

/* ------------------------------------------------------------------------------------------------ main memory */

enum {
    MSL_GX_MEM_BASE = 0x00010000,   /* the first address handed out */
    MSL_GX_MEM_END = 0x01800000,    /* a Dolphin recording's decoder knows 24 MiB */
};

typedef struct MslGxBlock {
    const uint8_t* host;
    size_t size;
    uint32_t addr;
    uint32_t span;         /* the address range this block owns (a render target's is its size: nothing else is placed in it) */
    uint8_t emitted;       /* in the current recording's memory updates */
    uint8_t runtime;       /* a block of memory the game made at run time (no bytes known) */
    uint8_t heap;          /* memory the game made at run time that holds data the GPU reads (a texture, a palette): its bytes are sent like a file's */
    uint64_t hash;         /* the bytes as last sent (a quick hash) */
    unsigned stamp;        /* the frame this block was last looked at in */
    uint8_t* mirror;       /* MSL_GX_VERIFY: the consumer's copy */
} MslGxBlock;

static MslGxBlock* blocks;
static size_t block_count;
static size_t block_cap;
#define MSL_GX_TAIL 2048u
static uint32_t next_addr = MSL_GX_MEM_BASE;

static MslGxBlock* block_find(const uint8_t* host)
{
    size_t lo = 0;
    size_t hi = block_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (blocks[mid].host + blocks[mid].size <= host) {
            lo = mid + 1;
        } else if (blocks[mid].host > host) {
            hi = mid;
        } else {
            return &blocks[mid];
        }
    }
    return NULL;
}

static MslGxBlock* block_add(const uint8_t* host, size_t size)
{
    size_t lo = 0;
    size_t hi = block_count;
    MslGxBlock* b;
    uint32_t span = (uint32_t) ((size + 31) & ~(size_t) 31);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (blocks[mid].host < host) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (block_count == block_cap) {
        block_cap = block_cap ? block_cap * 2 : 1024;
        blocks = realloc(blocks, block_cap * sizeof(*blocks));
        if (blocks == NULL) {
            abort();
        }
    }
    memmove(&blocks[lo + 1], &blocks[lo], (block_count - lo) * sizeof(*blocks));
    block_count++;
    b = &blocks[lo];
    memset(b, 0, sizeof(*b));
    b->host = host;
    b->size = size;
    b->emitted = 0;
    b->addr = next_addr;
    b->span = span;
    if ((uint64_t) next_addr + span > MSL_GX_MEM_END) {
        fprintf(stderr, "msl_gx: the made-up main memory is full (%u bytes used)\n", next_addr);
        abort();
    }
    /* a palette is read in a whole (the game loads 256 entries for any 8-bit image, and what follows a short palette in memory is
     * not the file's bytes): the address space after a small block is kept as known zero bytes */
    next_addr += span + 32 + (size < MSL_GX_TAIL ? MSL_GX_TAIL : 0);
    return b;
}

static void note_reference(MslGxBlock* b);

/* A render target (the destination of an EFB copy) owns the whole of its range of the made-up memory: textures and arrays placed after it must not
 * fall inside, or the copy would overwrite them for the consumer (a 256x256 copy was given 32 bytes and a texture was placed in the rest). */
uint32_t msl_gx_phys_dest(const void* host, size_t bytes)
{
    const uint8_t* p = host;
    const uint8_t* base;
    size_t size;
    MslGxBlock* b;
    uint32_t span = (uint32_t) ((bytes + 31) & ~(size_t) 31);
    if (p == NULL) {
        return 0;
    }
    b = block_find(p);
    if (b == NULL && !msl_native_dat_block(p, &base, &size)) {
        b = block_add(p, bytes > MSL_GX_TAIL ? bytes : MSL_GX_TAIL);
        b->size = 1;
        b->runtime = 1;
    } else if (b != NULL && b->runtime && b->span < span) {
        b->addr = next_addr;
        b->span = span;
        next_addr += span + 32;
        if (next_addr > MSL_GX_MEM_END) {
            fprintf(stderr, "msl_gx: the made-up main memory is full (%u bytes used)%c", next_addr, 10);
            abort();
        }
    }
    return msl_gx_phys(host);
}

/* Data the GPU reads from memory that is not a file's (the HUD's textures are built at run time): the caller knows how many bytes it will read, so the bytes
 * are held and sent like any other block's (and sent again when they change). */
uint32_t msl_gx_phys_data(const void* host, size_t bytes)
{
    const uint8_t* p = host;
    const uint8_t* base;
    size_t size;
    MslGxBlock* b;
    if (p == NULL || bytes == 0) {
        return msl_gx_phys(host);
    }
    b = block_find(p);
    if (b == NULL && !msl_native_dat_block(p, &base, &size)) {
        b = block_add(p, bytes);
        b->heap = 1;
    } else if (b != NULL && b->runtime && b->size == 1) {
        /* a picture the game reads as a texture from memory it made at run time (the screen the Stadium jumbotron shows, a copy target of the frame
         * buffer): the console's memory holds what the game wrote there (a copy lands in the GPU's own cache), so its bytes are sent like a file's -
         * the first read can come before any copy */
        size_t need = (size_t) (p - b->host) + bytes;
        uint32_t span = (uint32_t) ((need + 31) & ~(size_t) 31);
        b->size = need;
        b->runtime = 0;
        b->heap = 1;
        b->emitted = 0;
        if (span > b->span) {
            b->addr = next_addr;
            b->span = span;
            next_addr += span + 32;
            if (next_addr > MSL_GX_MEM_END) {
                fprintf(stderr, "msl_gx: the made-up main memory is full (%u bytes used)%c", next_addr, 10);
                abort();
            }
        }
    } else if (b != NULL && b->heap && (size_t) (p - b->host) + bytes > b->size) {
        uint32_t span = (uint32_t) ((((size_t) (p - b->host) + bytes) + 31) & ~(size_t) 31);
        b->size = (size_t) (p - b->host) + bytes;
        b->addr = next_addr;
        b->span = span;
        b->emitted = 0;
        next_addr += span + 32;
        if (next_addr > MSL_GX_MEM_END) {
            fprintf(stderr, "msl_gx: the made-up main memory is full (%u bytes used)%c", next_addr, 10);
            abort();
        }
    }
    return msl_gx_phys(host);
}

/* A table of the host program that the game addresses as memory (the text engine's font bitmaps, GALE01 0x8040CD40): its whole extent is one block, so
   the textures cut from it are sent with their bytes. */
void msl_gx_known_block(const void* host, size_t size)
{
    if (host != NULL && block_find(host) == NULL) {
        block_add(host, size);
    }
}

/* present/hud.c: the game's static data (DOL .data) a draw can point the GPU at */
extern int msl_present_static_block(const void* pointer, const uint8_t** base, size_t* size);

uint32_t msl_gx_phys(const void* host)
{
    const uint8_t* p = host;
    const uint8_t* base;
    size_t size;
    MslGxBlock* b;
    if (p == NULL) {
        return 0;
    }
    b = block_find(p);
    if (b == NULL) {
        if (msl_native_dat_block(p, &base, &size) || msl_present_static_block(p, &base, &size)) {
            b = block_add(base, size);
        } else {
            /* memory the game made at run time (a copy target of the frame buffer, say): an address with no bytes */
            b = block_add(p, 1);
            b->size = 1;
            b->runtime = 1;
        }
    }
    if (b->size > 1) {
        note_reference(b);
    }
    return b->addr + (uint32_t) (p - b->host);
}

/* ------------------------------------------------------------------------------------------------ the shadow */

/* What the GPU holds after the bytes so far, kept by reading them as the command processor does (the recording's
 * header is this state at the first recorded frame). */
static uint32_t sh_bp[256];
static uint32_t sh_cp[256];
static uint32_t sh_xf[0x1000 + 0x60];
static uint32_t sh_bp_mask = 0xFFFFFF;
static size_t parsed;
static size_t copy_end;   /* end of the last XFB copy (BP 0x52) written in the pipe: where a recorded frame ends */
static size_t carry;      /* bytes after that copy, which belong to the next frame (GXCopyDisp restores zmode and cmode0 there) */

static unsigned bits(uint32_t v, int start, int width)
{
    return (v >> start) & ((1u << width) - 1);
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static const uint8_t comp_size[8] = { 1, 1, 2, 2, 4, 4, 4, 4 };
static const uint8_t color_size[8] = { 2, 3, 4, 2, 3, 4, 4, 4 };

/* the size of one vertex in vertex format `vat` with the attributes the CP holds (gx_dff.py: Decoder.vertex_layout) */
static unsigned vertex_size(unsigned vat)
{
    uint32_t lo = sh_cp[0x50];
    uint32_t hi = sh_cp[0x60];
    uint32_t a = sh_cp[0x70 + vat];
    uint32_t b = sh_cp[0x80 + vat];
    uint32_t c = sh_cp[0x90 + vat];
    unsigned size = 0;
    unsigned k;
    unsigned t;
    if (lo & 1) {
        size += 1;
    }
    for (t = 0; t < 8; ++t) {
        if (lo & (1u << (t + 1))) {
            size += 1;
        }
    }
    k = bits(lo, 9, 2);
    if (k) {
        size += k == 2 ? 1 : k == 3 ? 2 : comp_size[bits(a, 1, 3)] * (bits(a, 0, 1) ? 3 : 2);
    }
    k = bits(lo, 11, 2);
    if (k) {
        unsigned nbt = bits(a, 9, 1);
        unsigned nfmt = bits(a, 10, 3);
        if (k == 1) {
            size += comp_size[nfmt] * (nbt ? 9 : 3);
        } else {
            unsigned idx = k == 2 ? 1 : 2;
            size += idx * ((nbt && bits(a, 31, 1)) ? 3 : 1);
        }
    }
    k = bits(lo, 13, 2);
    if (k) {
        size += k == 1 ? color_size[bits(a, 14, 3)] : k == 2 ? 1 : 2;
    }
    k = bits(lo, 15, 2);
    if (k) {
        size += k == 1 ? color_size[bits(a, 18, 3)] : k == 2 ? 1 : 2;
    }
    {
        unsigned el[8] = { bits(a, 21, 1), bits(b, 0, 1), bits(b, 9, 1), bits(b, 18, 1),
                           bits(b, 27, 1), bits(c, 5, 1), bits(c, 14, 1), bits(c, 23, 1) };
        unsigned fm[8] = { bits(a, 22, 3), bits(b, 1, 3), bits(b, 10, 3), bits(b, 19, 3),
                           bits(b, 28, 3), bits(c, 6, 3), bits(c, 15, 3), bits(c, 24, 3) };
        for (t = 0; t < 8; ++t) {
            k = bits(hi, 2 * t, 2);
            if (k) {
                size += k == 2 ? 1 : k == 3 ? 2 : comp_size[fm[t]] * (el[t] ? 2 : 1);
            }
        }
    }
    return size;
}

static void shadow_bp(uint32_t word)
{
    unsigned addr = word >> 24;
    uint32_t val = word & 0xFFFFFF;
    if (addr == 0xFE) {
        sh_bp_mask = val;
        return;
    }
    sh_bp[addr] = (sh_bp[addr] & ~sh_bp_mask & 0xFFFFFF) | (val & sh_bp_mask);
    sh_bp_mask = 0xFFFFFF;
}

static int bad_command;

/* the commands in [parsed, len) of the pipe, applied to the shadow; stops at a command not wholly written */
static void shadow_sync(void)
{
    const uint8_t* d = msl_gx_pipe.data;
    size_t n = msl_gx_pipe.len;
    size_t pos = parsed;
    while (pos < n && !bad_command) {
        unsigned op = d[pos];
        if (op == 0x00 || op == 0x44 || op == 0x48 || op == 0x01) {
            pos += 1;
        } else if ((op & 0xC0) == 0x80) {
            unsigned count;
            size_t end;
            if (pos + 3 > n) {
                break;
            }
            count = d[pos + 1] << 8 | d[pos + 2];
            end = pos + 3 + (size_t) vertex_size(op & 7) * count;
            if (end > n) {
                break;
            }
            pos = end;
        } else if (op == 0x61) {
            if (pos + 5 > n) {
                break;
            }
            shadow_bp(rd32(d + pos + 1));
            pos += 5;
            if (d[pos - 4] == 0x52 && (rd32(d + pos - 4) & 0x4000)) {   /* the copy to the XFB, not a texture copy */
                copy_end = pos;
            }
        } else if (op == 0x08) {
            if (pos + 6 > n) {
                break;
            }
            sh_cp[d[pos + 1]] = rd32(d + pos + 2);
            pos += 6;
        } else if (op == 0x10) {
            uint32_t head;
            unsigned cnt;
            unsigned i;
            if (pos + 5 > n) {
                break;
            }
            head = rd32(d + pos + 1);
            cnt = (head >> 16) + 1;
            if (pos + 5 + 4 * (size_t) cnt > n) {
                break;
            }
            for (i = 0; i < cnt; ++i) {
                unsigned a = (head & 0xFFFF) + i;
                if (a < sizeof(sh_xf) / sizeof(sh_xf[0])) {
                    sh_xf[a] = rd32(d + pos + 5 + 4 * i);
                }
            }
            pos += 5 + 4 * (size_t) cnt;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (pos + 5 > n) {
                break;
            }
            pos += 5;   /* an indexed XF load reads main memory: the recording keeps what it read */
        } else if (op == 0x40) {
            if (pos + 9 > n) {
                break;
            }
            pos += 9;
        } else {
            fprintf(stderr, "msl_gx: unknown command %02x at %zu of the frame\n", op, pos);
            bad_command = 1;
            break;
        }
    }
    parsed = pos;
}

/* ------------------------------------------------------------------------------------------------ frames, a recording */

static const uint8_t msl_gx_zero_tail[2048];

typedef struct MslGxUpdate {
    uint32_t pos;
    uint32_t addr;
    const uint8_t* host;
    uint32_t size;
    uint8_t type;
    uint8_t owned;   /* host is a copy made when the update was written: freed with the frame */
} MslGxUpdate;

typedef struct MslGxFrame {
    uint8_t* data;
    size_t len;
    MslGxUpdate* updates;
    size_t update_count;
} MslGxFrame;

static struct {
    int on;
    char path[1024];
    uint32_t bp[256];
    uint32_t cp[256];
    uint32_t xf[0x1000 + 0x60];
    MslGxFrame* frames;
    size_t frame_count;
    size_t frame_cap;
} rec;

static int initialized;
static MslGxFrame kept;
static int kept_valid;
static int hash_mode;
static int stream_mode;
static size_t hash_n;

void msl_gx_set_stream_mode(int on)
{
    stream_mode = on;
}

int msl_gx_kept(const uint8_t** data, size_t* len, int* update_count)
{
    if (!kept_valid) {
        return 0;
    }
    *data = kept.data;
    *len = kept.len;
    *update_count = (int) kept.update_count;
    return 1;
}

int msl_gx_kept_update(int i, uint32_t* addr, const uint8_t** host, uint32_t* size, int* type, uint32_t* pos)
{
    if (!kept_valid || i < 0 || (size_t) i >= kept.update_count) {
        return 0;
    }
    *addr = kept.updates[i].addr;
    *host = kept.updates[i].host;
    *size = kept.updates[i].size;
    *type = kept.updates[i].type;
    *pos = kept.updates[i].pos;
    return 1;
}

const uint32_t* msl_gx_initial_state(int which)
{
    return which == 0 ? rec.bp : which == 1 ? rec.cp : rec.xf;
}

void msl_gx_set_hash_mode(int on)
{
    hash_mode = on;
}

static uint64_t fnv1a(uint64_t h, const uint8_t* d, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        h = (h ^ d[i]) * 1099511628211ULL;
    }
    return h;
}

int msl_gx_ready(void)
{
    return initialized;
}

/* ----- memory the game rewrites: a block is hashed when the game first refers to it in a frame (texture, palette, array, display list),
 * and a block whose bytes changed since they were sent is sent again, at the stream position of the reference. */
static unsigned frame_serial;
static int dirty_off;            /* MSL_GX_NO_DIRTY: the old behaviour, each block once (for testing the verifier) */
static int verify_on;            /* MSL_GX_VERIFY: keep the consumer's copy of memory and compare it with the live bytes of every referenced block */
static unsigned long verify_checked, verify_bad, resend_count;
static MslGxUpdate* pend;        /* updates found mid-frame: written with the frame */
static size_t pend_count, pend_cap;

static uint64_t qhash(const uint8_t* d, size_t n)
{
    /* four independent lanes: a block is hashed each frame it is referenced, so the hash must run near memory speed */
    uint64_t h0 = 0x9E3779B97F4A7C15ULL ^ (uint64_t) n;
    uint64_t h1 = 0xC2B2AE3D27D4EB4FULL;
    uint64_t h2 = 0x165667B19E3779F9ULL;
    uint64_t h3 = 0x27D4EB2F165667C5ULL;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, d + i, 8);
        memcpy(&w1, d + i + 8, 8);
        memcpy(&w2, d + i + 16, 8);
        memcpy(&w3, d + i + 24, 8);
        h0 = (h0 ^ w0) * 0xFF51AFD7ED558CCDULL; h0 ^= h0 >> 32;
        h1 = (h1 ^ w1) * 0xFF51AFD7ED558CCDULL; h1 ^= h1 >> 32;
        h2 = (h2 ^ w2) * 0xFF51AFD7ED558CCDULL; h2 ^= h2 >> 32;
        h3 = (h3 ^ w3) * 0xFF51AFD7ED558CCDULL; h3 ^= h3 >> 32;
    }
    h0 ^= h1 * 3 + h2 * 5 + h3 * 7;
    for (; i < n; ++i) {
        h0 = (h0 ^ d[i]) * 0x100000001B3ULL;
    }
    return h0;
}

static uint8_t* snapshot(const uint8_t* d, size_t n)
{
    uint8_t* c = malloc(n ? n : 1);
    if (c == NULL) {
        abort();
    }
    memcpy(c, d, n);
    return c;
}

static void note_reference(MslGxBlock* b)
{
    uint64_t h;
    if (b->stamp == frame_serial) {
        return;
    }
    b->stamp = frame_serial;
    if (!rec.on || !b->emitted || dirty_off) {
        return;
    }
    h = qhash(b->host, b->size);
    if (h == b->hash) {
        return;
    }
    b->hash = h;
    if (pend_count == pend_cap) {
        pend_cap = pend_cap ? pend_cap * 2 : 16;
        pend = realloc(pend, pend_cap * sizeof(*pend));
        if (pend == NULL) {
            abort();
        }
    }
    pend[pend_count].pos = (uint32_t) msl_gx_pipe.len;
    pend[pend_count].addr = b->addr;
    pend[pend_count].host = snapshot(b->host, b->size);
    pend[pend_count].size = (uint32_t) b->size;
    pend[pend_count].type = 1;
    pend[pend_count].owned = 1;
    pend_count++;
    resend_count++;
}

static void free_frame(MslGxFrame* f)
{
    size_t i;
    for (i = 0; i < f->update_count; ++i) {
        if (f->updates[i].owned) {
            free((void*) f->updates[i].host);
        }
    }
    free(f->data);
    free(f->updates);
}

static unsigned overlap_count(void)
{
    size_t i, j;
    unsigned n = 0;
    for (i = 0; i < block_count; ++i) {
        for (j = i + 1; j < block_count; ++j) {
            if (blocks[i].addr < blocks[j].addr + blocks[j].span && blocks[j].addr < blocks[i].addr + blocks[i].span) {
                n++;
            }
        }
    }
    return n;
}

static void verify_report(void)
{
    if (verify_on) {
        fprintf(stderr, "VERIFY address ranges: %zu blocks, %u overlapping pairs%c", block_count, overlap_count(), 10);
        fprintf(stderr, "VERIFY frames %u referenced-blocks-checked %lu mismatches %lu blocks-sent-again %lu%c", frame_serial, verify_checked, verify_bad, resend_count, 10);
    }
}

static void verify_frame(const MslGxFrame* f)
{
    size_t i;
    for (i = 0; i < f->update_count; ++i) {
        MslGxBlock* b = NULL;
        size_t k;
        for (k = 0; k < block_count; ++k) {
            if (blocks[k].addr == f->updates[i].addr) {
                b = &blocks[k];
                break;
            }
        }
        if (b != NULL && f->updates[i].size <= b->size) {
            if (b->mirror == NULL) {
                b->mirror = calloc(b->size, 1);
            }
            if (f->updates[i].host == NULL) { fprintf(stderr, "VERIFY null host update %zu addr %08x size %u%c", i, f->updates[i].addr, f->updates[i].size, 10); continue; }
            memcpy(b->mirror, f->updates[i].host, f->updates[i].size);
        }
    }
    for (i = 0; i < block_count; ++i) {
        MslGxBlock* b = &blocks[i];
        if (b->stamp != frame_serial || b->size <= 1 || b->runtime) {
            continue;
        }
        verify_checked++;
        if (b->mirror == NULL || memcmp(b->mirror, b->host, b->size) != 0) {
            verify_bad++;
            if (verify_bad < 10) {
                fprintf(stderr, "VERIFY frame %u: block %08x (%zu bytes) differs from the consumer's copy%c", frame_serial, b->addr, b->size, 10);
            }
        }
    }
}

void msl_gx_frame_begin(void)
{
    static int mutate = -1;
    if (mutate < 0) {
        const char* e = getenv("MSL_GX_TEST_MUTATE");
        mutate = e != NULL ? atoi(e) : 0;
        dirty_off = getenv("MSL_GX_NO_DIRTY") != NULL;
        verify_on = getenv("MSL_GX_VERIFY") != NULL;
        if (verify_on) {
            atexit(verify_report);
        }
    }
    frame_serial++;
    if (mutate > 0 && rec.on && frame_serial % (unsigned) mutate == 0) {
        /* a test: the game 'rewrites' the first sizeable block it has sent (one byte), to see the change reach the consumer */
        size_t k;
        for (k = 0; k < block_count; ++k) {
            if (blocks[k].emitted && !blocks[k].runtime && blocks[k].size >= 4096 && blocks[k].stamp + 1 == frame_serial) {
                ((uint8_t*) blocks[k].host)[frame_serial % blocks[k].size] ^= 0x5A;
                break;
            }
        }
    }
    msl_gx_pipe.len = carry;   /* what the last frame wrote after its copy */
    parsed = carry;
    copy_end = 0;
    carry = 0;
}

void msl_gx_frame_end(void)
{
    shadow_sync();
    if (rec.on) {
        MslGxFrame* f;
        size_t i;
        if (rec.frame_count == rec.frame_cap) {
            rec.frame_cap = rec.frame_cap ? rec.frame_cap * 2 : 8;
            rec.frames = realloc(rec.frames, rec.frame_cap * sizeof(*rec.frames));
            if (rec.frames == NULL) {
                abort();
            }
        }
        f = &rec.frames[rec.frame_count++];
        size_t cut = copy_end ? copy_end : msl_gx_pipe.len;
        f->data = malloc(cut ? cut : 1);
        memcpy(f->data, msl_gx_pipe.data, cut);
        f->len = cut;
        carry = msl_gx_pipe.len - cut;
        memmove(msl_gx_pipe.data, msl_gx_pipe.data + cut, carry);
        f->updates = NULL;
        f->update_count = 0;
        /* every block the commands named, once, at the frame's start: in the order of their made-up addresses (the order they were first named in),
         * not of the host's pointers, which differ between builds (the 32-bit and 64-bit hosts lay the heap out differently) */
        size_t* order = malloc((block_count ? block_count : 1) * sizeof(*order));
        size_t oi;
        for (i = 0; i < block_count; ++i) {
            order[i] = i;
        }
        for (i = 1; i < block_count; ++i) {   /* (insertion: the list is nearly in address order already) */
            size_t v = order[i];
            size_t j = i;
            while (j > 0 && blocks[order[j - 1]].addr > blocks[v].addr) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = v;
        }
        for (oi = 0; oi < block_count; ++oi) {
            i = order[oi];
            if (!blocks[i].emitted && blocks[i].size > 1) {
                f->updates = realloc(f->updates, (f->update_count + 1) * sizeof(*f->updates));
                f->updates[f->update_count].pos = 0;
                f->updates[f->update_count].addr = blocks[i].addr;
                f->updates[f->update_count].host = snapshot(blocks[i].host, blocks[i].size);
                f->updates[f->update_count].owned = 1;
                f->updates[f->update_count].size = (uint32_t) blocks[i].size;
                f->updates[f->update_count].type = 1;
                f->update_count++;
                blocks[i].emitted = 1;
                blocks[i].hash = qhash(blocks[i].host, blocks[i].size);
                if (blocks[i].size < MSL_GX_TAIL && ((blocks[i].size + 31) & ~(size_t) 31) < MSL_GX_TAIL) {
                    uint32_t at = (uint32_t) ((blocks[i].size + 31) & ~(size_t) 31);
                    f->updates = realloc(f->updates, (f->update_count + 1) * sizeof(*f->updates));
                    f->updates[f->update_count].pos = 0;
                    f->updates[f->update_count].addr = blocks[i].addr + at;
                    f->updates[f->update_count].host = msl_gx_zero_tail;
                    f->updates[f->update_count].owned = 0;
                    f->updates[f->update_count].size = MSL_GX_TAIL - at;
                    f->updates[f->update_count].type = 1;
                    f->update_count++;
                }
            }
        }
        free(order);
        /* blocks that changed mid-frame: those referenced before the cut belong to this frame, the rest to the next */
        {
            size_t keep = 0;
            for (i = 0; i < pend_count; ++i) {
                if (pend[i].pos <= cut) {
                    f->updates = realloc(f->updates, (f->update_count + 1) * sizeof(*f->updates));
                    f->updates[f->update_count++] = pend[i];
                } else {
                    pend[i].pos -= (uint32_t) cut;
                    pend[keep++] = pend[i];
                }
            }
            pend_count = keep;
        }
        if (verify_on) {
            verify_frame(f);
        }
        if (stream_mode) {
            if (kept_valid) {
                free_frame(&kept);
            }
            kept = *f;
            kept_valid = 1;
            rec.frame_count--;
        } else if (hash_mode) {
            uint64_t h = 14695981039346656037ULL;
            h = fnv1a(h, f->data, f->len);
            for (i = 0; i < f->update_count; ++i) {
                uint32_t head[3] = { f->updates[i].addr, f->updates[i].size, f->updates[i].type };
                h = fnv1a(h, (const uint8_t*) head, sizeof(head));
                h = fnv1a(h, f->updates[i].host, f->updates[i].size);
            }
            fprintf(stderr, "GXHASH %zu %016llx\n", hash_n++, (unsigned long long) h);
            free_frame(f);
            rec.frame_count--;
        }
    }
}

int msl_gx_recording_start(const char* path)
{
    size_t i;
    shadow_sync();
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.path, sizeof(rec.path), "%s", path);
    memcpy(rec.bp, sh_bp, sizeof(rec.bp));
    memcpy(rec.cp, sh_cp, sizeof(rec.cp));
    memcpy(rec.xf, sh_xf, sizeof(rec.xf));
    for (i = 0; i < block_count; ++i) {
        blocks[i].emitted = 0;
    }
    pend_count = 0;
    rec.on = 1;
    return 0;
}

static void put_le32(FILE* f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) };
    fwrite(b, 1, 4, f);
}

static void put_le64(FILE* f, uint64_t v)
{
    put_le32(f, (uint32_t) v);
    put_le32(f, (uint32_t) (v >> 32));
}

static void pad_to(FILE* f, long multiple)
{
    long at = ftell(f);
    while (at % multiple) {
        fputc(0, f);
        at++;
    }
}

static void set_le(uint8_t* at, uint64_t v, int bytes)
{
    int i;
    for (i = 0; i < bytes; ++i) {
        at[i] = (uint8_t) (v >> (8 * i));
    }
}

/* a .dff, file version 4 (Dolphin's FifoFileStruct.h; tools/gx_dff.py's build_dff writes the same layout) */
int msl_gx_recording_finish(void)
{
    FILE* f;
    uint64_t bp_off;
    uint64_t cp_off;
    uint64_t xf_off;
    uint64_t xfr_off;
    uint64_t fl_off;
    uint64_t* f_off;
    uint64_t* u_off;
    uint64_t* data_off = NULL;
    size_t i;
    size_t k;
    uint8_t header[128];
    if (!rec.on) {
        return 1;
    }
    f = fopen(rec.path, "wb");
    if (f == NULL) {
        return 1;
    }
    memset(header, 0, sizeof(header));
    fwrite(header, 1, sizeof(header), f);
    bp_off = (uint64_t) ftell(f);
    for (i = 0; i < 256; ++i) {
        put_le32(f, rec.bp[i]);
    }
    cp_off = (uint64_t) ftell(f);
    for (i = 0; i < 256; ++i) {
        put_le32(f, rec.cp[i]);
    }
    xf_off = (uint64_t) ftell(f);
    for (i = 0; i < 0x1000; ++i) {
        put_le32(f, rec.xf[i]);
    }
    xfr_off = (uint64_t) ftell(f);
    for (i = 0; i < 0x60; ++i) {
        put_le32(f, rec.xf[0x1000 + i]);
    }
    f_off = calloc(rec.frame_count + 1, sizeof(*f_off));
    u_off = calloc(rec.frame_count + 1, sizeof(*u_off));
    /* each frame: its commands, then its updates' bytes, then the update records */
    for (i = 0; i < rec.frame_count; ++i) {
        MslGxFrame* fr = &rec.frames[i];
        pad_to(f, 4);
        f_off[i] = (uint64_t) ftell(f);
        fwrite(fr->data, 1, fr->len, f);
        data_off = realloc(data_off, (fr->update_count + 1) * sizeof(*data_off));
        for (k = 0; k < fr->update_count; ++k) {
            pad_to(f, 4);
            data_off[k] = (uint64_t) ftell(f);
            fwrite(fr->updates[k].host, 1, fr->updates[k].size, f);
        }
        pad_to(f, 8);
        u_off[i] = (uint64_t) ftell(f);
        for (k = 0; k < fr->update_count; ++k) {
            uint8_t r24[24];
            memset(r24, 0, sizeof(r24));
            set_le(r24 + 0, fr->updates[k].pos, 4);
            set_le(r24 + 4, fr->updates[k].addr, 4);
            set_le(r24 + 8, data_off[k], 8);
            set_le(r24 + 16, fr->updates[k].size, 4);
            r24[20] = fr->updates[k].type;
            fwrite(r24, 1, 24, f);
        }
    }
    free(data_off);
    pad_to(f, 8);
    fl_off = (uint64_t) ftell(f);
    for (i = 0; i < rec.frame_count; ++i) {
        uint8_t info[64];
        memset(info, 0, sizeof(info));
        set_le(info + 0, f_off[i], 8);
        set_le(info + 8, rec.frames[i].len, 4);
        set_le(info + 20, u_off[i], 8);
        set_le(info + 28, rec.frames[i].update_count, 4);
        fwrite(info, 1, sizeof(info), f);
    }
    fflush(f);
    fseek(f, 0, SEEK_SET);
    put_le32(f, 0x0D01F1F0);
    put_le32(f, 4);
    put_le32(f, 1);
    put_le64(f, bp_off);
    put_le32(f, 256);
    put_le64(f, cp_off);
    put_le32(f, 256);
    put_le64(f, xf_off);
    put_le32(f, 0x1000);
    put_le64(f, xfr_off);
    put_le32(f, 0x60);
    put_le64(f, fl_off);
    put_le32(f, (uint32_t) rec.frame_count);
    put_le32(f, 0);
    put_le64(f, 0);
    put_le32(f, 0);
    fclose(f);
    free(f_off);
    free(u_off);
    for (i = 0; i < rec.frame_count; ++i) {
        free(rec.frames[i].data);
        free(rec.frames[i].updates);
    }
    free(rec.frames);
    rec.on = 0;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ GXInit */

static uint8_t fifo_stand_in[64];

void msl_gx_init(void)
{
    if (initialized) {
        return;
    }
    initialized = 1;
    msl_gx_pipe.len = 0;
    /* GXInit(base, size) on the pipe: its register writes go through the same macros as every frame's */
    GXInit(fifo_stand_in, sizeof(fifo_stand_in));
    msl_gx_video_init();
    /* refs/melee/src/melee/gm/gmmain.c::main: after HSD_InitComponent */
    GXSetMisc(GX_MT_XF_FLUSH, 8);
    shadow_sync();
}

#endif /* MSL_GX_RECORD */
