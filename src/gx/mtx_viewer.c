/* MSL_GX_RECORD: the projection and light matrix builders of the SDK's mtx library as the decomp has them
 * (extern/dolphin/src/dolphin/mtx/{mtx44.c,mtx.c} at the pinned decomp commit; the rest of the library is
 * platform/dolphin_mtx.c), compiled only in the viewer build. */
#if MSL_GX_RECORD
#include <dolphin/mtx.h>
#include <macros.h>
#include <math.h>

void MTXFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    f32 tmp;

    ASSERTMSGLINE(0x4C, m, "MTXFrustum():  NULL Mtx44Ptr 'm' ");
    ASSERTMSGLINE(0x4D, t != b,
                  "MTXFrustum():  't' and 'b' clipping planes are equal ");
    ASSERTMSGLINE(0x4E, l != r,
                  "MTXFrustum():  'l' and 'r' clipping planes are equal ");
    ASSERTMSGLINE(0x4F, n != f,
                  "MTXFrustum():  'n' and 'f' clipping planes are equal ");
    tmp = 1 / (r - l);
    m[0][0] = (2 * n * tmp);
    m[0][1] = 0;
    m[0][2] = (tmp * (r + l));
    m[0][3] = 0;
    tmp = 1 / (t - b);
    m[1][0] = 0;
    m[1][1] = (2 * n * tmp);
    m[1][2] = (tmp * (t + b));
    m[1][3] = 0;
    m[2][0] = 0;
    m[2][1] = 0;
    tmp = 1 / (f - n);
    m[2][2] = (-n * tmp);
    m[2][3] = (tmp * -(f * n));
    m[3][0] = 0;
    m[3][1] = 0;
    m[3][2] = -1;
    m[3][3] = 0;
}

void MTXPerspective(Mtx m, f32 fovY, f32 aspect, f32 n, f32 f)
{
    f32 angle;
    f32 cot;
    f32 tmp;

    ASSERTMSGLINE(0x93, m, "MTXPerspective():  NULL Mtx44Ptr 'm' ");
    ASSERTMSGLINE(0x94, (fovY > 0.0) && (fovY < 180.0),
                  "MTXPerspective():  'fovY' out of range ");
    ASSERTMSGLINE(0x95, 0.0f != aspect, "MTXPerspective():  'aspect' is 0 ");

    angle = (0.5f * fovY);
    angle = angle * 0.017453293f;
    cot = 1 / tanf(angle);
    m[0][0] = (cot / aspect);
    m[0][1] = 0;
    m[0][2] = 0;
    m[0][3] = 0;
    m[1][0] = 0;
    m[1][1] = (cot);
    m[1][2] = 0;
    m[1][3] = 0;
    m[2][0] = 0;
    m[2][1] = 0;
    tmp = 1 / (f - n);
    m[2][2] = (-n * tmp);
    m[2][3] = (tmp * -(f * n));
    m[3][0] = 0;
    m[3][1] = 0;
    m[3][2] = -1;
    m[3][3] = 0;
}

void MTXOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    f32 tmp;

    ASSERTMSGLINE(0xDB, m, "MTXOrtho():  NULL Mtx44Ptr 'm' ");
    ASSERTMSGLINE(0xDC, t != b,
                  "MTXOrtho():  't' and 'b' clipping planes are equal ");
    ASSERTMSGLINE(0xDD, l != r,
                  "MTXOrtho():  'l' and 'r' clipping planes are equal ");
    ASSERTMSGLINE(0xDE, n != f,
                  "MTXOrtho():  'n' and 'f' clipping planes are equal ");
    tmp = 1 / (r - l);
    m[0][0] = 2 * tmp;
    m[0][1] = 0;
    m[0][2] = 0;
    m[0][3] = (tmp * -(r + l));
    tmp = 1 / (t - b);
    m[1][0] = 0;
    m[1][1] = 2 * tmp;
    m[1][2] = 0;
    m[1][3] = (tmp * -(t + b));
    m[2][0] = 0;
    m[2][1] = 0;
    tmp = 1 / (f - n);
    m[2][2] = (-1 * tmp);
    m[2][3] = (-f * tmp);
    m[3][0] = 0;
    m[3][1] = 0;
    m[3][2] = 0;
    m[3][3] = 1;
}

