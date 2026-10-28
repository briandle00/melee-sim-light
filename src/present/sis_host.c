/* Viewer build only (MSL_GX_RECORD): the text engine's (baselib/sislib.c, vendored) host glue.
 *
 * The console's text engine keeps its blocks in one 32-bit arena of its own, with a block header of 12 bytes. The host uses malloc:
 * which blocks exist and what they hold is the same, where they are is not visible to anything. The block structs
 * (HSD_Text, SisBlock, sislib_UnkAlloc3) have their host sizes.
 * refs/melee/src/sysdolphin/baselib/sislib.c::{HSD_SisLib_Alloc,HSD_SisLib_Free,HSD_SisLib_803A6048,HSD_SisLib_803A945C,HSD_SisLib_803A947C} */

#if MSL_GX_RECORD

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <baselib/archive.h>
#include <baselib/sislib.h>

#include "sis_host.h"

HSD_Archive* msl_host_archive_find(const char* basename);
void msl_gx_known_block(const void* host, size_t size);
extern unsigned char HSD_SisLib_8040CD40[147456];

static unsigned char** handles;
static u32 handle_count;
static u32 handle_capacity;
static HSD_Text* sis_free_dummy;

void* HSD_SisLib_Alloc(s32 size)
{
    return calloc(1, (size_t) (size > 0 ? size : 1) + 32);
}

void HSD_SisLib_Free(void* ptr)
{
    free(ptr);
}

void HSD_SisLib_803A6048(size_t size)
{
    int i;

    (void) size;
    (void) sis_free_dummy;
    /* the default font's glyph bitmaps are textures of the draw pass */
    msl_gx_known_block(HSD_SisLib_8040CD40, sizeof(HSD_SisLib_8040CD40));
    used_head = NULL;
    free_head = NULL;
    HSD_SisLib_804D7978 = NULL;
    HSD_SisLib_804D797C = NULL;
    handle_count = 0;
    for (i = 0; i < 5; i++) {
        HSD_SisLib_804D1124[i] = NULL;
    }
}

u32 sis_handle(void* pointer)
{
    u32 i;

    for (i = 0; i < handle_count; ++i) {
        if (handles[i] == pointer) {
            return i + 1;
        }
    }
    if (handle_count == handle_capacity) {
        handle_capacity = handle_capacity ? handle_capacity * 2 : 64;
        handles = realloc(handles, handle_capacity * sizeof(*handles));
    }
    handles[handle_count] = pointer;
    return ++handle_count;
}

unsigned char* sis_resolve(u32 handle)
{
    return handle != 0 && handle <= handle_count ? handles[handle - 1] : NULL;
}

/* the archives of fonts are the ones GameData preloaded (game_data.c) */
HSD_Archive* HSD_SisLib_803A945C(char* path)
{
    return msl_host_archive_find(path);
}

void HSD_SisLib_803A947C(HSD_Archive* archive)
{
    (void) archive;
}

#endif
