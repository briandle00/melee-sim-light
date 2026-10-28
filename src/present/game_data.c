/* Viewer build only (MSL_GX_RECORD): the presentation archives GameData preloads, before the native arena seals.
 *
 * The game loads its HUD archive when a versus match starts (ifall.c::ifAll_802F390C: lbArchive_80016F80(parchive, "IfAll") finds it
 * by the name without the extension, which the DVD layer completes by the language, ".usd" for the English game). The simulator
 * keeps archives by the name they were loaded with and translates a public symbol on its first lookup, which has to be before
 * GameData seals the arena: so each archive is loaded under both names and all its public symbols are looked up here. */

#if MSL_GX_RECORD

#include <string.h>

#include <baselib/archive.h>

#include "lb/lbarchive.h"
#include "platform/files.h"

HSD_Archive* msl_host_archive_find(const char* basename);
void msl_host_archive_store(const char* basename, HSD_Archive* archive);

/* symbol: the one public symbol to translate (a stage file's others are translated by the stage loader); NULL: all of them */
static void preload_symbol(const char* file, const char* game_name, const char* symbol)
{
    HSD_Archive* archive = lbArchive_LoadArchive(file);
    uint32_t i;

    if (archive == NULL) {
        return;
    }
    msl_host_archive_store(game_name, archive);
    if (symbol != NULL) {
        (void) HSD_ArchiveGetPublicAddress(archive, (char*) symbol);
        return;
    }
    for (i = 0; i < archive->header.nb_public; ++i) {
        (void) HSD_ArchiveGetPublicAddress(archive, archive->symbols + archive->public_info[i].symbol);
    }
}

static void preload(const char* file, const char* game_name)
{
    preload_symbol(file, game_name, NULL);
}

void msl_dol_load(void);
void msl_present_sis_data_init(void);
void msl_present_dol_data_init(void);

void msl_present_game_data_init(void)
{
    /* the user's own main.dol: the presentation code's tables and constants (none are in this source) */
    msl_dol_load();
    msl_present_sis_data_init();
    msl_present_dol_data_init();
    /* refs/melee/src/melee/if/ifall.c::ifAll_802F390C */
    preload("IfAll.usd", "IfAll");
    /* refs/melee/src/melee/if/ifcoget.c::un_802FF1B4 (lbArchive_80016DBC("IfCoGet.dat", .., "ScInfCgt_scene_data"): the coin-get notice's camera,
     * light and model, built with the HUD) */
    preload("IfCoGet.dat", "IfCoGet.dat");
    /* refs/melee/src/melee/lb/lbbgflash.c::lbBgFlash_80021A18 (lbArchive_LoadSymbols("LbBf.dat", ...)) */
    preload("LbBf.dat", "LbBf.dat");
    /* refs/melee/src/melee/lb/lbrefract.c::lbRefract_800222A4 (lbArchive_LoadSymbols(lbl_803BB0B0.filename, ...)) */
    preload("LbRf.dat", "LbRf.dat");
    /* refs/melee/src/melee/if/ifnametag.c (HSD_SisLib_803A62A0(2, "SdIntro.dat", "SIS_IntroData")): the name tags' font */
    preload("SdIntro.dat", "SdIntro.dat");
    /* refs/melee/src/melee/gr/grpstadium.c::grStadium_801D2BEC: the jumbotron's text, in the English game's file (GrPs.usd) */
    preload_symbol("GrPs.usd", "GrPs.usd", "SIS_GrPStadiumData");
}

#endif
