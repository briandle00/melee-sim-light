#include "platform/native_dat.h"
#include "platform/memory.h"
#include "runtime/context.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <baselib/jobj.h>
#include <baselib/id.h>
#include <baselib/memory.h>
#include <baselib/psstructs.h>
#include <baselib/quatlib.h>
#include <melee/ft/types.h>
#include <melee/ft/chara/ftGameWatch/forward.h>
#include <melee/gr/ground.h>
#include <melee/it/it_3F14.h>
#include <melee/it/types.h>
#if MSL_GX_RECORD
#include <baselib/lobj.h>
#include <melee/sc/types.h>
#endif
#ifndef MSL_CORE_WASM
#include <sys/mman.h>
#endif

// HSD DAT stores a 32-bit big-endian graph. On retail/PPC,
// HSD_ArchiveParse relocates that graph in place. A 64-bit little-endian
// process instead materializes native objects once at archive initialization.
// Source: refs/melee/src/sysdolphin/baselib/archive.c::HSD_ArchiveParse.

typedef struct MslDatMemo {
    uint32_t source_offset;
    const MslDatType* type;
    void* native;
} MslDatMemo;

typedef struct MslNativePublic {
    char* symbol;
    void* address;
} MslNativePublic;

typedef struct MslNativeArchive {
    HSD_Archive* archive;
    uint8_t* source;
    uint8_t* data;
    uint32_t data_size;
    uint32_t* reloc_fields;
    uint32_t reloc_count;
    uint32_t console_base;   /* where the console would hold the file's data: what a relocated pointer read past a block holds (the viewer build) */
    uint32_t* boundaries;
    uint32_t boundary_count;
    MslDatMemo* memo;
    uint32_t memo_count;
    uint32_t memo_capacity;
    uint8_t* command_words;
    const MslDatType* command_word_type;
    MslNativePublic* publics;
    uint32_t public_count;
    uint32_t public_capacity;
} MslNativeArchive;

enum {
    // Native graph nodes contain widened pointers and HSD data may request
    // 32-byte alignment. No per-type padding is required.
    MSL_NATIVE_DAT_ARENA_ALIGN = 32,
};

#define native_archive_cache (msl_core_native_dat_context()->archive_cache)
#define native_archive_cache_count                                            \
    (msl_core_native_dat_context()->archive_cache_count)
#define native_dat_arena (msl_core_native_dat_context()->arena)
#define native_dat_arena_used (msl_core_native_dat_context()->arena_used)
#define native_dat_initialization_complete                                    \
    (msl_core_native_dat_context()->initialization_complete)

enum {
    DW_ATE_BOOLEAN = 2,
    DW_ATE_FLOAT = 4,
    DW_ATE_SIGNED = 5,
    DW_ATE_SIGNED_CHAR = 6,
};

static uint16_t read_be16(const void* source)
{
    const uint8_t* bytes = source;
    return (uint16_t) ((uint16_t) bytes[0] << 8 | bytes[1]);
}

static uint32_t read_be32(const void* source)
{
    const uint8_t* bytes = source;
    return (uint32_t) bytes[0] << 24 | (uint32_t) bytes[1] << 16 |
           (uint32_t) bytes[2] << 8 | bytes[3];
}

static uint64_t read_be64(const void* source)
{
    return (uint64_t) read_be32(source) << 32 |
           read_be32((const uint8_t*) source + 4);
}

static int compare_u32(const void* lhs, const void* rhs)
{
    uint32_t a = *(const uint32_t*) lhs;
    uint32_t b = *(const uint32_t*) rhs;
    return a < b ? -1 : a > b;
}

#if MSL_GX_RECORD
// The viewer build's GX layer names the memory its commands point at (textures, vertex arrays) by the block of
// the native arena it lies in: each allocation's offset and size, in allocation (so address) order.
static uint32_t* gx_block_offsets;
static uint32_t* gx_block_sizes;
static size_t gx_block_count;
static size_t gx_block_cap;

static void gx_block_note(size_t offset, size_t size)
{
    if (gx_block_count == gx_block_cap) {
        gx_block_cap = gx_block_cap ? gx_block_cap * 2 : 65536;
        gx_block_offsets = realloc(gx_block_offsets, gx_block_cap * sizeof(*gx_block_offsets));
        gx_block_sizes = realloc(gx_block_sizes, gx_block_cap * sizeof(*gx_block_sizes));
        if (gx_block_offsets == NULL || gx_block_sizes == NULL) {
            abort();
        }
    }
    gx_block_offsets[gx_block_count] = (uint32_t) offset;
    gx_block_sizes[gx_block_count] = (uint32_t) size;
    gx_block_count++;
}

int msl_native_dat_block(const void* pointer, const uint8_t** base, size_t* size)
{
    size_t lo = 0;
    size_t hi = gx_block_count;
    size_t offset;
    if (!msl_native_dat_owns(pointer)) {
        return 0;
    }
    offset = (size_t) ((const uint8_t*) pointer - native_dat_arena);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (gx_block_offsets[mid] + (size_t) gx_block_sizes[mid] <= offset) {
            lo = mid + 1;
        } else if (gx_block_offsets[mid] > offset) {
            hi = mid;
        } else {
            *base = native_dat_arena + gx_block_offsets[mid];
            *size = gx_block_sizes[mid];
            return 1;
        }
    }
    return 0;
}
#endif

static void* native_alloc(size_t size)
{
    size_t aligned;
    void* result;

    if (native_dat_initialization_complete) {
        fprintf(stderr,
                "native DAT translation reached after match initialization\n");
        abort();
    }
    if (native_dat_arena == NULL) {
#ifdef MSL_CORE_WASM
        native_dat_arena = malloc(MSL_NATIVE_DAT_ARENA_BYTES);
        if (native_dat_arena == NULL) {
            fprintf(stderr, "native DAT arena reservation failed\n");
            abort();
        }
#else
        void* mapping = mmap(NULL, MSL_NATIVE_DAT_ARENA_BYTES,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            fprintf(stderr, "native DAT arena reservation failed\n");
            abort();
        }
        native_dat_arena = mapping;
#endif
    }
    if (size == 0) {
        size = 1;
    }
    if (size > MSL_NATIVE_DAT_ARENA_BYTES - MSL_NATIVE_DAT_ARENA_ALIGN) {
        fprintf(stderr, "native DAT allocation is too large: %zu bytes\n",
                size);
        abort();
    }
    aligned = (size + MSL_NATIVE_DAT_ARENA_ALIGN - 1) &
              ~(size_t) (MSL_NATIVE_DAT_ARENA_ALIGN - 1);
    if (aligned > MSL_NATIVE_DAT_ARENA_BYTES - native_dat_arena_used) {
        fprintf(stderr, "native DAT arena exhausted: request=%zu used=%zu/%u\n",
                size, native_dat_arena_used, MSL_NATIVE_DAT_ARENA_BYTES);
        abort();
    }
    result = native_dat_arena + native_dat_arena_used;
#if MSL_GX_RECORD
    gx_block_note(native_dat_arena_used, size);
#endif
    native_dat_arena_used += aligned;
    memset(result, 0, size);
    return result;
}

static void require_dat_initialization(const char* operation)
{
    if (native_dat_initialization_complete) {
        fprintf(stderr, "%s reached after native DAT initialization\n",
                operation);
        abort();
    }
}

void msl_native_dat_for_each_figa(void (*visit)(FigaTree*, void*),
                                  void* visit_context)
{
    uint32_t archive_index;
    HSD_ASSERT(171, visit != NULL);
    for (archive_index = 0; archive_index < native_archive_cache_count;
         ++archive_index)
    {
        MslNativeArchive* archive =
            native_archive_cache[archive_index].archive.top_ptr;
        uint32_t public_index;
        for (public_index = 0; public_index < archive->public_count;
             ++public_index)
        {
            MslNativePublic* public = &archive->publics[public_index];
            size_t length = strlen(public->symbol);
            if (length >= 9 &&
                strcmp(public->symbol + length - 9, "_figatree") == 0)
            {
                visit(public->address, visit_context);
            }
        }
    }
}

void msl_native_dat_finish_initialization(void)
{
    const size_t page_size = 4096;
    size_t protected_size;
    uint32_t i;

    if (native_dat_initialization_complete) {
        return;
    }
    for (i = 0; i < native_archive_cache_count; ++i) {
        // The runtime graph and byte streams live in the native arena. Make
        // original big-endian file buffers inaccessible so a leaked raw
        // pointer or per-frame archive read fails immediately.
        msl_memory_protect_allocation(native_archive_cache[i].source);
    }
    protected_size =
        (native_dat_arena_used + page_size - 1) & ~(page_size - 1);
#ifdef MSL_CORE_WASM
    // Wasm linear memory has no page-level read-only mapping. The logical seal
    // below still rejects every later DAT allocation/translation attempt.
    (void) protected_size;
#else
#if MSL_GX_RECORD
    // The viewer build keeps the arena writable: the game's refraction loader rewrites display lists in place
    // (refs/melee/src/melee/lb/lbrefract.c::lbRefract_PObjLoad), the same bytes every time.
    (void) protected_size;
    if (0)
#else
    if (protected_size != 0 &&
        mprotect(native_dat_arena, protected_size, PROT_READ) != 0)
#endif
    {
        perror("mprotect immutable native DAT arena");
        abort();
    }
#endif
    native_dat_initialization_complete = 1;
}

int msl_native_dat_owns(const void* pointer)
{
    uintptr_t address = (uintptr_t) pointer;
    uintptr_t begin = (uintptr_t) native_dat_arena;
    return native_dat_arena != NULL && address >= begin &&
           address - begin < native_dat_arena_used;
}

void msl_native_dat_context_destroy(MslNativeDatContext* context)
{
    if (context == NULL || context->arena == NULL) {
        return;
    }
#ifdef MSL_CORE_WASM
    free(context->arena);
#else
    munmap(context->arena, MSL_NATIVE_DAT_ARENA_BYTES);
#endif
    memset(context, 0, sizeof(*context));
}

static int contains_u32(const uint32_t* values, uint32_t count, uint32_t value)
{
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (values[mid] < value) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < count && values[lo] == value;
}

