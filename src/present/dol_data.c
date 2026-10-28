/* Viewer build only (MSL_GX_RECORD): data of the game's executable (main.dol) that the decomp's presentation files use as externs and that no C
 * source defines. The viewer ships none of it: each value is read from the user's own main.dol (present/dol_file.c) when GameData initializes, at
 * the address in its name (.sdata/.sdata2 constants, .data tables); storage the game keeps in .bss/.sbss is zero.
 * refs/melee/src/melee/if/{ifmagnify.c,ifstatus.c}, refs/melee/src/melee/lb/lbbgflash.c */

#if MSL_GX_RECORD

#include <stdint.h>
#include <string.h>

#include <baselib/cobj.h>
#include <baselib/wobj.h>
#include <melee/if/types.h>

uint32_t msl_dol_u32(uint32_t address);
uint16_t msl_dol_u16(uint32_t address);
float msl_dol_f32(uint32_t address);
const uint8_t* msl_dol_at(uint32_t address, uint32_t length);

/* .sdata2 constants of ifmagnify.c (0x804DDB08..0x804DDB60) */
f32 ifMagnify_804DDB08;
f32 ifMagnify_804DDB28;
f32 ifMagnify_804DDB2C;
f32 ifMagnify_804DDB30;
f32 ifMagnify_804DDB34;
f32 ifMagnify_804DDB38;
f32 ifMagnify_804DDB3C;
f32 ifMagnify_804DDB4C;
int ifMagnify_804DDB60; /* (0, .sbss2: not in the file) */

/* the magnifier's camera: main.dol 0x803F97E8 with the two WObjDescs it points at (0x803F97C0: eye 0, 0, 300; 0x803F97D4: 0, 0, 0) */
static HSD_WObjDesc ifMagnify_803F97C0 = { NULL, { 0.0F, 0.0F, 300.0F }, NULL };
static HSD_WObjDesc ifMagnify_803F97D4 = { NULL, { 0.0F, 0.0F, 0.0F }, NULL };
/* (declared as the union the camera reader takes, so the reader's view of it ends inside the object) */
HSD_CObjDesc ifMagnify_803F97E8 = { .perspective = {
    NULL, 0, 3, { 0, 0x280, 0, 0x1E0 }, { 0, 0x280, 0, 0x1E0 },
    &ifMagnify_803F97C0, &ifMagnify_803F97D4, 0.0F, NULL,
    0x1.99999a0000000p-4F, 32768.0F, 0.0F, 0.0F,
} };

/* lbbgflash.c: the flash's camera (main.dol 0x803BB028, 0x38 bytes) and the two WObjDescs it points at (0x803BB000, 0x803BB014) */
static HSD_WObjDesc lbl_803BB000;
static HSD_WObjDesc lbl_803BB014;
HSD_CObjDesc lbl_803BB028;

/* ifstatus.c: the percent's color at low and at high damage (.sdata 0x804D57A8, 0x804D57AC) */
u8 ifStatus_804D57A8[4];
u8 ifStatus_804D57AC[4];

/* .bss of ifstatus.c (0x804A10C8) */
HudIndex ifStatus_HudInfo;

/* a WObjDesc as the game has it: the class name pointer, the position (+4), the robj pointer (pointers are not read: they are NULL here) */
static void read_wobj(HSD_WObjDesc* w, uint32_t address)
{
    memset(w, 0, sizeof(*w));
    w->pos.x = msl_dol_f32(address + 4);
    w->pos.y = msl_dol_f32(address + 8);
    w->pos.z = msl_dol_f32(address + 12);
}

/* a perspective camera descriptor as the game has it (HSD_CameraDescPerspective: flags +4, projection type +6, viewport +8, scissor +0x10,
 * the two WObjDescs' pointers +0x18/+0x1C, roll +0x20, near +0x28, far +0x2C, fov +0x30, aspect +0x34) */
static void read_camera(HSD_CameraDescPerspective* c, uint32_t address, HSD_WObjDesc* eye, HSD_WObjDesc* interest)
{
    memset(c, 0, sizeof(*c));
    c->flags = msl_dol_u16(address + 4);
    c->projection_type = msl_dol_u16(address + 6);
    c->viewport.xmin = (s16) msl_dol_u16(address + 8);
    c->viewport.xmax = (s16) msl_dol_u16(address + 10);
    c->viewport.ymin = (s16) msl_dol_u16(address + 12);
    c->viewport.ymax = (s16) msl_dol_u16(address + 14);
    c->scissor.left = msl_dol_u16(address + 16);
    c->scissor.right = msl_dol_u16(address + 18);
    c->scissor.top = msl_dol_u16(address + 20);
    c->scissor.bottom = msl_dol_u16(address + 22);
    c->eyepos = eye;
    c->interest = interest;
    c->roll = msl_dol_f32(address + 0x20);
    c->nnear = msl_dol_f32(address + 0x28);
    c->ffar = msl_dol_f32(address + 0x2C);
    c->fov = msl_dol_f32(address + 0x30);
    c->aspect = msl_dol_f32(address + 0x34);
}

extern unsigned char HSD_DebugFontAtlas[];

void msl_present_dol_data_init(void)
{
    /* the debug console's font (hsd_3915.c.inc's HSD_DebugFontAtlas): 128 glyphs of 56 bytes */
    memcpy(HSD_DebugFontAtlas, msl_dol_at(0x804088B8u, 128u * 56u), 128u * 56u);
    ifMagnify_804DDB08 = msl_dol_f32(0x804DDB08u);
    ifMagnify_804DDB28 = msl_dol_f32(0x804DDB28u);
    ifMagnify_804DDB2C = msl_dol_f32(0x804DDB2Cu);
    ifMagnify_804DDB30 = msl_dol_f32(0x804DDB30u);
    ifMagnify_804DDB34 = msl_dol_f32(0x804DDB34u);
    ifMagnify_804DDB38 = msl_dol_f32(0x804DDB38u);
    ifMagnify_804DDB3C = msl_dol_f32(0x804DDB3Cu);
    ifMagnify_804DDB4C = msl_dol_f32(0x804DDB4Cu);

    read_wobj(&ifMagnify_803F97C0, 0x803F97C0u);
    read_wobj(&ifMagnify_803F97D4, 0x803F97D4u);
    read_camera(&ifMagnify_803F97E8, 0x803F97E8u, &ifMagnify_803F97C0, &ifMagnify_803F97D4);

    read_wobj(&lbl_803BB000, 0x803BB000u);
    read_wobj(&lbl_803BB014, 0x803BB014u);
    read_camera(&lbl_803BB028.perspective, 0x803BB028u, &lbl_803BB000, &lbl_803BB014);

    memcpy(ifStatus_804D57A8, msl_dol_at(0x804D57A8u, 4), 4);
    memcpy(ifStatus_804D57AC, msl_dol_at(0x804D57ACu, 4), 4);
}

#endif
