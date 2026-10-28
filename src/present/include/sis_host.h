/* Viewer build only (MSL_GX_RECORD): the text engine's (baselib/sislib.c) reads of its byte streams on a 64-bit little-endian host.
 * A SIS stream is bytes: the multi-byte operands are big-endian, as on the console, and a "pointer" operand (the return address a
 * jump pushes, op 8/9) is a 32-bit handle of the host pointer (sis_handle / sis_resolve in sis_host.c).
 * refs/melee/src/sysdolphin/baselib/sislib.c */
#ifndef PRESENT_SIS_HOST_H
#define PRESENT_SIS_HOST_H

#include <stdint.h>
#include <platform.h>

static inline u16 sis_rd_u16(const void* p)
{
    const unsigned char* b = (const unsigned char*) p;
    return (u16) ((b[0] << 8) | b[1]);
}

static inline s16 sis_rd_s16(const void* p)
{
    return (s16) sis_rd_u16(p);
}

static inline s32 sis_rd_s32(const void* p)
{
    const unsigned char* b = (const unsigned char*) p;
    return (s32) (((u32) b[0] << 24) | ((u32) b[1] << 16) | ((u32) b[2] << 8) | (u32) b[3]);
}

u32 sis_handle(void* pointer);
unsigned char* sis_resolve(u32 handle);

#endif