static uint32_t next_boundary(const MslNativeArchive* context,
                              uint32_t offset)
{
    uint32_t lo = 0;
    uint32_t hi = context->boundary_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (context->boundaries[mid] <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < context->boundary_count ? context->boundaries[lo]
                                        : context->data_size;
}

static MslNativeArchive* archive_context(HSD_Archive* archive)
{
    return (MslNativeArchive*) archive->top_ptr;
}

static uint32_t raw_public_offset(HSD_Archive* archive, const char* symbol)
{
    uint32_t i;
    for (i = 0; i < archive->header.nb_public; ++i) {
        if (strcmp(archive->symbols + archive->public_info[i].symbol, symbol) ==
            0)
        {
            return archive->public_info[i].offset;
        }
    }
    return UINT32_MAX;
}

static uint32_t raw_pointer(const MslNativeArchive* context,
                            uint32_t field_offset)
{
    if (field_offset + 4 > context->data_size ||
        !contains_u32(context->reloc_fields, context->reloc_count,
                      field_offset))
    {
        return UINT32_MAX;
    }
    return read_be32(context->data + field_offset);
}

static void translate_value(MslNativeArchive* context,
                            const MslDatType* type, uint32_t source_offset,
                            void* native);

static MslDatMemo* find_memo(MslNativeArchive* context, uint32_t source_offset)
{
    uint32_t i;
    for (i = 0; i < context->memo_count; ++i) {
        if (context->memo[i].source_offset == source_offset) {
            return &context->memo[i];
        }
    }
    return NULL;
}

static void add_memo(MslNativeArchive* context, uint32_t source_offset,
                     const MslDatType* type, void* native)
{
    if (context->memo_count == context->memo_capacity) {
        uint32_t capacity = context->memo_capacity == 0
                                ? 256
                                : context->memo_capacity * 2;
        MslDatMemo* replacement = native_alloc(
            (size_t) capacity * sizeof(*replacement));
        if (context->memo != NULL) {
            memcpy(replacement, context->memo,
                   (size_t) context->memo_count * sizeof(*replacement));
        }
        context->memo = replacement;
        context->memo_capacity = capacity;
    }
    context->memo[context->memo_count++] =
        (MslDatMemo) { source_offset, type, native };
}

static void* translate_command_address(MslNativeArchive* context,
                                       uint32_t source_offset,
                                       const MslDatType* type)
{
    uint32_t count;
    uint32_t i;

    if (type->source_size != 4 || type->native_size < sizeof(void*) ||
        source_offset % type->source_size != 0)
    {
        fprintf(stderr, "native DAT has an invalid command-word layout\n");
        abort();
    }
    count = (context->data_size + type->source_size - 1) / type->source_size;
    if (context->command_words == NULL) {
        context->command_words =
            native_alloc((size_t) count * type->native_size);
        context->command_word_type = type;

        // A fighter script may share a suffix with another script, so a
        // relocation target can lie inside a sequential command stream. A
        // boundary-sized translation would split that stream into unrelated
        // allocations once native pointers widen CmdUnion from four to eight
        // bytes. Mirror the archive's source-word address space once instead:
        // sequential cursor increments and every interior control-flow target
        // then retain the retail topology exactly.
        // refs/melee/src/melee/lb/{types.h,lbcommand.c}
        // refs/melee/src/melee/ft/ftaction.c::ftAction_80073240
        for (i = 0; i < count; ++i) {
            uint32_t offset = i * type->source_size;
            uint32_t remaining = context->data_size - offset;
            uint32_t size = remaining < type->source_size ? remaining
                                                          : type->source_size;
            memcpy(context->command_words + (size_t) i * type->native_size,
                   context->data + offset, size);
        }
        for (i = 0; i < context->reloc_count; ++i) {
            uint32_t field = context->reloc_fields[i];
            uint32_t target = read_be32(context->data + field);
            void* pointer;

            if (field % type->source_size != 0 ||
                target >= context->data_size ||
                target % type->source_size != 0)
            {
                continue;
            }
            pointer = context->command_words +
                      (size_t) (target / type->source_size) *
                          type->native_size;
            memcpy(context->command_words +
                       (size_t) (field / type->source_size) *
                           type->native_size,
                   &pointer, sizeof(pointer));
        }
    } else if (context->command_word_type != type) {
        fprintf(stderr, "native DAT command word requested with two types\n");
        abort();
    }
    return context->command_words +
           (size_t) (source_offset / type->source_size) * type->native_size;
}

static void* translate_target_count(MslNativeArchive* context,
                                    uint32_t source_offset,
                                    const MslDatType* type, uint32_t count)
{
    MslDatMemo* memo;
    uint8_t* result;
    uint32_t i;

    if (source_offset >= context->data_size) {
        fprintf(stderr, "native DAT pointer %x is outside %x-byte body\n",
                source_offset, context->data_size);
        abort();
    }
    if (strcmp(type->name, "CmdUnion") == 0) {
        return translate_command_address(context, source_offset, type);
    }
    memo = find_memo(context, source_offset);
    if (memo != NULL) {
        if (memo->type != type) {
            fprintf(stderr,
                    "native DAT offset %x requested as both %s and %s\n",
                    source_offset, memo->type->name, type->name);
            abort();
        }
        return memo->native;
    }
    if (strcmp(type->name, "ItemStateArray") == 0) {
        const MslDatType* array;
        const MslDatType* element;
        uint32_t span;

        if (type->field_count != 1 ||
            (array = type->fields[0].type)->kind != MSL_DAT_ARRAY ||
            (element = array->element)->source_size == 0)
        {
            fprintf(stderr, "native DAT ItemStateArray lacks an element view\n");
            abort();
        }
        span = next_boundary(context, source_offset) - source_offset;
        if (span < element->source_size) {
            fprintf(stderr,
                    "native DAT ItemStateArray has invalid %u-byte extent\n",
                    span);
            abort();
        }
        // The extent runs to the next referenced offset. PlKb.dat packs an
        // unreferenced 8-byte pair between the one-entry state table of
        // Kirby's Vanish article (x48_items[0]) and the article itself, so
        // the extent is 24 bytes for a 16-byte element; the tail past the
        // last whole element belongs to no state and is dropped.
        count = span / element->source_size;
        result = native_alloc((size_t) count * element->native_size);
        add_memo(context, source_offset, type, result);
        for (i = 0; i < count; ++i) {
            translate_value(context, element,
                            source_offset + i * element->source_size,
                            result + (size_t) i * element->native_size);
        }
        return result;
    }
    if (type->kind == MSL_DAT_VOID) {
        uint32_t end = next_boundary(context, source_offset);
        size_t size = end > source_offset ? end - source_offset : 1;
        size_t held = size;
#if MSL_GX_RECORD
        // The console holds the whole file in one block of memory: a palette read past its own entries (the game loads 256
        // entries for any 8-bit image) reads the file's next bytes, which the viewer build must hold too. A small block
        // keeps up to 2 KiB of what follows it in the file.
        if (size < 2048 && source_offset < context->data_size) {
            held = context->data_size - source_offset;
            if (held > 2048) {
                held = 2048;
            }
            if (held < size) {
                held = size;
            }
        }
#endif
        result = native_alloc(held);
        add_memo(context, source_offset, type, result);
        memcpy(result, context->data + source_offset, held <= context->data_size - source_offset ? held : size);
#if MSL_GX_RECORD
        // the file's pointer fields in the bytes that follow are the console's relocated addresses
        if (held > size && source_offset + held <= context->data_size) {
            uint32_t k;
            for (k = 0; k < context->reloc_count; ++k) {
                uint32_t field = context->reloc_fields[k];
                if (field >= source_offset + size && field + 4 <= source_offset + held) {
                    uint32_t target = read_be32(context->data + field);
                    uint8_t* at = (uint8_t*) result + (field - source_offset);
                    uint32_t value = context->console_base + target;
                    at[0] = (uint8_t) (value >> 24);
                    at[1] = (uint8_t) (value >> 16);
                    at[2] = (uint8_t) (value >> 8);
                    at[3] = (uint8_t) value;
                }
            }
        }
#endif
        return result;
    }
    if (count == 0) {
        count = 1;
    }
    if (type->native_size == 0 ||
        (size_t) count > SIZE_MAX / type->native_size)
    {
        fprintf(stderr, "invalid native DAT extent for %s\n", type->name);
        abort();
    }
    if (strcmp(type->name, "HSD_Joint") == 0) {
        HSD_ASSERT(470, count == 1);
        result = native_alloc(type->native_size + sizeof(Quaternion));
    } else {
        result = native_alloc((size_t) count * type->native_size);
    }
    add_memo(context, source_offset, type, result);
    for (i = 0; i < count; ++i) {
        translate_value(context, type,
                        source_offset + i * type->source_size,
                        result + (size_t) i * type->native_size);
    }
    return result;
}

static void* translate_target(MslNativeArchive* context,
                              uint32_t source_offset,
                              const MslDatType* type)
{
#if MSL_GX_RECORD
    // UnkStageDat.unk28 is declared UnkStageDatInternal**: its elements are materials the stage's joints use too
    // (grDatFiles_801C6228 sets a render flag in the word at +4, an MObjDesc's rendermode). The viewer build draws, so
    // it holds the whole descriptor.
    if (type->kind == MSL_DAT_STRUCT &&
        strcmp(type->name, "UnkStageDatInternal") == 0)
    {
        type = msl_dat_root_HSD_MObjDesc;
    }
#endif
    uint32_t span = next_boundary(context, source_offset) - source_offset;
    uint32_t count = 1;
    int graph_node =
        type->kind == MSL_DAT_STRUCT &&
        (strncmp(type->name, "HSD_", 4) == 0 ||
         strncmp(type->name, "_HSD_", 5) == 0);
#if MSL_GX_RECORD
    // A model's vertex descriptor list, its envelope weights and the list of those are arrays ended by an entry of their
    // own (an attribute of 0xFF, a joint of 0, a pointer of 0); the next relocation target may lie inside them (the
    // ending entry is shared), so they are counted by their ending.
    // refs/melee/src/sysdolphin/baselib/pobj.c::{setupVtxDesc,loadEnvelopeDesc}
    if (strcmp(type->name, "HSD_VtxDescList") == 0 ||
        strcmp(type->name, "HSD_EnvelopeDesc") == 0 ||
        (type->kind == MSL_DAT_POINTER && type->source_size == 4 &&
         type->element != NULL && type->element->kind == MSL_DAT_STRUCT &&
         strcmp(type->element->name, "HSD_EnvelopeDesc") == 0))
    {
        uint32_t size = type->source_size;
        uint32_t k;
        for (k = 0; source_offset + (k + 1) * size <= context->data_size; ++k) {
            uint32_t word = read_be32(context->data + source_offset + k * size);
            int ends = type->kind == MSL_DAT_POINTER
                           ? word == 0
                           : strcmp(type->name, "HSD_VtxDescList") == 0
                                 ? word == 0xFF
                                 : word == 0;
            if (ends) {
                return translate_target_count(context, source_offset, type,
                                              k + 1);
            }
        }
    }
#endif
    // An Article is always a single object referenced slot-by-slot from
    // ftData.x48_items; no source consumer indexes an Article array through
    // one pointer. PlLk.dat packs an unreferenced hookshot joint blob
    // between its boomerang Article and the next relocation target, so the
    // span heuristic would materialize that residue as a second Article and
    // mistype the joints it points at.
    // refs/melee/src/melee/it/types.h::Article
    int single_object =
        type->kind == MSL_DAT_STRUCT && strcmp(type->name, "Article") == 0;
    if (!graph_node && !single_object && type->source_size != 0 &&
        span >= type->source_size && span % type->source_size == 0)
    {
        count = span / type->source_size;
    }
    return translate_target_count(context, source_offset, type, count);
}

static void translate_stage_animation_array(MslNativeArchive* context,
                                             uint32_t offset,
                                             const MslDatType* type)
{
    uint32_t count = 1;
    uint32_t i;
    uint8_t* result;

    if (find_memo(context, offset) != NULL) {
        return;
    }
    // Stage animation descriptors are packed node arrays: grAnime_801C7C1C
    // indexes by part before following child/next links. The linked extent
    // supplies the array bound; the three source node types start with those
    // two pointers. GALE01 0x801C7CB8/7D04/7D4C.
    for (i = 0; i < count; ++i) {
        uint32_t source = offset + i * type->source_size;
        uint32_t field;
        if (source > context->data_size ||
            type->source_size > context->data_size - source)
        {
            fprintf(stderr, "stage animation array exceeds its archive\n");
            abort();
        }
        for (field = 0; field < 8; field += 4) {
            uint32_t target = raw_pointer(context, source + field);
            if (target != UINT32_MAX) {
                uint32_t extent;
                if (target < offset ||
                    (target - offset) % type->source_size != 0)
                {
                    fprintf(stderr, "stage animation array is not packed\n");
                    abort();
                }
                extent = (target - offset) / type->source_size + 1;
                if (extent > count) {
                    count = extent;
                }
            }
        }
    }
    result = native_alloc((size_t) count * type->native_size);
    // Register the whole address range first: graph edges and direct part
    // indexing must resolve to the same interior nodes.
    for (i = 0; i < count; ++i) {
        add_memo(context, offset + i * type->source_size, type,
                  result + (size_t) i * type->native_size);
    }
    for (i = 0; i < count; ++i) {
        translate_value(context, type, offset + i * type->source_size,
                        result + (size_t) i * type->native_size);
    }
}

static void translate_stage_animation_tables(MslNativeArchive* context,
                                              uint32_t map_head)
{
    const MslDatType* types[] = { msl_dat_root_HSD_AnimJoint,
                                 msl_dat_root_HSD_MatAnimJoint,
                                 msl_dat_root_HSD_ShapeAnimJoint };
    uint32_t maps = raw_pointer(context, map_head + 8);
    uint32_t count = read_be32(context->data + map_head + 12);
    uint32_t i;
    uint32_t kind;

    // UnkStageDat.unk8/unkC and UnkStageDat_x8_t's three animation tables.
    if (maps == UINT32_MAX || maps > context->data_size ||
        count > (context->data_size - maps) / 0x34)
    {
        fprintf(stderr, "stage map animation table is outside its archive\n");
        abort();
    }
    for (i = 0; i < count; ++i) {
        for (kind = 0; kind < 3; ++kind) {
            uint32_t table = raw_pointer(context, maps + i * 0x34 +
                                                       4 + kind * 4);
            uint32_t end;
            uint32_t cursor;
            if (table == UINT32_MAX) {
                continue;
            }
            end = next_boundary(context, table);
            for (cursor = table; cursor + 4 <= end; cursor += 4) {
                uint32_t animation = raw_pointer(context, cursor);
                if (animation != UINT32_MAX) {
                    translate_stage_animation_array(context, animation,
                                                       types[kind]);
                }
            }
        }
    }
}

static uint64_t read_unsigned(const uint8_t* source, uint32_t size)
{
    switch (size) {
    case 1: return source[0];
    case 2: return read_be16(source);
    case 4: return read_be32(source);
    case 8: return read_be64(source);
    default:
        fprintf(stderr, "unsupported native DAT scalar width %u\n", size);
        abort();
    }
}

static void write_integer(void* native, uint32_t size, uint64_t value)
{
    switch (size) {
    case 1: *(uint8_t*) native = (uint8_t) value; break;
    case 2: *(uint16_t*) native = (uint16_t) value; break;
    case 4: *(uint32_t*) native = (uint32_t) value; break;
    case 8: *(uint64_t*) native = value; break;
    default:
        fprintf(stderr, "unsupported native integer width %u\n", size);
        abort();
    }
}

static void translate_base(const MslDatType* type, const uint8_t* source,
                           void* native)
{
    uint64_t value = read_unsigned(source, type->source_size);
    if (type->base_encoding == DW_ATE_FLOAT) {
        if (type->source_size != type->native_size) {
            fprintf(stderr, "native DAT float width changed for %s\n",
                    type->name);
            abort();
        }
        write_integer(native, type->native_size, value);
        return;
    }
    if ((type->base_encoding == DW_ATE_SIGNED ||
         type->base_encoding == DW_ATE_SIGNED_CHAR) &&
        type->source_size < 8)
    {
        uint32_t bits = type->source_size * 8;
        value = (uint64_t) ((int64_t) (value << (64 - bits)) >> (64 - bits));
    }
    write_integer(native, type->native_size, value);
}

static void translate_bitfield(const MslDatType* type,
                               const uint8_t* source, uint8_t* native)
{
    uint64_t value = 0;
    uint32_t i;
    for (i = 0; i < type->bit_size; ++i) {
        uint32_t bit = type->source_bit_offset + i;
        value = value << 1 | ((source[bit / 8] >> (7 - bit % 8)) & 1U);
    }
    for (i = 0; i < type->bit_size; ++i) {
        uint32_t bit = type->native_bit_offset + i;
        uint8_t mask = (uint8_t) (1U << (bit % 8));
        if ((value >> i) & 1U) {
            native[bit / 8] |= mask;
        } else {
            native[bit / 8] &= (uint8_t) ~mask;
        }
    }
}

static void translate_value(MslNativeArchive* context,
                            const MslDatType* type, uint32_t source_offset,
                            void* native)
{
    uint32_t i;
    if (source_offset + type->source_size > context->data_size &&
        type->kind != MSL_DAT_VOID)
    {
        fprintf(stderr, "native DAT %s at %x exceeds body\n", type->name,
                source_offset);
        abort();
    }
    switch (type->kind) {
    case MSL_DAT_VOID: return;
    case MSL_DAT_BASE:
        translate_base(type, context->data + source_offset, native);
        return;
    case MSL_DAT_STRUCT:
        if (strcmp(type->name, "HSD_Joint") == 0) {
            uint32_t flags = read_be32(context->data + source_offset + 0x04);
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                translate_value(context, field->type,
                                source_offset + field->source_offset,
                                (uint8_t*) native + field->native_offset);
            }
            if ((flags & JOBJ_SPLINE) != 0) {
                uint32_t target = raw_pointer(context, source_offset + 0x10);
                void* spline = NULL;
                if (target != UINT32_MAX) {
                    spline = translate_target(context, target,
                                              msl_dat_root_HSD_Spline);
                }
                memcpy((uint8_t*) native + offsetof(HSD_Joint, u), &spline,
                       sizeof(spline));
            }
#if MSL_GX_RECORD
            // The viewer build draws: the DObj descriptors a joint's union points at are translated too (the generator
            // leaves the union out of the joint's fields).
            // refs/melee/src/sysdolphin/baselib/jobj.c::JObjLoadCore
            if ((flags & (JOBJ_SPLINE | JOBJ_PTCL)) == 0) {
                uint32_t target = raw_pointer(context, source_offset + 0x10);
                void* dobj = NULL;
                if (target != UINT32_MAX) {
                    dobj = translate_target(context, target,
                                            msl_dat_root_HSD_DObjDesc);
                }
                memcpy((uint8_t*) native + offsetof(HSD_Joint, u), &dobj,
                       sizeof(dobj));
            }
#endif
            {
                HSD_Joint* joint = native;
                Vec3 rotation = joint->rotation;
                EulerToQuat(&rotation,
                            (Quaternion*) ((uint8_t*) native +
                                           type->native_size));
            }
            // The shared DWARF view omits HSD_Joint's anonymous union. Spline
            // is gameplay-bearing through HSD_A_J_PATH; DObj and particle
            // variants remain presentation-only in this headless graph.
            // refs/melee/src/sysdolphin/baselib/{jobj.h,jobj.c::JObjUpdateFunc}
            return;
        }
#if MSL_GX_RECORD
        if (strcmp(type->name, "itGamewatchparachuteAttributes") == 0 ||
            strcmp(type->name, "MslDatGameWatchChefAttrs") == 0 ||
            strcmp(type->name, "MslDatKirbyChefPanAttrs") == 0) {
            // Every Game & Watch article's first special attribute is a pointer (declared void*, so copied raw) to the record the draw reads
            // through it_266F_ItemVars: {u16 count, u8* indices, u16 count, u8* indices} (the dynamic bones it hides while the flat pass draws).
            // The viewer build draws, so the record is translated into the host's layout of that struct (u16 at +0, pointer at +8, u16 at +16,
            // pointer at +24).
            // refs/melee/src/melee/it/itdraw.c::{it_8026EECC,it_8026EBC8,it_8026EC54}, items/itgamewatch*.c, it_2725.c::it_8027CE64
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                uint32_t field_source = source_offset + field->source_offset;
                if (field->source_offset == 0) {
                    uint32_t target = raw_pointer(context, field_source);
                    void* record = NULL;
                    if (target != UINT32_MAX && target + 16 <= context->data_size) {
                        /* (the host's layout of the record: u16, pointer, u16, pointer) */
                        struct MslBoneRecord { uint16_t count0; uint8_t* indices0; uint16_t count1; uint8_t* indices1; };
                        struct MslBoneRecord* host = native_alloc(sizeof(struct MslBoneRecord));
                        int k;
                        for (k = 0; k < 2; ++k) {
                            uint32_t count_at = target + 8 * k;
                            uint32_t table = raw_pointer(context, count_at + 4);
                            uint16_t count = (uint16_t) read_be16(context->data + count_at);
                            uint8_t* indices = NULL;
                            if (table != UINT32_MAX && table + count <= context->data_size) {
                                indices = native_alloc(count ? count : 1);
                                memcpy(indices, context->data + table, count);
                            }
                            if (k == 0) {
                                host->count0 = count;
                                host->indices0 = indices;
                            } else {
                                host->count1 = count;
                                host->indices1 = indices;
                            }
                        }
                        record = host;
                    }
                    memcpy((uint8_t*) native + field->native_offset, &record, sizeof(record));
                    continue;
                }
                translate_value(context, field->type, field_source, (uint8_t*) native + field->native_offset);
            }
            return;
        }
        if (strcmp(type->name, "FtPartsDesc") == 0 || strncmp(type->name, "MslDatKirbyHat", 14) == 0) {
            // vis_table is a void* (*)[4]: a table of [costume][4] pointers to FtPartsVisLookup arrays (the hide-part
            // lists of each model, ftParts_8007487C). The viewer build hides parts, so the lookups are translated; the
            // table's extent runs to the next referenced offset. A FtPartsDesc has it at +4; a Kirby hat's record at +8
            // (KirbyHatStruct), except the five hats with per-costume models (Donkey Kong, Jigglypuff, Mewtwo, Falco,
            // Game & Watch): their record starts with the parts desc itself (+4), which the code reads through a
            // FtPartsDesc cast, i.e. at the host offset 8 (a u32 model_num, then a pointer).
            // refs/melee/src/melee/ft/ftparts.c::{ftParts_8007487C,ftParts_80074D7C},
            // ftKirby/ftkirby.c::ftKb_SpecialN_800F11F0
            int own_desc = strncmp(type->name, "MslDatKirbyHat", 14) == 0 &&
                           (strcmp(type->name + 14, "Donkey") == 0 || strcmp(type->name + 14, "Purin") == 0 ||
                            strcmp(type->name + 14, "Mewtwo") == 0 || strcmp(type->name + 14, "Falco") == 0 ||
                            strcmp(type->name + 14, "Gamewatch") == 0);
            uint32_t vis_at = type->name[0] == 'F' || own_desc ? 4 : 8;
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                uint32_t field_source = source_offset + field->source_offset;
                if (field->source_offset == vis_at &&
                    (field->type->kind == MSL_DAT_POINTER || field->type->kind == MSL_DAT_BASE)) {
                    uint32_t target = raw_pointer(context, field_source);
                    void* table = NULL;
                    if (target != UINT32_MAX && target % 4 == 0) {
                        uint32_t words = (next_boundary(context, target) - target) / 4;
                        uint32_t k;
                        void** native_table;
                        words -= words % 4;
                        native_table = native_alloc((size_t) (words ? words : 4) * sizeof(void*));
                        for (k = 0; k < words; ++k) {
                            uint32_t entry = raw_pointer(context, target + k * 4);
                            native_table[k] = entry == UINT32_MAX
                                                  ? NULL
                                                  : translate_target(context, entry,
                                                                     msl_dat_root_FtPartsVisLookup);
                        }
                        table = native_table;
                    }
                    if (own_desc) {
                        translate_value(context, field->type, field_source,
                                        (uint8_t*) native + field->native_offset);
                        {
                            /* (the host's FtPartsDesc: a u32 model_num, then the pointer) */
                            struct { uint32_t model_num; void* vis_table; } host_desc;
                            memcpy((uint8_t*) native + ((uint8_t*) &host_desc.vis_table - (uint8_t*) &host_desc), &table, sizeof(table));
                        }
                    } else {
                        memcpy((uint8_t*) native + field->native_offset, &table, sizeof(table));
                    }
                    continue;
                }
                translate_value(context, field->type, field_source, (uint8_t*) native + field->native_offset);
            }
            return;
        }
        if (strcmp(type->name, "HSD_PObjDesc") == 0) {
            // The union at the end holds, by the pobj's type bits, the joint it is bound to, its shape set or its envelope
            // list.
            // refs/melee/src/sysdolphin/baselib/pobj.c::{PObjLoad,HSD_PObjResolveRefs}
            uint32_t flags = read_be16(context->data + source_offset + 0x0C);
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                uint32_t field_source = source_offset + field->source_offset;
                if (field->type->kind == MSL_DAT_UNION) {
                    uint32_t target = raw_pointer(context, field_source);
                    void* pointer = NULL;
                    if (target != UINT32_MAX) {
                        switch (flags & 0x3000) {
                        case 0x1000:
                            pointer = translate_target(
                                context, target,
                                msl_dat_root_HSD_ShapeSetDesc);
                            break;
                        case 0x2000:
                            pointer = translate_target(
                                context, target,
                                msl_dat_root_MslDatEnvelopeList);
                            break;
                        default:
                            pointer = translate_target(
                                context, target, msl_dat_root_HSD_Joint);
                            break;
                        }
                    }
                    memcpy((uint8_t*) native + field->native_offset, &pointer,
                           sizeof(pointer));
                    continue;
                }
                translate_value(context, field->type, field_source,
                                (uint8_t*) native + field->native_offset);
            }
            return;
        }
