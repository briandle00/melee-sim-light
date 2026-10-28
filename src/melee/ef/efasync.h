#ifndef MELEE_EF_EFASYNC_H
#define MELEE_EF_EFASYNC_H

#include "ef/forward.h"
#include <baselib/forward.h>

/* 063930 */ void* efAsync_Dispatch(s32 gfx_id, HSD_GObj* gobj, va_list vlist);
/* 06729C */ void efAsync_LoadAsync(int index);
/* 06730C */ void efAsync_OnLoad(HSD_Archive* archive, u8* data, u32 length,
                                 int index);
/* 06737C */ void efAsync_LoadSync(int index);
/* 06744C */ void efAsync_QueueProcessDeferred(HSD_GObj* gobj,
                                               EF_QueuedEffect* queued_effect);
/* 067624 */ void efAsync_QueueFlush(HSD_GObj* gobj, void* arg_struct);
/* 067688 */ void efAsync_QueueClear(void* arg_struct);
/* 0676F0 */ void efAsync_Spawn(HSD_GObj* gobj, void* queue_head,
                                u32 spawn_kind, u32 gfx_id, HSD_JObj* jobj,
                                ...);
/* 0676F0 */ void efAsync_QueueInit(void);

#if MSL_GX_RECORD
/* efAsync_Spawn's variable arguments, as slots of one size on every host. Retail reads f32* (or Vec3*) pointers from them, and a few decomp
 * call sites pass the float itself (promoted to a double): a 64-bit host with the Windows calling convention gives each argument an 8-byte
 * slot, a 32-bit host (WebAssembly) 4 bytes for a pointer and 8 for a double, so the callee's one reading (src/present/decomp/melee/ef/
 * efasync.c.inc) saw a different slot there. Each argument is made a 64-bit slot here: the pointer's address, the double's bits. */
static inline unsigned long long efasync_slot_ptr(const void* p)
{
    return (unsigned long long) (__UINTPTR_TYPE__) p;
}
static inline unsigned long long efasync_slot_int(long long v)
{
    return (unsigned long long) v;
}
static inline unsigned long long efasync_slot_dbl(double v)
{
    unsigned long long bits;
    __builtin_memcpy(&bits, &v, sizeof(bits));
    return bits;
}
#define EFASYNC_SLOT(x)                                                           _Generic((x), float: efasync_slot_dbl, double: efasync_slot_dbl,                       char: efasync_slot_int, signed char: efasync_slot_int,                        unsigned char: efasync_slot_int, short: efasync_slot_int,                     unsigned short: efasync_slot_int, int: efasync_slot_int,                      unsigned int: efasync_slot_int, long: efasync_slot_int,                       unsigned long: efasync_slot_int, long long: efasync_slot_int,                 unsigned long long: efasync_slot_int, default: efasync_slot_ptr)(x)
#define EFASYNC_S1(a) EFASYNC_SLOT(a)
#define EFASYNC_S2(a, ...) EFASYNC_SLOT(a), EFASYNC_S1(__VA_ARGS__)
#define EFASYNC_S3(a, ...) EFASYNC_SLOT(a), EFASYNC_S2(__VA_ARGS__)
#define EFASYNC_S4(a, ...) EFASYNC_SLOT(a), EFASYNC_S3(__VA_ARGS__)
#define EFASYNC_S5(a, ...) EFASYNC_SLOT(a), EFASYNC_S4(__VA_ARGS__)
#define EFASYNC_SEL(_1, _2, _3, _4, _5, N, ...) N
#define EFASYNC_SLOTS(...)                                                        EFASYNC_SEL(__VA_ARGS__, EFASYNC_S5, EFASYNC_S4, EFASYNC_S3, EFASYNC_S2,                 EFASYNC_S1)(__VA_ARGS__)
#define efAsync_Spawn(gobj, queue, kind, gfx, jobj, ...)                          (efAsync_Spawn)(gobj, queue, kind, gfx, jobj __VA_OPT__(, EFASYNC_SLOTS(__VA_ARGS__)))
#endif
#endif