void C_MTXLookAt(Mtx m, Vec* camPos, Vec* camUp, Vec* target)
{
    Vec vLook;
    Vec vRight;
    Vec vUp;

    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    VECNormalize(&vLook, &vLook);

    VECCrossProduct(camUp, &vLook, &vRight);
    VECNormalize(&vRight, &vRight);
    VECCrossProduct(&vLook, &vRight, &vUp);

    m[0][0] = vRight.x;
    m[0][1] = vRight.y;
    m[0][2] = vRight.z;
    m[0][3] = -((camPos->z * vRight.z) + ((camPos->x * vRight.x) + (camPos->y * vRight.y)));

    m[1][0] = vUp.x;
    m[1][1] = vUp.y;
    m[1][2] = vUp.z;
    m[1][3] = -((camPos->z * vUp.z) + ((camPos->x * vUp.x) + (camPos->y * vUp.y)));

    m[2][0] = vLook.x;
    m[2][1] = vLook.y;
    m[2][2] = vLook.z;
    m[2][3] = -((camPos->z * vLook.z) + ((camPos->x * vLook.x) + (camPos->y * vLook.y)));
}

void MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS,
                     f32 scaleT, f32 transS, f32 transT)
{
    f32 tmp;

    ASSERTMSGLINE(0x6A2, m, "MTXLightFrustum():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(
        0x6A3, (t != b),
        "MTXLightFrustum():  't' and 'b' clipping planes are equal ");
    ASSERTMSGLINE(
        0x6A4, (l != r),
        "MTXLightFrustum():  'l' and 'r' clipping planes are equal ");

    tmp = 1 / (r - l);
    m[0][0] = (scaleS * (2 * n * tmp));
    m[0][1] = 0;
    m[0][2] = (scaleS * (tmp * (r + l))) - transS;
    m[0][3] = 0;
    tmp = 1 / (t - b);
    m[1][0] = 0;
    m[1][1] = (scaleT * (2 * n * tmp));
    m[1][2] = (scaleT * (tmp * (t + b))) - transT;
    m[1][3] = 0;
    m[2][0] = 0;
    m[2][1] = 0;
    m[2][2] = -1;
    m[2][3] = 0;
}

void MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT,
                         f32 transS, f32 transT)
{
    f32 angle;
    f32 cot;

    ASSERTMSGLINE(0x6DF, m, "MTXLightPerspective():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(0x6E0, (fovY > 0.0) && (fovY < 180.0),
                  "MTXLightPerspective():  'fovY' out of range ");
    ASSERTMSGLINE(0x6E1, 0 != aspect,
                  "MTXLightPerspective():  'aspect' is 0 ");

    angle = (0.5f * fovY);
    angle = angle * 0.017453293f;
    cot = 1 / tanf(angle);
    m[0][0] = (scaleS * (cot / aspect));
    m[0][1] = 0;
    m[0][2] = -transS;
    m[0][3] = 0;
    m[1][0] = 0;
    m[1][1] = (cot * scaleT);
    m[1][2] = -transT;
    m[1][3] = 0;
    m[2][0] = 0;
    m[2][1] = 0;
    m[2][2] = -1;
    m[2][3] = 0;
}

void MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT,
                   f32 transS, f32 transT)
{
    f32 tmp;

    ASSERTMSGLINE(0x720, m, "MTXLightOrtho():  NULL MtxPtr 'm' ");
    ASSERTMSGLINE(0x721, (t != b),
                  "MTXLightOrtho():  't' and 'b' clipping planes are equal ");
    ASSERTMSGLINE(0x722, (l != r),
                  "MTXLightOrtho():  'l' and 'r' clipping planes are equal ");
    tmp = 1 / (r - l);
    m[0][0] = (2 * tmp * scaleS);
    m[0][1] = 0;
    m[0][2] = 0;
    m[0][3] = (transS + (scaleS * (tmp * -(r + l))));
    tmp = 1 / (t - b);
    m[1][0] = 0;
    m[1][1] = (2 * tmp * scaleT);
    m[1][2] = 0;
    m[1][3] = (transT + (scaleT * (tmp * -(t + b))));
    m[2][0] = 0;
    m[2][1] = 0;
    m[2][2] = 0;
    m[2][3] = 1;
}
#endif /* MSL_GX_RECORD */