#endif
        if (strcmp(type->name, "HSD_AObjDesc") == 0) {
            // AObjDesc::obj_id is declared u32 because retail uses either an
            // object-table id or a relocated HSD_Joint address in the same
            // word. When the DAT relocation table marks it, materialize the
            // Joint graph and retain its deterministic native-DAT offset id —
            // the same source-width identity used by the hosted HSD table.
            // refs/melee/src/sysdolphin/baselib/aobj.c::HSD_AObjLoadDesc
            // refs/melee/src/sysdolphin/baselib/jobj.c::HSD_JObjLoadJoint
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                uint32_t field_source = source_offset + field->source_offset;
                if (field->source_offset == 0x0C) {
                    uint32_t target = raw_pointer(context, field_source);
                    if (target != UINT32_MAX) {
                        void* joint = translate_target(
                            context, target, msl_dat_root_HSD_Joint);
                        write_integer((uint8_t*) native +
                                          field->native_offset,
                                      field->type->native_size,
                                      msl_hsd_id_from_pointer(joint));
                        continue;
                    }
                }
                translate_value(context, field->type, field_source,
                                (uint8_t*) native + field->native_offset);
            }
            return;
        }
        if (strcmp(type->name, "ftDynamics") == 0) {
            // ftDynamics::x10 is declared FigaTree***, but the archive's
            // table holds, per animation blend slot, a pointer to plain
            // 32-bit counts: how many leading chain joints the animation
            // drives. ftCo_8009E4A8 and ftCo_8009E7B4 pass tree[i] to
            // ftCo_8009CB40, which compares it as (s32) arg3 against the
            // chain index. The counts are not relocated, so following the
            // declared pointer type turned every one into NULL (0): each
            // chain joint was then left to the physics. Keep the declared
            // host shape (pointer-sized slots) and carry each count in its
            // slot, as the PPC word it is; a slot without a relocation stays
            // NULL, as ftCo_8009E7B4's checks expect.
            // refs/melee/src/melee/ft/ftdynamics.c::{ftCo_8009CB40,
            //   ftCo_8009E4A8,ftCo_8009E7B4}
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                uint32_t field_source = source_offset + field->source_offset;
                if (field->source_offset == 0x10 &&
                    field->type->kind == MSL_DAT_POINTER)
                {
                    uint32_t table = raw_pointer(context, field_source);
                    void** slots = NULL;
                    if (table != UINT32_MAX) {
                        uint32_t slot_count =
                            (next_boundary(context, table) - table) / 4;
                        uint32_t s;
                        if (slot_count == 0) {
                            slot_count = 1;
                        }
                        slots = native_alloc((size_t) slot_count *
                                             sizeof(*slots));
                        for (s = 0; s < slot_count; ++s) {
                            uint32_t counts =
                                raw_pointer(context, table + s * 4);
                            uintptr_t* words;
                            uint32_t n;
                            uint32_t k;
                            if (counts == UINT32_MAX) {
                                continue;
                            }
                            n = (next_boundary(context, counts) - counts) / 4;
                            if (n == 0) {
                                n = 1;
                            }
                            words = native_alloc((size_t) n * sizeof(*words));
                            for (k = 0; k < n; ++k) {
                                words[k] = read_be32(context->data + counts +
                                                     k * 4);
                            }
                            slots[s] = words;
                        }
                    }
                    memcpy((uint8_t*) native + field->native_offset, &slots,
                           sizeof(slots));
                    continue;
                }
                translate_value(context, field->type, field_source,
                                (uint8_t*) native + field->native_offset);
            }
            return;
        }
        if (strcmp(type->name, "DynamicsDesc") == 0) {
            const MslDatType* source_element = NULL;
            uint32_t count;

            // Archive DynamicsDesc::data points at a packed array of
            // lb_00F9_UnkDesc1Inner records.  Its declared DynamicsData* type
            // is the runtime view used after lb_8000FD48 allocates a linked
            // list; lb_80011710 deliberately recovers the packed source view
            // and indexes it through the archive-owned count.  Translating a
            // widened DynamicsData would split that packed array at the PPC
            // union/pointer boundary, so materialize the actual source view.
            // refs/melee/src/melee/lb/lbspdisplay.c::{lb_8000FD48,lb_80011710}
            count = read_be32(context->data + source_offset +
                              type->fields[1].source_offset);
            for (i = 0; i < type->field_count; ++i) {
                const MslDatField* field = &type->fields[i];
                if (field->type->kind == MSL_DAT_POINTER) {
                    const MslDatType* dynamics_data = field->type->element;
                    uint32_t j;
                    for (j = 0; j < dynamics_data->field_count; ++j) {
                        const MslDatType* candidate =
                            dynamics_data->fields[j].type;
                        uint32_t k;
                        if (strcmp(candidate->name, "PolymorphicDesc") != 0) {
                            continue;
                        }
                        for (k = 0; k < candidate->field_count; ++k) {
                            const MslDatType* view = candidate->fields[k].type;
                            if (strcmp(view->name, "lb_00F9_UnkDesc1") == 0 &&
                                view->field_count == 1 &&
                                view->fields[0].type->kind == MSL_DAT_ARRAY)
                            {
                                source_element =
                                    view->fields[0].type->element;
                                break;
                            }
                        }
                    }
                    if (source_element == NULL) {
                        fprintf(stderr,
                                "native DAT DynamicsDesc lacks source view\n");
                        abort();
                    }
                    {
                        uint32_t target = raw_pointer(
                            context, source_offset + field->source_offset);
                        void* pointer = NULL;
                        if (target != UINT32_MAX) {
                            pointer = translate_target_count(
                                context, target, source_element, count);
                        }
                        memcpy((uint8_t*) native + field->native_offset,
                               &pointer, sizeof(pointer));
                    }
                } else {
                    translate_value(context, field->type,
                                    source_offset + field->source_offset,
                                    (uint8_t*) native + field->native_offset);
                }
            }
            return;
        }
        for (i = 0; i < type->field_count; ++i) {
            const MslDatField* field = &type->fields[i];
            translate_value(context, field->type,
                            source_offset + field->source_offset,
                            (uint8_t*) native + field->native_offset);
        }
        return;
    case MSL_DAT_UNION:
        if (strcmp(type->name, "CmdUnion") == 0 ||
            strcmp(type->name, "ColorOverlay_x8_t") == 0)
        {
            uint32_t target = raw_pointer(context, source_offset);

            // Action and color scripts are streams of overlapping PPC
            // bitfield views.
            // Preserve each source word's big-endian storage so every command
            // view observes the same bits under the native
            // scalar-storage-order declarations. Relocation words used by
            // Subroutine/Goto instead become native command-stream pointers in
            // the widened union slot.
            // refs/melee/src/melee/lb/{types.h,lbcommand.c}
            if (target != UINT32_MAX) {
                void* pointer = translate_target(context, target, type);
                memcpy(native, &pointer, sizeof(pointer));
            } else {
                memcpy(native, context->data + source_offset,
                       type->source_size);
            }
            return;
        }
#if MSL_GX_RECORD
        if (strcmp(type->name, "HSD_CObjDesc") == 0) {
            // A camera descriptor is read through whichever member the projection type names; they share the leading fields and
            // differ in the last four words. The whole of the largest is translated.
            // refs/melee/src/sysdolphin/baselib/cobj.c::CObjLoad
            const MslDatField* widest = NULL;
            uint32_t k;
            for (k = 0; k < type->field_count; ++k) {
                if (widest == NULL || type->fields[k].type->source_size > widest->type->source_size) {
                    widest = &type->fields[k];
                }
            }
            if (widest != NULL) {
                translate_value(context, widest->type, source_offset + widest->source_offset,
                                (uint8_t*) native + widest->native_offset);
            }
            return;
        }
#endif
        if (type->field_count != 0) {
            const MslDatField* field = &type->fields[0];
            translate_value(context, field->type,
                            source_offset + field->source_offset,
                            (uint8_t*) native + field->native_offset);
        }
        return;
    case MSL_DAT_POINTER: {
        uint32_t target = raw_pointer(context, source_offset);
        void* pointer = NULL;
        if (target != UINT32_MAX) {
            if (type->element->kind == MSL_DAT_FUNCTION) {
                fprintf(stderr,
                        "native DAT contains a non-null function pointer at %x\n",
                        source_offset);
                abort();
            }
            pointer = translate_target(context, target, type->element);
        }
        memcpy(native, &pointer, sizeof(pointer));
        return;
    }
    case MSL_DAT_ARRAY:
    {
        uint32_t count = type->count;
        uint32_t boundary = next_boundary(context, source_offset);
        uint32_t span = boundary - source_offset;

        // Several DAT declarations are fixed-capacity runtime containers whose
        // archive instance stores only the populated prefix (notably
        // ItemStateArray). The next relocation target is the next packed
        // object; leave the native capacity's unused tail zero-initialized.
        if (type->element->source_size != 0 &&
            span / type->element->source_size < count)
        {
            count = span / type->element->source_size;
        }
        for (i = 0; i < count; ++i) {
            translate_value(context, type->element,
                            source_offset + i * type->element->source_size,
                            (uint8_t*) native +
                                (size_t) i * type->element->native_size);
        }
        return;
    }
    case MSL_DAT_FUNCTION:
        return;
    case MSL_DAT_BITFIELD:
        translate_bitfield(type, context->data + source_offset,
                           (uint8_t*) native);
        return;
    }
}

int msl_native_archive_parse(HSD_Archive* archive, uint8_t* source,
                             size_t file_size)
{
    MslNativeArchive* context;
    uint32_t offset;
    uint32_t i;
    uint32_t boundaries_capacity;

    for (i = 0; i < native_archive_cache_count; ++i) {
        MslNativeArchiveCacheEntry* entry = &native_archive_cache[i];
        if (entry->source == source && entry->file_size == file_size) {
            *archive = entry->archive;
            return 0;
        }
    }
    require_dat_initialization("uncached native archive parse");

    if (archive == NULL || source == NULL || file_size < 0x20) {
        fprintf(stderr, "native DAT invalid parse arguments\n");
        return -1;
    }
    memset(archive, 0, sizeof(*archive));
    archive->header.file_size = read_be32(source + 0x00);
    archive->header.data_size = read_be32(source + 0x04);
    archive->header.nb_reloc = read_be32(source + 0x08);
    archive->header.nb_public = read_be32(source + 0x0C);
    archive->header.nb_extern = read_be32(source + 0x10);
    memcpy(archive->header.version, source + 0x14,
           sizeof(archive->header.version));
    if (archive->header.file_size != file_size ||
        archive->header.data_size > file_size - 0x20)
    {
        fprintf(stderr,
                "native DAT invalid header: file=%u/%zu data=%u\n",
                archive->header.file_size, file_size,
                archive->header.data_size);
        return -1;
    }
    archive->flags = HSD_ARCHIVE_DONT_FREE;
    archive->data = source + 0x20;
    offset = 0x20 + archive->header.data_size;

    context = native_alloc(sizeof(*context));
    context->archive = archive;
    context->source = source;
    context->data = archive->data;
    context->data_size = archive->header.data_size;
    context->reloc_count = archive->header.nb_reloc;
#if MSL_GX_RECORD
    {
        // The console relocates a file in place: each pointer field holds the file's base address plus its target. The base is
        // wherever the game's heap put the file, which no input here determines; files are placed one after the other from a
        // fixed start (MSL_DAT_BASE sets one base for every file, to test against a console capture).
        static uint32_t next_console_base = 0x80300000u;
        const char* forced = getenv("MSL_DAT_BASE");
        context->console_base = forced != NULL ? (uint32_t) strtoul(forced, NULL, 0) : next_console_base;
        next_console_base += (archive->header.data_size + 31u) & ~31u;
    }
#endif
    context->reloc_fields = native_alloc(
        (size_t) context->reloc_count * sizeof(*context->reloc_fields));
    boundaries_capacity = context->reloc_count + archive->header.nb_public + 1;
    context->boundaries = native_alloc(
        (size_t) boundaries_capacity * sizeof(*context->boundaries));

    for (i = 0; i < context->reloc_count; ++i) {
        uint32_t field = read_be32(source + offset + i * 4);
        uint32_t target;
        if (field + 4 > context->data_size) {
            fprintf(stderr, "native DAT relocation %u field %x out of body %x\n",
                    i, field, context->data_size);
            return -1;
        }
        context->reloc_fields[i] = field;
        target = read_be32(context->data + field);
        if (target < context->data_size) {
            context->boundaries[context->boundary_count++] = target;
        }
    }
    qsort(context->reloc_fields, context->reloc_count, sizeof(uint32_t),
          compare_u32);
    offset += archive->header.nb_reloc * 4;

    archive->public_info = native_alloc(
        (size_t) archive->header.nb_public * sizeof(*archive->public_info));
    for (i = 0; i < archive->header.nb_public; ++i) {
        archive->public_info[i].offset = read_be32(source + offset + i * 8);
        archive->public_info[i].symbol =
            read_be32(source + offset + i * 8 + 4);
        if (archive->public_info[i].offset < context->data_size) {
            context->boundaries[context->boundary_count++] =
                archive->public_info[i].offset;
        }
    }
    offset += archive->header.nb_public * 8;

    archive->extern_info = native_alloc(
        (size_t) archive->header.nb_extern * sizeof(*archive->extern_info));
    for (i = 0; i < archive->header.nb_extern; ++i) {
        archive->extern_info[i].offset = read_be32(source + offset + i * 8);
        archive->extern_info[i].symbol =
            read_be32(source + offset + i * 8 + 4);
    }
    offset += archive->header.nb_extern * 8;
    if (offset > file_size) {
        fprintf(stderr, "native DAT metadata end %x exceeds file %zx\n", offset,
                file_size);
        return -1;
    }
    archive->symbols = (char*) source + offset;
    context->boundaries[context->boundary_count++] = context->data_size;
    qsort(context->boundaries, context->boundary_count, sizeof(uint32_t),
          compare_u32);
    archive->top_ptr = context;
    if (native_archive_cache_count == MSL_NATIVE_ARCHIVE_CACHE_CAPACITY) {
        fprintf(stderr, "native DAT archive cache exhausted\n");
        abort();
    }
    native_archive_cache[native_archive_cache_count].source = source;
    native_archive_cache[native_archive_cache_count].file_size = file_size;
    native_archive_cache[native_archive_cache_count].archive = *archive;
    native_archive_cache_count += 1;
    return 0;
}

