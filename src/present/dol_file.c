/* Viewer build only (MSL_GX_RECORD): the game's executable (main.dol) of the user's own copy, read at run time from the extracted game data
 * (data/raw/main.dol, the same place the game's other files load from). The viewer ships none of the executable's bytes: every table, string and
 * constant of it that the presentation code needs is read from here when GameData initializes (before it seals), at the address the game has it.
 * The file is a GameCube DOL: 18 section slots (7 text, 11 data) of file offset, load address and size. */

#if MSL_GX_RECORD

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <platform.h>

s32 lbFile_800163D8(const char* basename);
void lbFile_8001668C(const char* basename, u32* destination, u32* length);

static uint8_t* dol_bytes;
static uint32_t dol_size;

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/* loads main.dol (once); a missing or malformed file stops the process with a message, as a missing game file does */
void msl_dol_load(void)
{
    u32 length = 0;
    s32 size;

    if (dol_bytes != NULL) {
        return;
    }
    size = lbFile_800163D8("main.dol");
    dol_bytes = malloc((size_t) size + 16);
    lbFile_8001668C("main.dol", (u32*) (void*) dol_bytes, &length);
    dol_size = length;
    if (dol_size < 0x100) {
        fprintf(stderr, "main.dol is too small\n");
        abort();
    }
}

/* the bytes the game has at a memory address (0x80xxxxxx); NULL if the range is outside the file's sections */
const uint8_t* msl_dol_at(uint32_t address, uint32_t length)
{
    int i;

    msl_dol_load();
    for (i = 0; i < 18; ++i) {
        uint32_t offset = be32(dol_bytes + 4 * i);
        uint32_t load = be32(dol_bytes + 0x48 + 4 * i);
        uint32_t span = be32(dol_bytes + 0x90 + 4 * i);

        if (span != 0 && address >= load && address + length <= load + span && offset + (address - load) + length <= dol_size) {
            return dol_bytes + offset + (address - load);
        }
    }
    fprintf(stderr, "main.dol has no data at 0x%08X (%u bytes)\n", (unsigned) address, (unsigned) length);
    abort();
    return NULL;
}

uint32_t msl_dol_u32(uint32_t address)
{
    return be32(msl_dol_at(address, 4));
}

uint16_t msl_dol_u16(uint32_t address)
{
    const uint8_t* p = msl_dol_at(address, 2);
    return (uint16_t) ((p[0] << 8) | p[1]);
}

float msl_dol_f32(uint32_t address)
{
    uint32_t bits = msl_dol_u32(address);
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

#endif
