/* Viewer build only (MSL_GX_RECORD): the text engine's (baselib/sislib.c) game data, read from the user's own main.dol when GameData initializes
 * (present/dol_file.c); none of its bytes are in this source. The default font's glyphs (HSD_SisLib_8040CD40: I4 32x32, 512 bytes each, glyph
 * codes 0x2000..0x211E), and the two ASCII-to-glyph tables (lbl_8040C8C0 shift-JIS codes, HSD_SisLib_8040C680 glyph codes), read bytewise.
 * refs/melee/src/sysdolphin/baselib/sislib.c */

#if MSL_GX_RECORD

#include <string.h>

#include <platform.h>

const unsigned char* msl_dol_at(unsigned address, unsigned length);

enum { SIS_GLYPHS = 0x120 };

unsigned char HSD_SisLib_8040CD40[SIS_GLYPHS * 512];
unsigned int lbl_8040C8C0[144];
unsigned int HSD_SisLib_8040C680[144];

void msl_present_sis_data_init(void)
{
    memcpy(HSD_SisLib_8040CD40, msl_dol_at(0x8040CD40u, sizeof(HSD_SisLib_8040CD40)), sizeof(HSD_SisLib_8040CD40));
    memcpy(lbl_8040C8C0, msl_dol_at(0x8040C8C0u, sizeof(lbl_8040C8C0)), sizeof(lbl_8040C8C0));
    memcpy(HSD_SisLib_8040C680, msl_dol_at(0x8040C680u, sizeof(HSD_SisLib_8040C680)), sizeof(HSD_SisLib_8040C680));
}

#endif