// Special attributes of the articles stored in Kirby's copy roots. The
// copied projectiles run the original fighters' item logic, which reads the
// same attribute blocks the fighter profiles above attach; Article.x4 is
// void in the decomp, so attach them here by hat slot.
// refs/melee/src/melee/ft/chara/ftKirby/ftkirby.c::ftKb_SpecialN_800F16D0
static const struct {
    const char* symbol;
    uint8_t slot;
    const MslDatType* const* type;
} msl_kirby_copy_article_attrs[] = {
    { "ftDataKirbyCopyMario", 0, &msl_dat_root_itUnkAttributes },
    { "ftDataKirbyCopyLuigi", 0, &msl_dat_root_itUnkAttributes },
    { "ftDataKirbyCopyDrmario", 0, &msl_dat_root_itUnkAttributes },
    { "ftDataKirbyCopyPopo", 0, &msl_dat_root_itClimbersIceAttributes },
    { "ftDataKirbyCopyPeach", 1, &msl_dat_root_itPeachToadSporeAttributes },
    { "ftDataKirbyCopyFox", 0, &msl_dat_root_FoxLaserAttr },
    { "ftDataKirbyCopyFox", 1, &msl_dat_root_FoxBlasterAttr },
    { "ftDataKirbyCopyFalco", 3, &msl_dat_root_FoxLaserAttr },
    { "ftDataKirbyCopyFalco", 4, &msl_dat_root_FoxBlasterAttr },
    { "ftDataKirbyCopyLink", 0, &msl_dat_root_itLinkArrowAttributes },
    { "ftDataKirbyCopyClink", 0, &msl_dat_root_itLinkArrowAttributes },
    { "ftDataKirbyCopyMewtwo", 3, &msl_dat_root_itMewtwoShadowball_DatAttrs },
    { "ftDataKirbyCopyNess", 0, &msl_dat_root_itFlashAttributes },
    { "ftDataKirbyCopyNess", 1, &msl_dat_root_itFlashExplAttributes },
    { "ftDataKirbyCopyPikachu", 0, &msl_dat_root_itPikachutJoltGroundAttributes },
    { "ftDataKirbyCopyPichu", 0, &msl_dat_root_itPikachutJoltGroundAttributes },
    { "ftDataKirbyCopySamus", 0, &msl_dat_root_itSamusChargeShot_Attributes },
    { "ftDataKirbyCopyKoopa", 0, &msl_dat_root_itKoopaFlame_Attributes },
    { "ftDataKirbyCopySeak", 0, &msl_dat_root_itSeakNeedleThrownAttributes },
    { "ftDataKirbyCopyGamewatch", 5, &msl_dat_root_MslDatGameWatchChefAttrs },
    { "ftDataKirbyCopyGamewatch", 6, &msl_dat_root_MslDatKirbyChefPanAttrs },
};

static void msl_translate_kirby_copy_article_attrs(MslNativeArchive* context,
                                               const char* symbol,
                                               uint32_t root, void* native)
{
    enum { MSL_HAT_SLOTS_SOURCE_OFFSET = 0x0C, MSL_ARTICLE_ATTRS_SOURCE_OFFSET = 0x04 };
    Article** slots = (Article**) ((uint8_t*) native +
                                   offsetof(struct KirbyHatStruct, hat_dynamics));
    size_t i;
    for (i = 0; i < sizeof(msl_kirby_copy_article_attrs) /
                        sizeof(msl_kirby_copy_article_attrs[0]);
         ++i)
    {
        uint32_t slot = msl_kirby_copy_article_attrs[i].slot;
        uint32_t article;
        uint32_t attrs;
        if (strcmp(msl_kirby_copy_article_attrs[i].symbol, symbol) != 0) {
            continue;
        }
        article = raw_pointer(context, root + MSL_HAT_SLOTS_SOURCE_OFFSET + slot * 4);
        if (article == UINT32_MAX || slots[slot] == NULL) {
            continue;
        }
        attrs = raw_pointer(context, article + MSL_ARTICLE_ATTRS_SOURCE_OFFSET);
        if (attrs != UINT32_MAX) {
            slots[slot]->x4_specialAttributes = translate_target_count(
                context, attrs, *msl_kirby_copy_article_attrs[i].type, 1);
        }
    }
}

static const MslDatType* msl_kirby_copy_root_type(const char* symbol)
{
    static const struct {
        const char* name;
        const MslDatType* const* type;
    } roots[] = {
        { "ftDataKirbyCopyMario", &msl_dat_root_MslDatKirbyHatMario },
        { "ftDataKirbyCopyFox", &msl_dat_root_MslDatKirbyHatFox },
        { "ftDataKirbyCopyCaptain", &msl_dat_root_MslDatKirbyHatCaptain },
        { "ftDataKirbyCopyKoopa", &msl_dat_root_MslDatKirbyHatKoopa },
        { "ftDataKirbyCopyLink", &msl_dat_root_MslDatKirbyHatLink },
        { "ftDataKirbyCopySeak", &msl_dat_root_MslDatKirbyHatSeak },
        { "ftDataKirbyCopyNess", &msl_dat_root_MslDatKirbyHatNess },
        { "ftDataKirbyCopyPeach", &msl_dat_root_MslDatKirbyHatPeach },
        { "ftDataKirbyCopyPopo", &msl_dat_root_MslDatKirbyHatPopo },
        { "ftDataKirbyCopyPikachu", &msl_dat_root_MslDatKirbyHatPikachu },
        { "ftDataKirbyCopySamus", &msl_dat_root_MslDatKirbyHatSamus },
        { "ftDataKirbyCopyYoshi", &msl_dat_root_MslDatKirbyHatYoshi },
        { "ftDataKirbyCopyLuigi", &msl_dat_root_MslDatKirbyHatLuigi },
        { "ftDataKirbyCopyMars", &msl_dat_root_MslDatKirbyHatMars },
        { "ftDataKirbyCopyZelda", &msl_dat_root_MslDatKirbyHatZelda },
        { "ftDataKirbyCopyClink", &msl_dat_root_MslDatKirbyHatClink },
        { "ftDataKirbyCopyDrmario", &msl_dat_root_MslDatKirbyHatDrmario },
        { "ftDataKirbyCopyPichu", &msl_dat_root_MslDatKirbyHatPichu },
        { "ftDataKirbyCopyGanon", &msl_dat_root_MslDatKirbyHatGanon },
        { "ftDataKirbyCopyEmblem", &msl_dat_root_MslDatKirbyHatEmblem },
        { "ftDataKirbyCopyDonkey", &msl_dat_root_MslDatKirbyHatDonkey },
        { "ftDataKirbyCopyPurin", &msl_dat_root_MslDatKirbyHatPurin },
        { "ftDataKirbyCopyMewtwo", &msl_dat_root_MslDatKirbyHatMewtwo },
        { "ftDataKirbyCopyFalco", &msl_dat_root_MslDatKirbyHatFalco },
        { "ftDataKirbyCopyGamewatch", &msl_dat_root_MslDatKirbyHatGamewatch },
    };
    size_t i;
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (strcmp(symbol, roots[i].name) == 0) {
            return *roots[i].type;
        }
    }
    return NULL;
}

#if MSL_GX_RECORD
// A text engine table (baselib/sislib.c, the public symbols "SIS_*"): word 0 the font's glyph bitmaps (32x32, I4, 512 bytes each), word 1
// the kerning pairs (2 bytes per glyph), then the strings the game asks for by index: byte streams of the engine (an op byte, its
// operands; glyph codes are 2 bytes; the grammar of HSD_SisLib_803A6478). The console keeps them as 32-bit pointers into the file; the
// table here holds host pointers to copies of the same bytes, with room after each string for what the game appends to it
// (HSD_SisLib_803A660C).
// refs/melee/src/sysdolphin/baselib/sislib.c
static uint32_t sis_stream_length(const MslNativeArchive* context, uint32_t start)
{
    uint32_t at = start;
    while (at < context->data_size) {
        uint8_t op = context->data[at];
        if (op == 0) {
            return at - start;
        }
        if (op >= 0x20) {
            at += 2;
            continue;
        }
        switch (op) {
        case 6: case 7: case 8: case 9: case 10: case 14:
            if (op == 8 || op == 9) {
                static int warned;
                if (!warned) {
                    warned = 1;
                    fprintf(stderr, "native DAT: a text stream jumps (op %u at 0x%x): not translated\n", (unsigned) op, (unsigned) at);
                }
            }
            at += 5;
            break;
        case 12:
            at += 4;
            break;
        case 5:
            at += 3;
            break;
        default:
            at += 1;
            break;
        }
    }
    return context->data_size - start;
}

static void* translate_sis_table(MslNativeArchive* context, uint32_t offset)
{
    uint32_t count = 0;
    uint8_t** table;
    uint32_t i;
    uint32_t bitmaps;
    uint32_t kerning;

    while (count < 256 && raw_pointer(context, offset + count * 4) != UINT32_MAX) {
        count += 1;
    }
    if (count < 2) {
        return NULL;
    }
    table = native_alloc((size_t) (count + 1) * sizeof(*table));
    bitmaps = raw_pointer(context, offset);
    kerning = raw_pointer(context, offset + 4);
    for (i = 0; i < count; ++i) {
        uint32_t target = raw_pointer(context, offset + i * 4);
        uint32_t length;
        uint32_t room = 0;

        if (target >= context->data_size) {
            continue;
        }
        if (i == 0) {
            length = kerning > bitmaps ? kerning - bitmaps : ((bitmaps - kerning) / 2) * 512;
        } else if (i == 1) {
            length = bitmaps > kerning ? bitmaps - kerning : (kerning - bitmaps) / 512 * 2;
        } else {
            length = sis_stream_length(context, target) + 1;
            room = 512;
        }
        if (target + length > context->data_size) {
            length = context->data_size - target;
        }
        table[i] = native_alloc((size_t) length + room + 4);
        memcpy(table[i], context->data + target, length);
    }
    return table;
}
#endif

static const MslDatType* public_type(const char* symbol)
{
    size_t length = strlen(symbol);
    if (strcmp(symbol, "map_head") == 0) {
        return msl_dat_root_UnkStageDat;
    }
    if (strncmp(symbol, "ftDataKirbyCopy", 15) == 0) {
        // Each copy archive's root is stored straight into ft_80459B88 and
        // read as a KirbyHatStruct; the per-hat views in native_dat_types.c
        // type its slots.
        // refs/melee/src/melee/ft/chara/ftKirby/ftkirby.c::ftKb_Init_803CA9D0
        return msl_kirby_copy_root_type(symbol);
    }
    if (strcmp(symbol, "coll_data") == 0) {
        return msl_dat_root_MapCollData;
    }
    if (strcmp(symbol, "grGroundParam") == 0) {
        return msl_dat_root_UnkStage6B0;
    }
    if (strcmp(symbol, "quake_model_set") == 0) {
        return msl_dat_root_DynamicModelDesc;
    }
    if (strcmp(symbol, "ftDataFox") == 0 ||
        strcmp(symbol, "ftDataCaptain") == 0 ||
        strcmp(symbol, "ftDataSeak") == 0 ||
        strcmp(symbol, "ftDataLuigi") == 0 ||
        strcmp(symbol, "ftDataSamus") == 0 ||
        strcmp(symbol, "ftDataPopo") == 0 ||
        strcmp(symbol, "ftDataNana") == 0 ||
        strcmp(symbol, "ftDataDonkey") == 0 ||
        strcmp(symbol, "ftDataGanon") == 0 ||
        strcmp(symbol, "ftDataPikachu") == 0 ||
        strcmp(symbol, "ftDataPichu") == 0 ||
        strcmp(symbol, "ftDataKirby") == 0 ||
        strcmp(symbol, "ftDataYoshi") == 0 ||
        strcmp(symbol, "ftDataKoopa") == 0 ||
        strcmp(symbol, "ftDataNess") == 0 ||
        strcmp(symbol, "ftDataLink") == 0 ||
        strcmp(symbol, "ftDataClink") == 0 ||
        strcmp(symbol, "ftDataMario") == 0 ||
        strcmp(symbol, "ftDataDrmario") == 0 ||
        strcmp(symbol, "ftDataMars") == 0 ||
        strcmp(symbol, "ftDataEmblem") == 0 ||
        strcmp(symbol, "ftDataPeach") == 0 ||
        strcmp(symbol, "ftDataZelda") == 0 ||
        strcmp(symbol, "ftDataFalco") == 0)
    {
        return msl_dat_root_ftData;
    }
    if (strcmp(symbol, "itPublicData") == 0) {
        return msl_dat_root_it_804D6D20_t;
    }
#if MSL_GX_RECORD
    if (length >= 11 && strcmp(symbol + length - 11, "_image_desc") == 0) {
        return msl_dat_root_HSD_ImageDesc;
    }
#endif
    if (length >= 9 && strcmp(symbol + length - 9, "_figatree") == 0) {
        return msl_dat_root_FigaTree;
    }
    if (length >= 14 &&
        strcmp(symbol + length - 14, "_matanim_joint") == 0)
    {
        return msl_dat_root_HSD_MatAnimJoint;
    }
    if (length >= 6 && strcmp(symbol + length - 6, "_joint") == 0) {
        return msl_dat_root_HSD_Joint;
    }
    return NULL;
}

static void* translate_stage_params(MslNativeArchive* context,
                                    uint32_t offset)
{
    const MslDatType* type;

    // Both imported stage owners publish the same DAT symbol with different
    // anonymous source structs. The source internal-stage owner selected
    // before grDatFiles_801C6038 is the concrete type authority for this
    // public. refs/melee/src/melee/gr/{grbattle.c,grpstadium.c}
    if (stage_info.internal_stage_id == BATTLE) {
        type = msl_dat_root_MslDatBattlefieldParams;
    } else if (stage_info.internal_stage_id == PSTADIUM) {
        type = msl_dat_root_MslDatPokemonStadiumParams;
    } else if (stage_info.internal_stage_id == IZUMI) {
        type = msl_dat_root_MslDatFountainParams;
    } else if (stage_info.internal_stage_id == STORY) {
        type = msl_dat_root_MslDatYoshisStoryParams;
    } else if (stage_info.internal_stage_id == OLDPUPUPU) {
        type = msl_dat_root_MslDatDreamLandParams;
    } else if (stage_info.internal_stage_id == LAST) {
        type = msl_dat_root_MslDatFinalDestinationParams;
    } else {
        fprintf(stderr,
                "native DAT yakumono_param has unsupported internal stage %d\n",
                stage_info.internal_stage_id);
        abort();
    }
    return translate_target_count(context, offset, type, 1);
}

#if MSL_GX_RECORD
/* map_plit: the stage's light sets (a null-ended array of LightList*; each LightList names a chain of HSD_LightDesc). The
 * generated layout leaves HSD_LightDesc.u (attenuation data a point or spot light reads) to be built here by light type,
 * as HSD_LObjLoadDesc reads it (LObjLoad). */
static void translate_light_attenuation(MslNativeArchive* context, uint32_t desc_offset, HSD_LightDesc* desc)
{
    uint32_t u = raw_pointer(context, desc_offset + 0x18);
    const MslDatType* type = NULL;

    if (u == UINT32_MAX) {
        return;
    }
    switch (desc->flags & LOBJ_TYPE_MASK) {
    case LOBJ_POINT:
        type = (desc->attnflags & LOBJ_LIGHT_ATTN) ? msl_dat_root_HSD_LightAttn : msl_dat_root_HSD_LightPointDesc;
        break;
    case LOBJ_SPOT:
        type = desc->attnflags != 0 ? msl_dat_root_HSD_LightAttn : msl_dat_root_HSD_LightSpotDesc;
        break;
    default:
        return;
    }
    desc->u.p = translate_target_count(context, u, type, 1);
}

