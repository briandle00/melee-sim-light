/* Windows shim for the dl_iterate_phdr() image-range lookup in
   src/runtime/savestate.c. Reports exactly one "PT_LOAD segment" covering the
   PE image that contains the caller's anchor, which is what the consumer wants. */
#ifndef MSL_WIN_LINK_H
#define MSL_WIN_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "msl_win_image.h"

#define ElfW(type) MslWinElf##type

#define PT_LOAD 1

typedef uint16_t MslWinElfHalf;
typedef uintptr_t MslWinElfAddr;
typedef size_t MslWinElfXword;

typedef struct {
    uint32_t p_type;
    MslWinElfAddr p_vaddr;
    MslWinElfXword p_memsz;
} MslWinElfPhdr;

struct dl_phdr_info {
    MslWinElfAddr dlpi_addr;
    const char* dlpi_name;
    const MslWinElfPhdr* dlpi_phdr;
    MslWinElfHalf dlpi_phnum;
};

/* The consumer passes its own anchor inside `data`; every caller in this tree
   filters by an anchor it owns, so reporting only the anchor's own module is
   both sufficient and cheaper than walking the loaded-module list. */
static inline int dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t,
                                                  void*),
                                  void* data)
{
    /* `data` is an ImageRangeQuery whose first member is the anchor address. */
    const void* anchor = (const void*) *(const uintptr_t*) data;
    void* module = msl_win_module_of(anchor);
    size_t image_size = msl_win_image_size(module);
    MslWinElfPhdr segment;
    struct dl_phdr_info info;

    if (module == NULL || image_size == 0) {
        return 0;
    }
    segment.p_type = PT_LOAD;
    segment.p_vaddr = 0;
    segment.p_memsz = image_size;
    info.dlpi_addr = (MslWinElfAddr) module;
    info.dlpi_name = "";
    info.dlpi_phdr = &segment;
    info.dlpi_phnum = 1;
    return callback(&info, sizeof info, data);
}

#endif
