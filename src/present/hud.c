/* Viewer build only (MSL_GX_RECORD): the HUD's glue.
 *
 * The HUD is the decomp's melee/if files, verbatim (decomp/melee/if/*.c.inc). Part not restored yet is a no-op here:
 *   - the name text of a player's name tag (GetNameText): the name-tag table below, filled from the replay the way Slippi's
 *     playback restores it (Playback/Core/RestoreGameInfo.asm).
 * refs/melee/src/melee/if/{ifall.c,ifnametag.c}, refs/melee/src/sysdolphin/baselib/sislib.c */

#if MSL_GX_RECORD

#include <string.h>
#include <baselib/sislib.h>

/* the saved language (a setting of the memory card's save): the English game (melee/lb/lblanguage.c); the Stadium screens' text asks */
bool lbLang_IsSavedLanguageJP(void)
{
    return false;
}

bool lbLang_IsSavedLanguageUS(void)
{
    return true;
}

/* The name-tag table (melee/mn/mnname.c: GetPersistentNameData(id)->namedata, 0x78 slots of Shift-JIS text, 8 two-byte characters
 * and a terminator). Slippi playback (RestoreGameInfo.asm, "RESTORE NAMETAGS") keeps the replay's game-info player byte 0xA
 * (the name-tag slot id, 0x78 = none), and for a player with a slot id copies the replay's 16 text bytes to
 * Nametag_GetNametagBlock(id) + 0x198 (namedata); a replay with no text for the slot gets 0x78. msl_present_set_nametag is that
 * restore, called by the driver before the match is built. */
#define NAMETAG_NONE 120
#define NAMEDATA_SIZE 18
static unsigned char nametag_slot_ids[4] = {NAMETAG_NONE, NAMETAG_NONE, NAMETAG_NONE, NAMETAG_NONE};
static char namedata[NAMETAG_NONE][NAMEDATA_SIZE];

void msl_present_clear_nametags(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        nametag_slot_ids[i] = NAMETAG_NONE;
    }
    memset(namedata, 0, sizeof(namedata));
}

void msl_present_set_nametag(int player_index, int slot_id, const unsigned char* text16)
{
    if (player_index < 0 || player_index > 3) {
        return;
    }
    if (slot_id >= NAMETAG_NONE || text16[0] == 0) {
        nametag_slot_ids[player_index] = NAMETAG_NONE; /* "Nametag was not backed up, give player a null nametag ID" */
        return;
    }
    nametag_slot_ids[player_index] = (unsigned char) slot_id;
    memcpy(namedata[slot_id], text16, 16);
    namedata[slot_id][16] = namedata[slot_id][17] = 0;
}

int msl_present_nametag_slot_id(int player_index)
{
    return nametag_slot_ids[player_index & 3];
}

/* melee/mn/mnname.c: returns the slot's namedata (NULL for an unused slot, as IsNameValid says) */
/* Static data the game's drawing code points the GPU at (main.dol .data, here a C array): the default font's glyph images
 * (sislib.c: HSD_SisLib_8040CD40, 0x120 glyphs of 512 bytes). The GX layer names such a block whole, with its bytes, as it does a
 * DAT allocation (msl_gx_phys), so the textures a text draw reads are in the stream's memory image. */
extern unsigned char HSD_SisLib_8040CD40[147456];

int msl_present_static_block(const void* pointer, const unsigned char** base, size_t* size)
{
    const unsigned char* p = pointer;
    if (p >= HSD_SisLib_8040CD40 && p < HSD_SisLib_8040CD40 + sizeof(HSD_SisLib_8040CD40)) {
        *base = HSD_SisLib_8040CD40;
        *size = sizeof(HSD_SisLib_8040CD40);
        return 1;
    }
    return 0;
}

char* GetNameText(int slot)
{
    if (slot < 0 || slot >= NAMETAG_NONE || namedata[slot][0] == 0) {
        return NULL;
    }
    return namedata[slot];
}

#endif