static void* translate_stage_lights(MslNativeArchive* context, uint32_t offset)
{
    uint32_t count = 0;
    uint32_t i;
    LightList** result;

    while (raw_pointer(context, offset + count * 4) != UINT32_MAX) {
        count += 1;
    }
    result = native_alloc((size_t) (count + 1) * sizeof(*result));
    for (i = 0; i < count; ++i) {
        uint32_t list = raw_pointer(context, offset + i * 4);
        uint32_t desc_offset;
        HSD_LightDesc* desc;

        result[i] = translate_target_count(context, list, msl_dat_root_LightList, 1);
        desc_offset = raw_pointer(context, list);
        for (desc = result[i]->desc; desc != NULL && desc_offset != UINT32_MAX; desc = desc->next) {
            translate_light_attenuation(context, desc_offset, desc);
            desc_offset = raw_pointer(context, desc_offset + 0x04);
        }
    }
    return result;
}

/* A scene's lights (SceneDesc.lights at +8: a null-ended array of LightList*): the scene's own translation covers the descs, not the
 * attenuation data a point or spot light reads (translate_light_attenuation) */
static void translate_scene_light_attenuation(MslNativeArchive* context, uint32_t scene_offset, SceneDesc* scene)
{
    uint32_t array = raw_pointer(context, scene_offset + 0x08);
    uint32_t i;

    if (scene == NULL || scene->lights == NULL || array == UINT32_MAX) {
        return;
    }
    for (i = 0; scene->lights[i] != NULL; ++i) {
        uint32_t list = raw_pointer(context, array + i * 4);
        uint32_t desc_offset;
        HSD_LightDesc* desc;

        if (list == UINT32_MAX) {
            return;
        }
        desc_offset = raw_pointer(context, list);
        for (desc = scene->lights[i]->desc; desc != NULL && desc_offset != UINT32_MAX; desc = desc->next) {
            translate_light_attenuation(context, desc_offset, desc);
            desc_offset = raw_pointer(context, desc_offset + 0x04);
        }
    }
}

/* A stage file's lights beyond map_plit: each map entry (UnkStageDat_x8_t at +8 of map_head, 0x34 bytes each) names its light
 * sets at +0x18 (the decomp types it void*), and UnkStageDat names the per-light flag overrides (Ground_801C20E0) at +0x18
 * with their count at +0x1C. The viewer build's Ground_801C466C reads both. */
/* Whether the words at `offset` are a stage's light sets (a null-ended array of LightList pointers, each a desc pointer and an
 * animation pointer, each desc a class name, a next pointer, light flags): the map entries' +0x18 word names different things in
 * different entries, and the stage's callbacks that say which is the lights are not known while the file is read. */
extern StageCallbacks* msl_native_dat_stage_callbacks;
static int looks_like_light_sets(MslNativeArchive* context, uint32_t offset)
{
    uint32_t n = 0;
    while (n < 16) {
        uint32_t list = raw_pointer(context, offset + n * 4);
        uint32_t desc;
        uint32_t flags;
        if (list == UINT32_MAX) {
            break;
        }
        if (list + 8 > context->data_size) {
            return 0;
        }
        desc = raw_pointer(context, list);
        if (desc == UINT32_MAX || desc + 0x1C > context->data_size) {
            return 0;
        }
        flags = read_be32(context->data + desc + 8);
        if ((flags >> 16) >= 0x1000 || (flags & 0xFFFF) >= 0x10) {
            return 0;
        }
        n++;
    }
    return n >= 1 && n < 16 && offset + n * 4 + 4 <= context->data_size && read_be32(context->data + offset + n * 4) == 0;
}

static void translate_stage_light_tables(MslNativeArchive* context, uint32_t map_head, UnkStageDat* dat)
{
    typedef struct MslLightOverride {
        HSD_LightDesc* desc;
        uint8_t a : 1, b : 1, c : 1;   /* the source byte's top three bits (0x80, 0x40, 0x20) */
        uint8_t rest : 5;
        uint8_t pad[3];   /* (ground.c's LightOverrideEntry: the host's own stride, 8 bytes with 4-byte pointers) */
    } MslLightOverride;
    uint32_t maps = raw_pointer(context, map_head + 8);
    uint32_t count = read_be32(context->data + map_head + 12);
    uint32_t i;
    uint32_t over;
    uint32_t over_count;

    /* map_plit's lights first: the overrides name descs of both sets */
    (void) msl_native_archive_get_public(context->archive, "map_plit");
    over = raw_pointer(context, map_head + 0x18);
    over_count = read_be32(context->data + map_head + 0x1C);

    if (maps != UINT32_MAX && dat->unk8 != NULL) {
        for (i = 0; i < count; ++i) {
            dat->unk8[i].x18 = NULL;   /* (the other entries' +0x18 words name other things: left out) */
        }
        for (i = 0; i < count; ++i) {
            uint32_t lights = raw_pointer(context, maps + i * 0x34 + 0x18);
            /* the map the stage's callbacks flag (0x80000000) holds the lights, as Ground_801C466C_inline picks it */
            if (msl_native_dat_stage_callbacks != NULL && !(msl_native_dat_stage_callbacks[i].flags & 0x80000000)) {
                continue;
            }
            if (lights != UINT32_MAX && looks_like_light_sets(context, lights)) {
                dat->unk8[i].x18 = translate_stage_lights(context, lights);
                break;
            }
        }
    }
    if (over != UINT32_MAX && over_count != 0) {
        MslLightOverride* native = native_alloc((size_t) over_count * sizeof(*native));
        for (i = 0; i < over_count; ++i) {
            uint32_t desc = raw_pointer(context, over + i * 8);
            uint8_t bits = context->data[over + i * 8 + 4];
            if (desc != UINT32_MAX) {
                /* only descs of a light set this build translates can be one a set holds: the rest never match */
                MslDatMemo* memo = find_memo(context, desc);
                if (memo != NULL && memo->type == msl_dat_root_HSD_LightDesc) {
                    native[i].desc = memo->native;
                }
            }
            native[i].a = (bits & 0x80) != 0;
            native[i].b = (bits & 0x40) != 0;
            native[i].c = (bits & 0x20) != 0;
        }
        dat->unk18 = native;
    }
    /* UnkStageDat.unk20: the light animations that loop (Ground_801C43C4): {LightAnim*, flag in the source byte's top bit} */
    {
        typedef struct MslLightAnimFlag {
            void* unk0;
            uint8_t flag : 1;   /* (the decomp's Ground_801C43C4 element: a pointer and a byte, the host's own stride) */
        } MslLightAnimFlag;
        uint32_t table = raw_pointer(context, map_head + 0x20);
        uint32_t table_count = read_be32(context->data + map_head + 0x24);
        if (table != UINT32_MAX && table_count != 0) {
            MslLightAnimFlag* native = native_alloc((size_t) table_count * sizeof(*native));
            for (i = 0; i < table_count; ++i) {
                uint32_t anim = raw_pointer(context, table + i * 8);
                MslDatMemo* memo = anim != UINT32_MAX ? find_memo(context, anim) : NULL;
                native[i].unk0 = memo != NULL ? memo->native : NULL;
                native[i].flag = (context->data[table + i * 8 + 4] & 0x80) != 0;
            }
            dat->unk20 = native;
        }
    }
}
#endif

static void* translate_stage_item_public(MslNativeArchive* context,
                                         uint32_t offset)
{
    enum {
        STAGE_ITEM_KIND_SOURCE_OFFSET = 0x00,
        STAGE_ITEM_ARTICLE_SOURCE_OFFSET = 0x04,
        ARTICLE_SPECIAL_ATTRS_SOURCE_OFFSET = 0x04,
    };
    typedef struct MslNativeStageItemEntry {
        int kind;
        Article* article;
    } MslNativeStageItemEntry;
    uint32_t count = 0;
    uint32_t i;
    MslNativeStageItemEntry** result;

    while (raw_pointer(context, offset + count * 4) != UINT32_MAX) {
        count += 1;
    }
    result = native_alloc((size_t) (count + 1) * sizeof(*result));
    for (i = 0; i < count; ++i) {
        uint32_t entry_source = raw_pointer(context, offset + i * 4);
        uint32_t article_source;
        result[i] = translate_target_count(
            context, entry_source, msl_dat_root_MslDatStageItemEntry, 1);
        article_source = raw_pointer(
            context, entry_source + STAGE_ITEM_ARTICLE_SOURCE_OFFSET);
        if (result[i]->kind == It_Kind_Heiho && article_source != UINT32_MAX) {
            uint32_t attrs_source = raw_pointer(
                context,
                article_source + ARTICLE_SPECIAL_ATTRS_SOURCE_OFFSET);
            if (result[i]->article == NULL || attrs_source == UINT32_MAX) {
                fprintf(stderr,
                        "native GrSt.dat Heiho article is incomplete\n");
                abort();
            }
            result[i]->article->x4_specialAttributes =
                translate_target_count(context, attrs_source,
                                       msl_dat_root_MslDatHeihoAttrs, 1);
        }
        (void) STAGE_ITEM_KIND_SOURCE_OFFSET;
    }
    return result;
}

static void* translate_item_public(MslNativeArchive* context,
                                   uint32_t offset)
{
    enum {
        COMMON_ITEM_COUNT = 43,
        CHARACTER_ITEM_COUNT = 118,
        POKEMON_ITEM_COUNT = 47,
    };
    it_804D6D20_t* result = native_alloc(sizeof(*result));
    uint32_t target;
    uint32_t article_source;
    uint32_t attrs_source;

    add_memo(context, offset, msl_dat_root_it_804D6D20_t, result);
    target = raw_pointer(context, offset + 0x00);
    if (target != UINT32_MAX) {
        result->x0 = translate_target_count(
            context, target, msl_dat_root_ItemCommonData, 1);
    }
    // Translate the source-owned common Article table. Stage item spawning is
    // disabled, but Peach's SpecialLw directly selects BombHei, Dosei, or
    // Sword from this table. Article.x4 is void in the decomp, so install the
    // three concrete source attribute layouts explicitly after translating
    // the shared Article graphs.
    // refs/melee/src/melee/it/iteffect.c::it_802787B4
    // refs/melee/src/melee/ft/chara/ftPeach/ftPe_SpecialLw.c
    target = raw_pointer(context, offset + 0x04);
    if (target != UINT32_MAX) {
        result->x4 = translate_target_count(
            context, target, msl_dat_root_MslDatCommonItemArticles, 1);
#define TRANSLATE_COMMON_ITEM_ATTRS(kind, type)                               \
    do {                                                                      \
        article_source = raw_pointer(context, target + (kind) * 4);           \
        if (article_source != UINT32_MAX && result->x4[(kind)] != NULL) {     \
            attrs_source = raw_pointer(context, article_source + 0x04);       \
            if (attrs_source != UINT32_MAX) {                                 \
                result->x4[(kind)]->x4_specialAttributes =                    \
                    translate_target_count(context, attrs_source, (type), 1); \
            }                                                                 \
        }                                                                     \
    } while (0)
        TRANSLATE_COMMON_ITEM_ATTRS(It_Kind_BombHei,
                                    msl_dat_root_itBombHeiAttributes);
        TRANSLATE_COMMON_ITEM_ATTRS(It_Kind_Dosei,
                                    msl_dat_root_itDoseiAttributes);
        TRANSLATE_COMMON_ITEM_ATTRS(It_Kind_Sword,
                                    msl_dat_root_itSword_UnkArticle1);
#undef TRANSLATE_COMMON_ITEM_ATTRS
    } else {
        result->x4 = native_alloc(COMMON_ITEM_COUNT * sizeof(*result->x4));
    }
    // Character articles are installed later by each fighter's OnLoad path.
    // refs/melee/src/melee/it/item.c::Item_80267978
    result->x8 = native_alloc(CHARACTER_ITEM_COUNT * sizeof(*result->x8));
    result->xC = native_alloc(POKEMON_ITEM_COUNT * sizeof(*result->xC));
    target = raw_pointer(context, offset + 0x10);
    if (target != UINT32_MAX) {
        result->x10 = translate_target_count(
            context, target, msl_dat_root_it_804D6D40_t, 1);
    }
    target = raw_pointer(context, offset + 0x14);
    if (target != UINT32_MAX) {
        result->x14 = translate_target(
            context, target, msl_dat_root_Fighter_804D653C_t);
    }
    return result;
}

// PlCo.dat's CPU input block. The DWARF layout carries its members as void
// pointers, so the generic translation copies the payloads as raw big-endian
// bytes and the CPU attack evaluator reads byte-swapped garbage: no
// CPU-driven fighter ever selects an attack. Translate the consumed shapes
// by hand: per-kind pointer arrays of cmd-terminated 0x24-byte attack
// entries (uniform 32-bit lanes), the per-kind distance floats, the weapon
// reach floats, and the byte-encoded command scripts.
// refs/melee/src/melee/ft/ftcpuattack.c::{ftCo_800B4AB0,ftCo_800B8A9C}
// refs/melee/src/melee/ft/ftcmdscript.c::{ftCo_800B3E04,ftCo_800B4880}
enum {
    MSL_CPU_TABLE_KINDS = 33,       // tables index by fp->kind
    // Scripts index by CPU attack/defend command ids (0x28/0x29-class rows
    // land here beside the literal 38/0x26 call sites); slots past the
    // source array read as non-relocated fields and stay NULL.
    MSL_CPU_CMDSCRIPT_COUNT = 64,
    MSL_CPU_WEAPON_REACH_COUNT = 6, // Harisen..Parasol reach bonuses
    MSL_CPU_ATTACK_ENTRY_BYTES = 0x24,
};

static void* translate_cpu_attack_entries(MslNativeArchive* context,
                                          uint32_t offset)
{
    uint32_t count = 0;
    uint32_t words;
    uint32_t w;
    uint32_t* native;

    while (offset + (count + 1) * MSL_CPU_ATTACK_ENTRY_BYTES <=
               context->data_size &&
           read_be32(context->data + offset +
                     count * MSL_CPU_ATTACK_ENTRY_BYTES) != 0)
    {
        ++count;
    }
    words = count * (MSL_CPU_ATTACK_ENTRY_BYTES / 4);
    native = native_alloc((count + 1) * MSL_CPU_ATTACK_ENTRY_BYTES);
    for (w = 0; w < words; ++w) {
        native[w] = read_be32(context->data + offset + w * 4);
    }
    for (w = words; w < (count + 1) * (MSL_CPU_ATTACK_ENTRY_BYTES / 4); ++w) {
        native[w] = 0;
    }
    return native;
}

static void* translate_cpu_attack_table(MslNativeArchive* context,
                                        uint32_t offset)
{
    void** native = native_alloc(MSL_CPU_TABLE_KINDS * sizeof(*native));
    uint32_t kind;

    for (kind = 0; kind < MSL_CPU_TABLE_KINDS; ++kind) {
        uint32_t target = raw_pointer(context, offset + kind * 4);
        native[kind] = target != UINT32_MAX
                           ? translate_cpu_attack_entries(context, target)
                           : NULL;
    }
    return native;
}

static void* translate_cpu_float_array(MslNativeArchive* context,
                                       uint32_t offset, uint32_t count)
{
    uint32_t* native = native_alloc(count * sizeof(*native));
    uint32_t i;

    for (i = 0; i < count; ++i) {
        native[i] = read_be32(context->data + offset + i * 4);
    }
    return native;
}

static void* translate_cpu_cmdscript(MslNativeArchive* context,
                                     uint32_t offset)
{
    // Byte-encoded command stream: opcodes above 0xBF carry two argument
    // bytes, above 0x7F one, and 0x7F terminates.
    // refs/melee/src/melee/ft/ftcmdscript.c::ftCo_800B4880
    uint32_t end = offset;
    uint8_t* native;
    uint32_t size;

    while (end < context->data_size && context->data[end] != 0x7F) {
        uint8_t cmd = context->data[end];
        end += 1 + (cmd > 0xBF ? 2 : cmd > 0x7F ? 1 : 0);
    }
    size = end + 1 - offset;
    native = native_alloc(size);
    memcpy(native, context->data + offset, size);
    return native;
}

static void* translate_fighter_cpu_tables(MslNativeArchive* context,
                                          uint32_t offset)
{
    void** result = native_alloc(10 * sizeof(*result));
    uint32_t member;

    for (member = 0; member < 10; ++member) {
        uint32_t target = raw_pointer(context, offset + member * 4);
        if (target == UINT32_MAX) {
            result[member] = NULL;
            continue;
        }
        switch (member) {
        case 0: { // cmdscripts: script pointer array
            void** scripts =
                native_alloc(MSL_CPU_CMDSCRIPT_COUNT * sizeof(*scripts));
            uint32_t i;
            for (i = 0; i < MSL_CPU_CMDSCRIPT_COUNT; ++i) {
                uint32_t script = raw_pointer(context, target + i * 4);
                scripts[i] = script != UINT32_MAX
                                 ? translate_cpu_cmdscript(context, script)
                                 : NULL;
            }
            result[member] = scripts;
            break;
        }
        case 8: // x20: per-kind distance thresholds
            result[member] = translate_cpu_float_array(context, target,
                                                       MSL_CPU_TABLE_KINDS);
            break;
        case 9: // x24: held-weapon reach bonuses
            result[member] = translate_cpu_float_array(
                context, target, MSL_CPU_WEAPON_REACH_COUNT);
            break;
        default: // x4..x1C: per-kind attack entry tables
            result[member] = translate_cpu_attack_table(context, target);
            break;
        }
    }
    return result;
}

static void* translate_fighter_common_public(MslNativeArchive* context,
                                             uint32_t offset)
{
    const MslDatType* const element_types[23] = {
        msl_dat_root_ftCommonData,
        // PlCo.dat's second public pointer is the 26-entry throw-attribute
        // table consumed across ftCo_MS_LightThrowF..HeavyThrowLw4.
        // refs/melee/src/melee/ft/chara/ftCommon/ftCo_ItemThrow.c::{
        //   ftCo_80095D5C,ftCo_80095EFC}
        msl_dat_root_MslDatItemThrowAttrs,
        msl_dat_root_MslDatFloat5,
        msl_dat_root_MslDatFloat,
        msl_dat_root_MslDatFighterPartsPointer,
        msl_dat_root_MslDatFighter6540Pointer,
        msl_dat_root_Fighter_804D653C_t,
        msl_dat_root_Fighter_804D653C_t,
        // refs/melee/src/melee/ft/ft_0D4D.c::ftCo_800D4FF4
        msl_dat_root_Fighter_804D6534_t,
        msl_dat_root_MslDatVec2Pointer,
        // Pointers 10 and 11 are one (list, length) pair each: the shake of
        // a held fighter that mashes and of a smash charge. Fighter_804D652C_t
        // has Fighter_804D6528_t's layout; as bytes the pair kept the file's
        // 32-bit words and ftCommon_InitGrab read its length past them.
        // refs/melee/src/melee/ft/ftcommon.c::{ftCommon_InitGrab,
        //   ftCommon_GrabMash,ftCommon_8008021C}
        msl_dat_root_Fighter_804D6528_t,
        msl_dat_root_Fighter_804D6528_t,
        msl_dat_root_Fighter_804D6524_t,
        msl_dat_root_Fighter_804D6520_t,
        msl_dat_root_Fighter_804D651C_t,
        msl_dat_root_Fighter_804D6518_t,
        msl_dat_root_HSD_Joint,
        msl_dat_root_MslDatByte,
        msl_dat_root_MslDatByte,
        msl_dat_root_MslDatByte,
        msl_dat_root_HSD_Joint,
        msl_dat_root_CrowdConfig,
        msl_dat_root_Fighter_804D64FC_t,
    };
    void** result = native_alloc(sizeof(*result) * 23);
    uint32_t i;

    for (i = 0; i < 23; ++i) {
        uint32_t target = raw_pointer(context, offset + i * 4);
        if (target != UINT32_MAX) {
            result[i] = i == 22
                            ? translate_fighter_cpu_tables(context, target)
                            : translate_target(context, target,
                                               element_types[i]);
        }
    }
    // Pointer 9 (Fighter_804D6530, the hit's model shake) is three pairs of
    // a table of offsets and that table's length: air, ground, electric. As
    // pointers the three lengths (plain words, no relocation) came out null,
    // so ftCo_8009064C stored a length of 0 and Fighter_procUpdate put the
    // shake's index back to 0 every frame: the shake stood on its table's
    // first entry. The lengths are the file's own words, kept in the odd
    // slots as the game's code reads them. Only the model's shake reads
    // this; gameplay does not.
    // refs/melee/src/melee/ft/chara/ftCommon/ftCo_DamageFall.c::{
    //   ftCo_8009064C,ftCo_80090690}, refs/melee/src/melee/ft/fighter.c
    {
        uint32_t target = raw_pointer(context, offset + 9 * 4);
        void** pairs = result[9];
        if (target != UINT32_MAX && pairs != NULL &&
            next_boundary(context, target) - target >= 24)
        {
            for (i = 0; i < 3; ++i) {
                pairs[i * 2 + 1] = (void*) (uintptr_t) read_be32(
                    context->data + target + (i * 2 + 1) * 4);
            }
        }
    }
    return result;
}

typedef enum MslFighterArticleProfile {
    MSL_FIGHTER_ARTICLES_NONE,
    MSL_FIGHTER_ARTICLES_FOX,
    MSL_FIGHTER_ARTICLES_FALCO,
    MSL_FIGHTER_ARTICLES_SHEIK,
    MSL_FIGHTER_ARTICLES_PEACH,
    MSL_FIGHTER_ARTICLES_ZELDA,
    MSL_FIGHTER_ARTICLES_LUIGI,
    MSL_FIGHTER_ARTICLES_MARIO,
    MSL_FIGHTER_ARTICLES_MARIOD,
    MSL_FIGHTER_ARTICLES_SAMUS,
    MSL_FIGHTER_ARTICLES_ICECLIMBER,
    MSL_FIGHTER_ARTICLES_PIKACHU,
    MSL_FIGHTER_ARTICLES_KIRBY,
    MSL_FIGHTER_ARTICLES_YOSHI,
    MSL_FIGHTER_ARTICLES_KOOPA,
    MSL_FIGHTER_ARTICLES_MEWTWO,
    MSL_FIGHTER_ARTICLES_GAMEWATCH,
    MSL_FIGHTER_ARTICLES_NESS,
    MSL_FIGHTER_ARTICLES_LINK,
    MSL_FIGHTER_ARTICLES_CLINK,
    MSL_FIGHTER_AUX_PURIN_PARTS,
} MslFighterArticleProfile;

#if MSL_GX_RECORD
/* The part-visibility tables of a fighter (FtPartsDesc.vis_table, ftParts_8007487C): the decomp types them void*, so the
 * layout generator leaves them as raw big-endian PPC data. The viewer build reads them (ftParts_80074D7C hides DObjs by
 * them), so they are rebuilt here as the native FtPartsVisLookup / TempS / u8 arrays. */
static void translate_parts_vis(MslNativeArchive* context, uint32_t desc_offset, FtPartsDesc* desc)
{
    uint32_t models, table, rows, r, c, m, j;
    void* (*native)[4];

    if (desc_offset == UINT32_MAX || desc_offset + 8 > context->data_size) {
        return;
    }
    models = read_be32(context->data + desc_offset);
    table = raw_pointer(context, desc_offset + 4);
    if (table == UINT32_MAX || models > 11) {
        return;
    }
    rows = (next_boundary(context, table) - table) / 16;
    if (rows == 0) {
        rows = 1;
    }
    native = native_alloc((size_t) rows * sizeof(*native));
    for (r = 0; r < rows; r++) {
        for (c = 0; c < 4; c++) {
            uint32_t lookup = raw_pointer(context, table + r * 16 + c * 4);
            FtPartsVisLookup* lk;
            if (lookup == UINT32_MAX) {
                continue;
            }
            lk = native_alloc((size_t) models * sizeof(*lk));
            for (m = 0; m < models; m++) {
                uint32_t count = read_be32(context->data + lookup + m * 8);
                uint32_t list = raw_pointer(context, lookup + m * 8 + 4);
                lk[m].x0 = (int) count;
                if (list == UINT32_MAX || count == 0) {
                    continue;
                }
                lk[m].x4 = native_alloc((size_t) count * sizeof(TempS));
                for (j = 0; j < count; j++) {
                    uint32_t n = read_be32(context->data + list + j * 8);
                    uint32_t bytes = raw_pointer(context, list + j * 8 + 4);
                    lk[m].x4[j].x0 = (int) n;
                    if (bytes != UINT32_MAX && n != 0) {
                        lk[m].x4[j].x4 = native_alloc(n);
                        memcpy(lk[m].x4[j].x4, context->data + bytes, n);
                    }
                }
            }
            native[r][c] = lk;
        }
    }
    desc->vis_table = native;
}
#endif

static ftData* translate_fighter_public(
    MslNativeArchive* context, uint32_t offset,
    const MslDatType* attrs_type, uint32_t anim_count,
    MslFighterArticleProfile article_profile)
{
    enum {
        FT_DATA_X48_ITEMS_SOURCE_OFFSET = 0x48,
        ARTICLE_SPECIAL_ATTRS_SOURCE_OFFSET = 0x04,
        MAX_REACHED_ARTICLE_COUNT = 11,
    };
    const MslDatType* article_list_type = NULL;
    const MslDatType* attr_types[MAX_REACHED_ARTICLE_COUNT] = { NULL };
    uint8_t indices[MAX_REACHED_ARTICLE_COUNT] = { 0 };
    uint32_t article_count = 0;
    ftData* result = translate_target_count(
        context, offset, msl_dat_root_ftData, 1);
    uint32_t target = raw_pointer(context, offset + 0x04);
    uint32_t list = raw_pointer(
        context, offset + FT_DATA_X48_ITEMS_SOURCE_OFFSET);
    uint32_t i;

    if (target == UINT32_MAX) {
        fprintf(stderr, "native fighter DAT is missing special attributes\n");
        abort();
    }
    result->ext_attr =
        translate_target_count(context, target, attrs_type, 1);
    target = raw_pointer(context, offset + 0x0C);
    if (target == UINT32_MAX) {
        fprintf(stderr, "native fighter DAT is missing its animation table\n");
        abort();
    }
    result->xC = translate_target_count(
        context, target, msl_dat_root_Fighter_WaitAnimData, anim_count);
    target = raw_pointer(context, offset + 0x10);
    if (target == UINT32_MAX) {
        fprintf(stderr, "native fighter DAT is missing animation metadata\n");
        abort();
    }
    result->x10 = translate_target_count(
        context, target, msl_dat_root_MslDatAnimBytePair, anim_count);
#if MSL_GX_RECORD
    translate_parts_vis(context, raw_pointer(context, offset + 0x08), &result->x8->x0);
#endif

    switch (article_profile) {
    case MSL_FIGHTER_ARTICLES_NONE:
        break;
    case MSL_FIGHTER_ARTICLES_FOX:
    case MSL_FIGHTER_ARTICLES_FALCO:
        article_list_type = msl_dat_root_MslDatSpaceAnimalArticles;
        article_count = 3;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = article_profile == MSL_FIGHTER_ARTICLES_FALCO ? 3 : 2;
        attr_types[0] = msl_dat_root_FoxLaserAttr;
        attr_types[1] = msl_dat_root_FoxBlasterAttr;
        attr_types[2] = msl_dat_root_FoxIllusionAttr;
        break;
    case MSL_FIGHTER_ARTICLES_SHEIK:
        article_list_type = msl_dat_root_MslDatSheikArticles;
        article_count = 4;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 3;
        attr_types[0] = msl_dat_root_itSeakNeedleThrownAttributes;
        attr_types[3] = msl_dat_root_itSeakChain_Attrs;
        break;
    case MSL_FIGHTER_ARTICLES_PEACH:
        article_list_type = msl_dat_root_MslDatPeachArticles;
        article_count = 5;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 3;
        indices[4] = 4;
        attr_types[1] = msl_dat_root_MslDatPeachTurnipAttrs;
        attr_types[4] = msl_dat_root_itPeachToadSporeAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_ZELDA:
        article_list_type = msl_dat_root_MslDatZeldaArticles;
        article_count = 2;
        indices[0] = 0;
        indices[1] = 1;
        attr_types[0] = msl_dat_root_MslDatZeldaDinFireAttrs;
        attr_types[1] = msl_dat_root_itZeldaDinFireExplodeAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_LUIGI:
        article_list_type = msl_dat_root_MslDatLuigiArticles;
        article_count = 1;
        indices[0] = 0;
        attr_types[0] = msl_dat_root_itUnkAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_MARIO:
        // PlMr/PlDr share one four-slot article layout, but each DAT only
        // populates its own owner's slots: Mario registers items[0]
        // (fireball) and items[2] (cape); Dr. Mario registers items[1]
        // (megavitamin) and items[3] (super sheet). The fireball and
        // megavitamin attribute blocks share the five-float itUnkAttributes
        // shape; the capes declare no special attributes.
        // refs/melee/src/melee/ft/chara/{ftMario/ftMr_Init.c,
        // ftDrMario/ftDr_Init.c}
        article_list_type = msl_dat_root_MslDatMarioArticles;
        article_count = 2;
        indices[0] = 0;
        indices[1] = 2;
        attr_types[0] = msl_dat_root_itUnkAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_MARIOD:
        article_list_type = msl_dat_root_MslDatMarioArticles;
        article_count = 2;
        indices[0] = 1;
        indices[1] = 3;
        attr_types[0] = msl_dat_root_itUnkAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_SAMUS:
        // PlSs.dat's five-slot list holds the four articles registered by
        // ftSs_Init_OnLoad (bomb, charge shot, missile, grapple beam), each
        // with its concrete attribute block, and the throw grapple-beam
        // accessory graph the list type translates structurally.
        // refs/melee/src/melee/ft/chara/ftSamus/ftSs_Init.c
        // refs/melee/src/melee/it/items/{itsamusbomb.c,itsamuschargeshot.c,
        //   itsamusmissile.c,itsamusgrapple.c}
        article_list_type = msl_dat_root_MslDatSamusArticles;
        article_count = 4;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 3;
        attr_types[0] = msl_dat_root_itSamusBombAttributes;
        attr_types[1] = msl_dat_root_itSamusChargeShot_Attributes;
        attr_types[2] = msl_dat_root_itSamusMissileAttributes;
        attr_types[3] = msl_dat_root_itSamusGrappleAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_ICECLIMBER:
        // PlPp.dat and PlNn.dat share one three-slot article layout: the ice
        // shot, blizzard, and belay-string articles ftPp_Init_OnLoad registers
        // as item kinds 106/107/113. Nana's DAT carries its own copies of the
        // same graphs; her OnLoad registers nothing, so both files translate
        // through the one list type.
        // refs/melee/src/melee/ft/chara/ftPopo/ftPp_Init.c
        // refs/melee/src/melee/it/items/{itclimbersice.c,itclimbersblizzard.c,
        //   itclimbersstring.c}
        article_list_type = msl_dat_root_MslDatIceClimberArticles;
        article_count = 3;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        attr_types[0] = msl_dat_root_itClimbersIceAttributes;
        attr_types[1] = msl_dat_root_itClimbersBlizzardAttributes;
        attr_types[2] = msl_dat_root_itClimbersStringAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_PIKACHU:
        // PlPk.dat's x48_items leads with the three articles
        // ftPk_Init_OnLoad registers under the item kinds stored in its
        // attribute block (xDC thunder 81, x14 ground jolt 89, x18 air jolt
        // 90). The thunder and ground jolt carry concrete attribute blocks;
        // the air jolt's ported logic never reads x4_specialAttributes. The
        // trailing DAT slots hold presentation graphs no ported code
        // reaches.
        // refs/melee/src/melee/ft/chara/ftPikachu/ftPk_Init.c
        // refs/melee/src/melee/it/items/{itpikachuthunder.c,
        //   itpikachutjoltground.c,itpikachutjoltair.c}
        article_list_type = msl_dat_root_MslDatPikachuArticles;
        article_count = 3;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        attr_types[0] = msl_dat_root_itPikachuthunderAttributes;
        attr_types[1] = msl_dat_root_itPikachutJoltGroundAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_KIRBY:
        // ftKb_Init_OnLoad registers four articles under item kinds 50..53:
        // the Final Cutter beam, the hammer, and the two copy-star slots
        // (It_Kind_Unk1/Unk2). The beam carries itKirbyCutterBeamAttributes
        // (speed, lifetime); the hat-drop star (kind 52, it_2ADA.c) reads
        // the shared five-float itUnkAttributes block for its spin speed;
        // the hammer and the second star slot (it_2F28.c) read none.
        // refs/melee/src/melee/ft/chara/ftKirby/ftkirby.c::ftKb_Init_OnLoad
        // refs/melee/src/melee/it/items/itkirbycutterbeam.c::it_8029BB90
        // refs/melee/src/melee/it/items/it_2ADA.c::it_802ADA1C
        article_list_type = msl_dat_root_MslDatKirbyArticles;
        article_count = 4;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 3;
        attr_types[0] = msl_dat_root_itKirbyCutterBeamAttributes;
        attr_types[2] = msl_dat_root_itUnkAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_KOOPA:
        // ftKp_Init_OnLoad registers the single Flame article.
        article_list_type = msl_dat_root_MslDatKoopaArticles;
        article_count = 1;
        indices[0] = 0;
        attr_types[0] = msl_dat_root_itKoopaFlame_Attributes;
        break;
    case MSL_FIGHTER_ARTICLES_YOSHI:
        // ftYs_Init_OnLoad: EggThrow, Star, EggLay. The fourth list slot
        // is the victim's egg accessory, translated by the structural type.
        article_list_type = msl_dat_root_MslDatYoshiArticles;
        article_count = 3;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        attr_types[0] = msl_dat_root_itYoshiEggThrowAttributes;
        attr_types[1] = msl_dat_root_StarAttrs;
        break;
    case MSL_FIGHTER_ARTICLES_MEWTWO:
        // ftMt_Init_OnLoad: Disable, Shadow Ball.
        article_list_type = msl_dat_root_MslDatMewtwoArticles;
        article_count = 2;
        indices[0] = 0;
        indices[1] = 1;
        attr_types[0] = msl_dat_root_itMDisableAttributes;
        attr_types[1] = msl_dat_root_itMewtwoShadowball_DatAttrs;
        break;
    case MSL_FIGHTER_ARTICLES_GAMEWATCH:
        // ftGw_Init_OnLoad and itgamewatch*: one-pointer attributes except
        // Chef's five-entry trajectory table. Slot 10 is not an Article.
        article_list_type = msl_dat_root_MslDatGameWatchArticles;
        article_count = 10;
        for (i = 0; i < article_count; ++i) {
            indices[i] = (uint8_t) i;
            attr_types[i] = msl_dat_root_itGamewatchparachuteAttributes;
        }
        attr_types[8] = msl_dat_root_MslDatGameWatchChefAttrs;
        break;
    case MSL_FIGHTER_ARTICLES_NESS:
        // PlNs.dat's x48_items carries the eleven articles ftNs_Init_OnLoad
        // registers: [0] PK Fire, [1] PK Fire pillar, [2] PK Flash charge,
        // [3] PK Thunder ball, [4..7] the four PK Thunder trail segments,
        // [8] PK Flash explosion, [9] the baseball bat, and [10] the Yo-Yo.
        // The trail segments and the bat read no special attributes; PK Fire
        // shares the pillar's two-float attribute shape (it_802AA1D8 and the
        // Logic23 spawn hooks read x0/x4 from its own article).
        // refs/melee/src/melee/ft/chara/ftNess/ftNs_Init.c
        // refs/melee/src/melee/it/items/{itnesspkfire.c,itnesspkfirepillar.c,
        //   itnesspkflash.c,itnesspkflashexplode.c,itnesspkthunderball.c,
        //   itnesspkthundertrail.c,itnessbat.c,itnessyoyo.c}
        article_list_type = msl_dat_root_MslDatNessArticles;
        article_count = 11;
        for (i = 0; i < article_count; ++i) {
            indices[i] = (uint8_t) i;
        }
        attr_types[0] = msl_dat_root_itNessPKFirepillarAttributes;
        attr_types[1] = msl_dat_root_itNessPKFirepillarAttributes;
        attr_types[2] = msl_dat_root_itFlashAttributes;
        attr_types[3] = msl_dat_root_itPKThunderAttributes;
        attr_types[8] = msl_dat_root_itFlashExplAttributes;
        attr_types[10] = msl_dat_root_itYoyoAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_LINK:
        // PlLk.dat's seven-slot x48_items carries the five articles
        // ftLk_Init_OnLoad registers: [0] bomb (kind 58), [1] boomerang
        // (60), [2] hookshot (62), [3] arrow (64), and [4] bow (76). Slot 5
        // is the milk-bottle slot only Young Link populates (NULL here) and
        // slot 6 is the sword HSD_Joint accessory the list type translates
        // structurally. The bow's ported logic reads no special attributes.
        // refs/melee/src/melee/ft/chara/ftLink/ftLk_Init.c
        // refs/melee/src/melee/it/items/{itlinkbomb.c,itlinkboomerang.c,
        //   itlinkhookshot.c,itlinkarrow.c,itlinkbow.c}
        article_list_type = msl_dat_root_MslDatLinkArticles;
        article_count = 5;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 3;
        indices[4] = 4;
        attr_types[0] = msl_dat_root_itLinkBombAttributes;
        attr_types[1] = msl_dat_root_itLinkBoomerangAttributes;
        attr_types[2] = msl_dat_root_itLinkHookshotAttributes;
        attr_types[3] = msl_dat_root_itLinkArrowAttributes;
        break;
    case MSL_FIGHTER_ARTICLES_CLINK:
        // PlCl.dat shares Link's seven-slot layout and additionally
        // populates slot 5 with the milk-bottle article ftCl_Init_OnLoad
        // registers as kind 123; neither the bow nor the milk reads special
        // attributes.
        // refs/melee/src/melee/ft/chara/ftCLink/ftCl_Init.c
        // refs/melee/src/melee/it/items/itclinkmilk.c
        article_list_type = msl_dat_root_MslDatLinkArticles;
        article_count = 6;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 3;
        indices[4] = 4;
        indices[5] = 5;
        attr_types[0] = msl_dat_root_itLinkBombAttributes;
        attr_types[1] = msl_dat_root_itLinkBoomerangAttributes;
        attr_types[2] = msl_dat_root_itLinkHookshotAttributes;
        attr_types[3] = msl_dat_root_itLinkArrowAttributes;
        break;
    case MSL_FIGHTER_AUX_PURIN_PARTS:
        // x48_items is not an article table for Purin. Its second pointer owns
        // the costume FtPartsDesc consumed by ftPr_Init_8013C360.
        // refs/melee/src/melee/ft/chara/ftPurin/ftPr_Init.c
        article_list_type = msl_dat_root_MslDatPurinAuxList;
        break;
    }

    if (article_profile == MSL_FIGHTER_ARTICLES_NONE) {
        if (list != UINT32_MAX) {
            fprintf(stderr,
                    "native fighter DAT has an untyped character article list\n");
            abort();
        }
        return result;
    }
    if (list == UINT32_MAX) {
        fprintf(stderr, "native fighter DAT is missing its article list\n");
        abort();
    }
    result->x48_items = translate_target(context, list, article_list_type);

    // Article.x4 is void in the decomp. The character OnLoad registrations and
    // reached item implementations are its concrete source type authority.
    // refs/melee/src/melee/ft/chara/{ftFox/ftFx_Init.c,
    //   ftFalco/ftFc_Init.c,ftSeak/ftSk_Init.c,ftPeach/ftPe_Init.c,
    //   ftZelda/ftZd_Init.c,ftLuigi/ftLg_Init.c}
    // refs/melee/src/melee/it/items/{itfoxlaser.c,itfoxblaster.c,
    //   itfoxillusion.c,itseakneedlethrown.c,itseakchain.c,
    //   itpeachturnip.c,itpeachtoadspore.c,itzeldadinfire.c,
    //   itzeldadinfireexplode.c,itluigifireball.c}
    if (result->x48_items == NULL) {
        fprintf(stderr, "native fighter DAT is missing its article list\n");
        abort();
    }
    for (i = 0; i < article_count; ++i) {
        uint32_t index = indices[i];
        uint32_t article = raw_pointer(context, list + index * 4);
        uint32_t attrs;
        if (article == UINT32_MAX || result->x48_items[index] == NULL) {
            fprintf(stderr, "native fighter DAT article %u is incomplete\n",
                    index);
            abort();
        }
        if (attr_types[i] != NULL) {
            attrs = raw_pointer(
                context, article + ARTICLE_SPECIAL_ATTRS_SOURCE_OFFSET);
            if (attrs == UINT32_MAX) {
                fprintf(stderr,
                        "native fighter DAT article %u lacks attributes\n",
                        index);
                abort();
            }
            ((Article*) result->x48_items[index])->x4_specialAttributes =
                translate_target_count(context, attrs, attr_types[i], 1);
        }
    }
    return result;
}

static MslNativePublic* find_public(MslNativeArchive* context,
                                    const char* symbol)
{
    uint32_t i;
    for (i = 0; i < context->public_count; ++i) {
        if (strcmp(context->publics[i].symbol, symbol) == 0) {
            return &context->publics[i];
        }
    }
    return NULL;
}

static void cache_public(MslNativeArchive* context, const char* symbol,
                         void* address)
{
    size_t length;
    char* symbol_copy;

    if (context->public_count == context->public_capacity) {
        uint32_t capacity = context->public_capacity == 0
                                ? 16
                                : context->public_capacity * 2;
        MslNativePublic* replacement =
            native_alloc((size_t) capacity * sizeof(*replacement));
        if (context->publics != NULL) {
            memcpy(replacement, context->publics,
                   (size_t) context->public_count * sizeof(*replacement));
        }
        context->publics = replacement;
        context->public_capacity = capacity;
    }
    length = strlen(symbol) + 1;
    symbol_copy = native_alloc(length);
    memcpy(symbol_copy, symbol, length);
    context->publics[context->public_count++] =
        (MslNativePublic) { symbol_copy, address };
}

void* msl_native_archive_get_public(HSD_Archive* archive, const char* symbol)
{
    MslNativeArchive* context = archive_context(archive);
    MslNativePublic* cached = find_public(context, symbol);
    const MslDatType* type;
    uint32_t offset;
    void* result = NULL;

    if (cached != NULL) {
        return cached->address;
    }
    if (native_dat_initialization_complete) {
        fprintf(stderr, "uncached native archive public lookup of '%s'", symbol);
    }
    require_dat_initialization("uncached native archive public lookup");
    type = public_type(symbol);
    offset = raw_public_offset(archive, symbol);
    if (offset == UINT32_MAX) {
        cache_public(context, symbol, NULL);
        return NULL;
    } else if (strcmp(symbol, "plLoadCommonData") == 0) {
        void** table = native_alloc(sizeof(*table));
        uint32_t target = raw_pointer(context, offset);
        if (target != UINT32_MAX) {
            table[0] = translate_target_count(
                context, target, msl_dat_root_pl_804D6470_t, 1);
            result = table;
        }
    } else if (strcmp(symbol, "itPublicData") == 0) {
        result = translate_item_public(context, offset);
    } else if (strcmp(symbol, "ftLoadCommonData") == 0) {
        result = translate_fighter_common_public(context, offset);
    } else if (strcmp(symbol, "ftDataFox") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftFox_DatAttrs, 327,
                                          MSL_FIGHTER_ARTICLES_FOX);
    } else if (strcmp(symbol, "ftDataCaptain") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftCaptain_DatAttrs, 318,
                                          MSL_FIGHTER_ARTICLES_NONE);
    } else if (strcmp(symbol, "ftDataSeak") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftSeakAttributes, 317,
                                          MSL_FIGHTER_ARTICLES_SHEIK);
    } else if (strcmp(symbol, "ftDataSamus") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftSs_DatAttrs, 313,
                                          MSL_FIGHTER_ARTICLES_SAMUS);
    } else if (strcmp(symbol, "ftDataPopo") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftIceClimberAttributes,
                                          321, MSL_FIGHTER_ARTICLES_ICECLIMBER);
    } else if (strcmp(symbol, "ftDataNana") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftIceClimberAttributes,
                                          321, MSL_FIGHTER_ARTICLES_ICECLIMBER);
    } else if (strcmp(symbol, "ftDataKoopa") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftKoopaAttributes,
                                          316, MSL_FIGHTER_ARTICLES_KOOPA);
    } else if (strcmp(symbol, "ftDataDonkey") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftDonkeyAttributes,
                                          337, MSL_FIGHTER_ARTICLES_NONE);
    } else if (strcmp(symbol, "ftDataGanon") == 0) {
        // Ganondorf's ext-attr blob is Captain-shaped: ftGn_Init routes
        // OnLoad/LoadSpecialAttrs through ftCa_Init_OnLoadForGanon and
        // ftCa_Init_LoadSpecialAttrs, so translate with the Captain layout.
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftCaptain_DatAttrs,
                                          318, MSL_FIGHTER_ARTICLES_NONE);
    } else if (strcmp(symbol, "ftDataPikachu") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftPikachuAttributes,
                                          320, MSL_FIGHTER_ARTICLES_PIKACHU);
    } else if (strcmp(symbol, "ftDataPichu") == 0) {
        // ftPc_Init_OnLoad -> ftPk_Init_OnLoadForPichu: Pichu shares
        // Pikachu's attribute layout and registers the same three articles
        // (thunder, ground jolt, air jolt) under the Pichu item kinds its
        // attribute block stores (xDC 82, x14 91, x18 92).
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftPikachuAttributes,
                                          320, MSL_FIGHTER_ARTICLES_PIKACHU);
    } else if (strcmp(symbol, "ftDataKirby") == 0) {
        // ftKb_Init_OnLoad pushes the single 0x424-byte ftKb_DatAttrs block
        // that holds every copy ability's tuning.
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftKb_DatAttrs, 479,
                                          MSL_FIGHTER_ARTICLES_KIRBY);
    } else if (strncmp(symbol, "ftDataKirbyCopy", 15) == 0) {
        result = translate_target_count(context, offset,
                                        msl_kirby_copy_root_type(symbol), 1);
        if (result != NULL) {
            msl_translate_kirby_copy_article_attrs(context, symbol, offset, result);
        }
    } else if (strcmp(symbol, "ftDataYoshi") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftYoshiAttributes,
                                          314, MSL_FIGHTER_ARTICLES_YOSHI);
    } else if (strcmp(symbol, "ftDataMewtwo") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftMewtwoAttributes,
                                          314, MSL_FIGHTER_ARTICLES_MEWTWO);
    } else if (strcmp(symbol, "ftDataGamewatch") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftGameWatchAttributes,
                                          ftGw_SM_Count,
                                          MSL_FIGHTER_ARTICLES_GAMEWATCH);
    } else if (strcmp(symbol, "ftDataNess") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftNessAttributes, 326,
                                          MSL_FIGHTER_ARTICLES_NESS);
    } else if (strcmp(symbol, "ftDataLink") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftLk_DatAttrs, 314,
                                          MSL_FIGHTER_ARTICLES_LINK);
    } else if (strcmp(symbol, "ftDataClink") == 0) {
        // Young Link's ext-attr blob is Link-shaped: ftCl_Init routes
        // OnLoad/LoadSpecialAttrs through ftLk_Init_OnLoadForCLink and
        // ftLk_Init_LoadSpecialAttrs, so translate with the Link layout.
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftLk_DatAttrs, 314,
                                          MSL_FIGHTER_ARTICLES_CLINK);
    } else if (strcmp(symbol, "ftDataLuigi") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftLuigiAttributes, 312,
                                          MSL_FIGHTER_ARTICLES_LUIGI);
    } else if (strcmp(symbol, "ftDataMario") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftMario_DatAttrs, 303,
                                          MSL_FIGHTER_ARTICLES_MARIO);
    } else if (strcmp(symbol, "ftDataDrmario") == 0) {
        // Doc's ext-attr blob is Mario-shaped: ftDrMarioAttributes is a
        // partial alias of ftMario_DatAttrs (x4/xC/x14 overlay specials),
        // and the shared ftMr_* specials read cape_reflection past the
        // 0x18-byte stub, so translate with the full Mario layout.
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftMario_DatAttrs, 303,
                                          MSL_FIGHTER_ARTICLES_MARIOD);
    } else if (strcmp(symbol, "ftDataMars") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_MarsAttributes, 327,
                                          MSL_FIGHTER_ARTICLES_NONE);
    } else if (strcmp(symbol, "ftDataEmblem") == 0) {
        // ftFe_Init_OnLoad -> ftMs_Init_OnLoadForRoy: Roy shares Marth's
        // attribute layout and has no articles.
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_MarsAttributes, 327,
                                          MSL_FIGHTER_ARTICLES_NONE);
    } else if (strcmp(symbol, "ftDataPeach") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftPe_DatAttrs, 318,
                                          MSL_FIGHTER_ARTICLES_PEACH);
    } else if (strcmp(symbol, "ftDataPurin") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftPurinAttributes, 327,
                                          MSL_FIGHTER_AUX_PURIN_PARTS);
    } else if (strcmp(symbol, "ftDataZelda") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftZelda_DatAttrs, 311,
                                          MSL_FIGHTER_ARTICLES_ZELDA);
    } else if (strcmp(symbol, "ftDataFalco") == 0) {
        result = translate_fighter_public(context, offset,
                                          msl_dat_root_ftFox_DatAttrs, 327,
                                          MSL_FIGHTER_ARTICLES_FALCO);
    } else if (strcmp(symbol, "map_head") == 0) {
        translate_stage_animation_tables(context, offset);
        result = translate_target_count(context, offset, type, 1);
#if MSL_GX_RECORD
        translate_stage_light_tables(context, offset, (UnkStageDat*) result);
#endif
    } else if (strcmp(symbol, "yakumono_param") == 0) {
        result = translate_stage_params(context, offset);
    } else if (strcmp(symbol, "itemdata") == 0) {
        result = translate_stage_item_public(context, offset);
#if MSL_GX_RECORD
    } else if (strcmp(symbol, "lupe") == 0 || strcmp(symbol, "tdsce") == 0 ||
               strcmp(symbol, "Stc_scemdls") == 0 ||
               strcmp(symbol, "Stc_rarwmdls") == 0 ||
               (strlen(symbol) > 13 &&
                strcmp(symbol + strlen(symbol) - 13, "_scene_models") == 0))
    {
        // The HUD's files publish null-ended lists of model descriptors (lbArchive_LoadSections hands them out as
        // DynamicModelDesc**).
        // refs/melee/src/melee/if/{ifstatus.c,ifstock.c,iftime.c,ifmagnify.c,ifnametag.c}
        uint32_t count = 0;
        while (raw_pointer(context, offset + count * 4) != UINT32_MAX) {
            count += 1;
        }
        result = translate_target_count(context, offset, msl_dat_root_MslDatModelDescList, count + 1);
    } else if (strcmp(symbol, "lbRefData") == 0) {
        // LbRf.dat: the refraction users' parameters: a count (a byte, then 3 of padding), then a pointer to count pairs of floats
        // (melee/lb/lbrefract.c::lbRefData)
        struct { int32_t count; float* params; }* data;
        uint32_t count = context->data[offset];
        uint32_t target = raw_pointer(context, offset + 4);
        uint32_t i;
        data = native_alloc(sizeof(*data));
        data->count = (int32_t) count;
        data->params = native_alloc((size_t) (count * 2 + 2) * sizeof(float));
        for (i = 0; target != UINT32_MAX && i < count * 2 && target + 4 * (i + 1) <= context->data_size; ++i) {
            uint32_t bits = read_be32(context->data + target + 4 * i);
            memcpy(&data->params[i], &bits, sizeof(float));
        }
        result = data;
    } else if (strncmp(symbol, "SIS_", 4) == 0) {
        result = translate_sis_table(context, offset);
    } else if (strcmp(symbol, "lbBgFlashColAnimData") == 0) {
        // LbBf.dat: the screen flash's color animation scripts, the fighters' color animation data type (lb_800144C8)
        result = translate_target(context, offset, msl_dat_root_Fighter_804D653C_t);
    } else if (strlen(symbol) > 11 && strcmp(symbol + strlen(symbol) - 11, "_scene_data") == 0) {
        // refs/melee/src/melee/if/ifall.c::ifAll_802F390C: the scene (cameras, lights, fogs, models)
        result = translate_target_count(context, offset, msl_dat_root_SceneDesc, 1);
        // the lights' attenuation data (a point or spot light's HSD_LightDesc.u), built as a stage's light sets are
        translate_scene_light_attenuation(context, offset, (SceneDesc*) result);
    } else if (strcmp(symbol, "map_plit") == 0) {
        result = translate_stage_lights(context, offset);
#endif
    } else if (type != NULL) {
        result = translate_target_count(context, offset, type, 1);
    }
    cache_public(context, symbol, result);
    return result;
}

char* msl_native_archive_get_extern(HSD_Archive* archive, int index)
{
    if (index < 0 || (uint32_t) index >= archive->header.nb_extern) {
        return NULL;
    }
    return archive->symbols + archive->extern_info[index].symbol;
}

void msl_native_archive_locate_extern(HSD_Archive* archive,
                                      const char* symbol, void* address)
{
    // Reached game archives resolve their externs to null during the source
    // loader. Generic graph translation treats non-relocation pointer fields
    // as null, which is the same result. A non-null external graph owner must
    // gain an explicit translation description before it is admitted.
    (void) archive;
    (void) symbol;
    if (address != NULL) {
        fprintf(stderr, "native DAT non-null external symbols are unsupported\n");
        abort();
    }
}

// The particle probe's (sysdolphin/baselib/ps_probe.c): each translated
// command's script. The translation keeps a command's header only (the
// headless core runs no particle scripts); the probe runs them, so with
// MSL_PARTICLE_PROBE set the bytes after the header are kept beside it.
typedef struct MslParticleScript {
    const HSD_PSCmdList* command;
    uint8_t* bytes;
    uint32_t size;
} MslParticleScript;
static MslParticleScript* particle_scripts;
static size_t particle_script_count;
static size_t particle_script_capacity;

static void note_particle_script(const HSD_PSCmdList* command,
                                 const uint8_t* bytes, uint32_t size)
{
    MslParticleScript* slot;
    const char* on = getenv("MSL_PARTICLE_PROBE");

#if MSL_GX_RECORD
    // (the viewer build runs the probe's library always)
    (void) on;
    if (command == NULL) {
        return;
    }
#else
    if (on == NULL || on[0] == '\0' || command == NULL) {
        return;
    }
#endif
    if (particle_script_count == particle_script_capacity) {
        particle_script_capacity =
            particle_script_capacity ? particle_script_capacity * 2 : 1024;
        particle_scripts =
            realloc(particle_scripts,
                    particle_script_capacity * sizeof(*particle_scripts));
    }
    slot = &particle_scripts[particle_script_count++];
    slot->command = command;
    slot->size = size;
    slot->bytes = malloc(size ? size : 1);
    memcpy(slot->bytes, bytes, size);
}

const uint8_t* msl_native_particle_script(const HSD_PSCmdList* command,
                                          uint32_t* size)
{
    size_t i;
    // (the newest first: an archive loaded again is translated again)
    for (i = particle_script_count; i > 0; --i) {
        if (particle_scripts[i - 1].command == command) {
            *size = particle_scripts[i - 1].size;
            return particle_scripts[i - 1].bytes;
        }
    }
    return NULL;
}

// psInitDataBankLoad's command bank: version 0 lists [0, count); versions
// 0x40..0x43 list [first, first + count) and are indexed by the full id.
// refs/melee/src/sysdolphin/baselib/particle.c::psInitDataBankLoad
static int msl_translate_particle_bank(MslNativeArchive* context, uint32_t bank,
                                       int* count, int* id_base,
                                       HSD_PSCmdList*** commands)
{
    uint32_t entries;
    uint32_t first_count;
    uint32_t total;
    uint16_t version;
    HSD_PSCmdList** result;
    uint32_t i;

    if (bank + 12 > context->data_size) {
        return -1;
    }
    version = read_be16(context->data + bank);
    first_count = read_be32(context->data + bank + 4);
    if (version == 0) {
        total = first_count;
        entries = bank + 8;
    } else if (version >= 0x40 && version <= 0x43) {
        total = first_count + read_be32(context->data + bank + 8);
        entries = bank + 12;
    } else {
        return -1;
    }
    if (total > 65536 ||
        entries + (version == 0 ? total : total - first_count) * 4 >
            context->data_size)
    {
        return -1;
    }
    *id_base = version == 0 ? 0 : (int) first_count;
    result = native_alloc((size_t) (total - *id_base) * sizeof(*result));
    for (i = (uint32_t) *id_base; i < total; ++i) {
        uint32_t relative = read_be32(
            context->data + entries +
            (version == 0 ? i : i - first_count) * 4);
        if (relative != 0) {
            // (its script runs to the next command's header, or the data's end)
            uint32_t start = bank + relative + 0x3C;
            uint32_t end = context->data_size;
            uint32_t j;
            result[i - *id_base] = translate_target_count(
                context, bank + relative, msl_dat_root_HSD_PSCmdList, 1);
            for (j = version == 0 ? 0 : first_count; j < total; ++j) {
                uint32_t other = read_be32(
                    context->data + entries +
                    (version == 0 ? j : j - first_count) * 4);
                if (other != 0 && bank + other >= start && bank + other < end) {
                    end = bank + other;
                }
            }
            if (start <= context->data_size) {
                note_particle_script(result[i - *id_base], context->data + start,
                                     end - start);
            }
        }
    }
    *count = (int) total;
    *commands = result;
    return 0;
}

int msl_native_effect_bank(HSD_Archive* archive, const char* symbol,
                           int* count, int* id_base,
                           HSD_PSCmdList*** commands)
{
    require_dat_initialization("native effect bank translation");
    MslNativeArchive* context = archive_context(archive);
    uint32_t table = raw_public_offset(archive, symbol);
    uint32_t bank;

    if (table == UINT32_MAX) {
        return -1;
    }
    if ((bank = raw_pointer(context, table)) == UINT32_MAX) {
        // A model-only effect archive carries no particle banks: both
        // leading table words are unrelocated NULLs and retail skips
        // psInitDataBank entirely (EfDkData.dat is the supported-domain
        // case). Publish an empty command bank so generator lookups
        // consume nothing, exactly like retail's absent bank.
        // refs/melee/src/melee/ef/efasync.c::efAsync_LoadSync
        if (table + 8 <= context->data_size &&
            read_be32(context->data + table) == 0 &&
            read_be32(context->data + table + 4) == 0 &&
            raw_pointer(context, table + 4) == UINT32_MAX)
        {
            *count = 0;
            *id_base = 0;
            *commands = NULL;
            return 0;
        }
        return -1;
    }
    return msl_translate_particle_bank(context, bank, count, id_base, commands);
}

int msl_native_particle_bank(HSD_Archive* archive, const char* symbol,
                             int* count, int* id_base,
                             HSD_PSCmdList*** commands)
{
    // A stage archive publishes its command bank itself as map_ptcl (not a
    // table of bank pointers): Ground_801C0800 hands it straight to
    // psInitDataBankLoad(0x1E, map_ptcl, map_texg).
    // refs/melee/src/melee/gr/ground.c::Ground_801C0800
    uint32_t bank;

    require_dat_initialization("native particle bank translation");
    bank = raw_public_offset(archive, symbol);
    if (bank == UINT32_MAX) {
        *count = 0;
        *id_base = 0;
        *commands = NULL;
        return 0;
    }
    return msl_translate_particle_bank(archive_context(archive), bank, count,
                                       id_base, commands);
}

EF_EffectDesc* msl_native_effect_models(HSD_Archive* archive,
                                        const char* symbol, int count)
{
    MslNativeArchive* context;
    uint32_t table;

    require_dat_initialization("native effect model translation");
    if (archive == NULL || symbol == NULL || count <= 0) {
        return NULL;
    }
    context = archive_context(archive);
    table = raw_public_offset(archive, symbol);
    if (table == UINT32_MAX) {
        return NULL;
    }
    // The public effect table stores particle-bank pointers in its first two
    // words, followed immediately by the EF_EffectDesc array used by
    // efLib_Create. Translate the reached prefix before the DAT arena seals.
    // refs/melee/src/melee/ef/{efasync.c::efAsync_LoadSync,
    //     eflib.c::efLib_Create}
    return translate_target_count(context, table + 8,
                                  msl_dat_root_EF_EffectDesc,
                                  (uint32_t) count);
}

#if MSL_GX_RECORD
// Viewer build: the particle texture groups of a bank (psInitDataBank's texBank: a count, then that many group pointers; a
// group is a header and texTable[num + palnum] pointers to the images and then the palettes). The game relocates them in
// place (psInitDataBankLocate); here each group is a host-width copy and each image or palette a copy of its bytes in the arena
// (so the GX layer can name them).
// refs/melee/src/sysdolphin/baselib/particle.c::{psInitDataBankLocate,psInitDataBankLoad}
// The bytes of an image of a texture format (GXGetTexBufferSize without mipmaps): whole tiles of 32 bytes (64 for RGBA8).
static uint32_t particle_image_bytes(uint32_t fmt, uint32_t width, uint32_t height)
{
    uint32_t tile_w;
    uint32_t tile_h;
    uint32_t tile_bytes = 32;

    switch (fmt) {
    case 0: case 8: case 0xE: tile_w = 8; tile_h = 8; break;      // I4, C4, CMPR
    case 1: case 2: case 9: tile_w = 8; tile_h = 4; break;        // I8, IA4, C8
    case 6: tile_w = 4; tile_h = 4; tile_bytes = 64; break;       // RGBA8
    default: tile_w = 4; tile_h = 4; break;                       // IA8, RGB565, RGB5A3, C14X2
    }
    return ((width + tile_w - 1) / tile_w) * ((height + tile_h - 1) / tile_h) * tile_bytes;
}

// The bank's own layout: every offset in it (the groups, their images and palettes) is relative to the bank's start, which the
// game adds in place (psInitDataBankLocate phase 3), so none of them are in the archive's relocation table. A group's palettes
// follow its images in texTable: one when palflag & 1, palnum of them, or one per image (formats C4, C8, C14X2 only).
static HSD_PSTexGroup** msl_translate_particle_textures(MslNativeArchive* context, uint32_t bank, int* count)
{
    uint32_t groups;
    uint32_t i;
    HSD_PSTexGroup** result;

    *count = 0;
    if (bank == UINT32_MAX || bank + 4 > context->data_size) {
        return NULL;
    }
    groups = read_be32(context->data + bank);
    if (groups > 4096 || bank + 4 + groups * 4 > context->data_size) {
        return NULL;
    }
    result = native_alloc((size_t) (groups + 1) * sizeof(*result));
    for (i = 0; i < groups; ++i) {
        uint32_t relative = read_be32(context->data + bank + 4 + i * 4);
        uint32_t group = bank + relative;
        uint32_t num;
        uint32_t fmt;
        uint32_t palnum;
        uint32_t palflag;
        uint32_t palettes = 0;
        uint32_t total;
        uint32_t j;
        HSD_PSTexGroup* copy;

        result[i] = NULL;
        if (relative == 0 || group + 0x18 > context->data_size) {
            continue;
        }
        num = read_be32(context->data + group);
        fmt = read_be32(context->data + group + 0x4);
        palnum = read_be16(context->data + group + 0x14);
        palflag = read_be16(context->data + group + 0x16);
        if (fmt == 8 || fmt == 9 || fmt == 10) {
            palettes = (palflag & 1) ? 1 : (palnum != 0 ? palnum : num);
        }
        total = num + palettes;
        if (total > 4096 || group + 0x18 + total * 4 > context->data_size) {
            continue;
        }
        copy = native_alloc(offsetof(HSD_PSTexGroup, texTable) + (size_t) (total + 1) * sizeof(copy->texTable[0]));
        copy->num = num;
        copy->fmt = fmt;
        copy->tlutfmt = read_be32(context->data + group + 0x8);
        copy->width = read_be32(context->data + group + 0xC);
        copy->height = read_be32(context->data + group + 0x10);
        copy->palnum = (u16) palnum;
        copy->palflag = (u16) palflag;
        for (j = 0; j < total; ++j) {
            uint32_t offset = read_be32(context->data + group + 0x18 + j * 4);
            uint32_t size = j < num ? particle_image_bytes(fmt, copy->width, copy->height)
                                    : (fmt == 8 ? 16u : (fmt == 9 ? 256u : 16384u)) * 2u;
            copy->texTable[j] = NULL;
            if (offset != 0 && bank + offset + size <= context->data_size) {
                uint8_t* bytes = native_alloc(size);
                memcpy(bytes, context->data + bank + offset, size);
                copy->texTable[j] = bytes;
            }
        }
        result[i] = copy;
    }
    *count = (int) groups;
    return result;
}

// An effect file's: its public table's second word points at the bank (efasync.c::efAsync_LoadSync passes it as texBank).
HSD_PSTexGroup** msl_native_effect_textures(HSD_Archive* archive, const char* symbol, int* count)
{
    MslNativeArchive* context;
    uint32_t table;

    require_dat_initialization("native effect texture translation");
    *count = 0;
    if (archive == NULL || symbol == NULL) {
        return NULL;
    }
    context = archive_context(archive);
    table = raw_public_offset(archive, symbol);
    if (table == UINT32_MAX) {
        return NULL;
    }
    return msl_translate_particle_textures(context, raw_pointer(context, table + 4), count);
}

// A stage's: map_texg is the bank itself (Ground_801C0800: psInitDataBankLoad(0x1E, map_ptcl, map_texg, 0, 0)).
HSD_PSTexGroup** msl_native_particle_textures(HSD_Archive* archive, const char* symbol, int* count)
{
    uint32_t bank;

    require_dat_initialization("native particle texture translation");
    *count = 0;
    if (archive == NULL || symbol == NULL) {
        return NULL;
    }
    bank = raw_public_offset(archive, symbol);
    return msl_translate_particle_textures(archive_context(archive), bank, count);
}
#endif
