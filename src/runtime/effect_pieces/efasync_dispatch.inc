// Copied from the Melee decompilation (doldecomp/melee src/melee/ef/efasync.c, efAsync_Dispatch) for
// runtime/effect_pieces.c: compiled there with every efLib call renamed to a
// recorder. Not compiled on its own (it has no .c name).
static inline HSD_JObj* efAsync_GetEffectJObj(EF_Effect* effect)
{
    return GET_JOBJ(effect->gobj);
}

static inline void efAsync_SetEffectRotationZFromPtr(EF_Effect* effect,
                                                     const f32* rotation)
{
    HSD_JObjSetRotationZ(GET_JOBJ(effect->gobj), *rotation);
}

static inline void efAsync_SetEffectRandomRotationZ(EF_Effect* effect)
{
    HSD_JObjSetRotationZ(GET_JOBJ(effect->gobj), M_TAU * HSD_Randf());
}

static inline void efAsync_SetEffectScaleXYZ(EF_Effect* effect, f32 scale)
{
    HSD_JObj* jobj;

    jobj = effect->gobj->hsd_obj;
    (void) jobj->scale.x;
    jobj = GET_JOBJ(effect->gobj);
    HSD_JObjSetScaleX(jobj, scale);
    jobj = GET_JOBJ(effect->gobj);
    HSD_JObjSetScaleY(jobj, scale);
    jobj = GET_JOBJ(effect->gobj);
    HSD_JObjSetScaleZ(jobj, scale);
}

static inline void efAsync_SetEffectScale(EF_Effect* effect, Vec3* scale)
{
    HSD_JObjSetScale(GET_JOBJ(effect->gobj), scale);
}

static inline void efAsync_SetEffectFacingDir(EF_Effect* effect,
                                              f32 facing_dir)
{
    f64 rotation;

    if (facing_dir < 0.0f) {
        rotation = -M_PI_2;
    } else {
        rotation = M_PI_2;
    }
    HSD_JObjSetRotationY(GET_JOBJ(effect->gobj), rotation);
}

void* efAsync_Dispatch(s32 gfx_id, HSD_GObj* gobj, va_list vlist)
{
    Vec3 translate;
    Vec3 scale;
    HSD_Generator* generator;
    HSD_psAppSRT* psAppSRT;
    EF_Effect* effect;
    void* va_pos;
    void* va_jobj;
    void* ret_obj;
    f64 rot_y;
    f32 f32_2;
    f32 f32_1;
    s32 u32_2;
    s32 u32_1;
    u32 color;
    HSD_JObj* jobj_2;
    HSD_JObj* jobj_1;
    HSD_JObj* jobj_3;
    Vec3* va_vec3;
    s32 count;
    struct {
        HSD_Generator* generator;
    } state;

    ret_obj = NULL;
    switch (gfx_id) {
    case 0x3E8:
        if (HSD_Randi(8) == 0) {
            ret_obj = efLib_Create_Attach_Pos(9, gobj, va_arg(vlist, Vec3*));
        } else {
            ret_obj = efLib_Create_Attach_Pos(0xA, gobj, va_arg(vlist, Vec3*));
        }
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            f32_1 = (0.04f * (*va_arg(vlist, f32*))) + 0.3f;
            if (f32_1 < 0.3f) {
                f32_1 = 0.3f;
            }
            if (f32_1 > 1.5f) {
                f32_1 = 1.5f;
            }
            scale.x = scale.y = scale.z = f32_1;
            HSD_JObjSetScale(GET_JOBJ(((EF_Effect*) ret_obj)->gobj), &scale);
        }
        break;
    case 0x3E9:
        ret_obj = efLib_CreateGenerator(0xC, va_arg(vlist, Vec3*));
        break;
    case 0x3EA:
        ret_obj = efLib_CreateGenerator(0x14, va_arg(vlist, Vec3*));
        break;
    case 0x3EB:
        ret_obj = efLib_Create_Attach_Pos(7, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
        }
        break;
    case 0x3EC:
        ret_obj = efLib_Create_Attach_Pos(8, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectRandomRotationZ(ret_obj);
        }
        break;
    case 0x3ED:
        va_vec3 = va_arg(vlist, Vec3*);
        f32_1 = va_vec3->x;
        translate = *va_vec3;
        ret_obj = efLib_CreateGenerator(0x50, &translate);
        if (ret_obj != NULL) {
            generator = efLib_CreateGenerator_AddAppSRT(0x54);
            if (generator != NULL) {
                psAppSRT = generator->appsrt;
                psAppSRT->translate = translate;
                if (*va_arg(vlist, f32*) < 0.0f) {
                    rot_y = 0.0;
                } else {
                    rot_y = -M_PI;
                }
                psAppSRT = generator->appsrt;
                psAppSRT->rot.y = rot_y;
            }
        }
        break;
    case 0x3EE:
        ret_obj = efLib_Create_Attach_Pos(0x27, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            efAsync_SetEffectRandomRotationZ(ret_obj);
        }
        break;
    case 0x3EF:
    case 0x3F0:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        if (gfx_id == 0x3F0) {
            f32_1 = *va_arg(vlist, f32*);
        } else {
            f32_1 = -*va_arg(vlist, f32*);
        }
        ret_obj =
            efLib_CreateGenerator_Translate_FacingDir(0x42, &translate, f32_1);
        break;
    case 0x3F1:
    case 0x3F2:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        if (gfx_id == 0x3F2) {
            f32_1 = *va_arg(vlist, f32*);
        } else {
            f32_1 = -*va_arg(vlist, f32*);
        }
        ret_obj = efLib_CreateGenerator_Translate_FacingDir(0x14B, &translate,
                                                            f32_1);
        break;
    case 0x3F3:
        ret_obj = efLib_CreateGenerator(0xB, va_arg(vlist, Vec3*));
        break;
    case 0x3F4:
        ret_obj = efLib_CreateGenerator(0x48, va_arg(vlist, Vec3*));
        break;
    case 0x3F5:
        ret_obj = efLib_Create_Attach_Pos(0x10, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectFacingDir(ret_obj, *va_arg(vlist, f32*));
        }
        break;
    case 0x3F6:
        ret_obj = efLib_Create_Attach_Pos(0x11, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
        }
        break;
    case 0x3F7:
        ret_obj = efLib_Create_Attach_Pos(0x12, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectFacingDir(ret_obj, *va_arg(vlist, f32*));
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x3F8:
        ret_obj = efLib_Create_Attach_Pos(0x13, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectFacingDir(ret_obj, *va_arg(vlist, f32*));
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x3F9:
        ret_obj = efLib_Create_Attach_Pos(0x14, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectFacingDir(ret_obj, *va_arg(vlist, f32*));
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x3FA:
        ret_obj = efLib_Create_Attach_Pos(0x15, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
        }
        break;
    case 0x3FB:
        ret_obj = efLib_Create_Attach_Pos(0x16, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
        }
        break;
    case 0x3FC:
        ret_obj = efLib_Create_Attach_Pos(0x17, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
        }
        break;
    case 0x3FD:
        ret_obj = efLib_Create_Attach_Pos(3, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectFacingDir(ret_obj, *va_arg(vlist, f32*));
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x3FE:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        f32_1 = *va_arg(vlist, f32*);
        ret_obj = efLib_CreateGenerator_Translate_FacingDir(0x107, &translate,
                                                            f32_1);
        break;
    case 0x3FF:
        ret_obj = efLib_Create_Attach_Pos(5, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectFacingDir(ret_obj, *va_arg(vlist, f32*));
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x400:
    case 0x401:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        if (gfx_id == 0x401) {
            f32_1 = *va_arg(vlist, f32*);
        } else {
            f32_1 = -*va_arg(vlist, f32*);
        }
        ret_obj =
            efLib_CreateGenerator_Translate_FacingDir(0x5A, &translate, f32_1);
        break;
    case 0x402:
        ret_obj = hsd_8039EFAC(0, 0, 0x59, va_arg(vlist, HSD_JObj*));
        break;
    case 0x403:
        ret_obj = hsd_8039EFAC(0, 0, 0x5E, va_arg(vlist, HSD_JObj*));
        break;
    case 0x404:
        ret_obj = efLib_Create_Attach_Pos(0x18, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x405:
        ret_obj = efLib_CreateGenerator(0x2C, va_arg(vlist, Vec3*));
        break;
    case 0x406:
        ret_obj = efLib_Create_Attach_Pos(4, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x407:
        ret_obj = efLib_CreateGenerator(0x3C, va_arg(vlist, Vec3*));
        break;
    case 0x408: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x3E);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->rot.z = f32_1;
        }
        break;
    }
    case 0x409:
        ret_obj = hsd_8039EFAC(0, 0, 0xE2, va_arg(vlist, HSD_JObj*));
        break;
    case 0x40A: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x241);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->rot.z = f32_1;
        }
        break;
    }
    case 0x40B: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x242);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->rot.z = f32_1;
        }
        break;
    }
    case 0x40C:
        ret_obj = efLib_CreateGenerator(0x19, va_arg(vlist, Vec3*));
        break;
    case 0x40D: {
        state.generator = efLib_CreateGenerator_AddAppSRT(0x19);
        if (state.generator != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = state.generator->appsrt;
            psAppSRT->translate = *va_vec3;
            jobj_1 = GET_JOBJ(gobj);
            (void) jobj_1;
            HSD_JObjGetScale(jobj_1, &scale);
            ret_obj = state.generator;
            state.generator->appsrt->scale.x =
                state.generator->appsrt->scale.y =
                    state.generator->appsrt->scale.z = scale.y;
        }
        break;
    }
    case 0x40E:
        ret_obj = efLib_CreateGenerator(0x43, va_arg(vlist, Vec3*));
        break;
    case 0x40F:
        ret_obj = efLib_CreateGenerator(0xE3, va_arg(vlist, Vec3*));
        break;
    case 0x410:
        ret_obj = efLib_CreateGenerator(0x22A, va_arg(vlist, Vec3*));
        break;
    case 0x411:
        ret_obj = efLib_CreateGenerator(0x4B, va_arg(vlist, Vec3*));
        break;
    case 0x412:
        ret_obj = hsd_8039EFAC(0, 0, 0x13, va_arg(vlist, HSD_JObj*));
        break;
    case 0x413:
        ret_obj = hsd_8039EFAC(0, 0, 0x37, va_arg(vlist, HSD_JObj*));
        break;
    case 0x414:
        ret_obj = hsd_8039EFAC(0, 0, 0xE1, va_arg(vlist, HSD_JObj*));
        break;
    case 0x415:
        ret_obj =
            efLib_Create_AttachChild(0x25, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            efAsync_SetEffectScale(ret_obj, &scale);
        }
        break;
    case 0x416:
        ret_obj = efLib_CreateGenerator(0x196, va_arg(vlist, Vec3*));
        break;
    case 0x417:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach(0xB, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            jobj_1 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            u32_1 = va_arg(vlist, u32);
            if (jobj_1 == NULL) {
                jobj_2 = NULL;
            } else {
                jobj_2 = jobj_1->child;
            }
            if ((u32) (u32_1 + 0xFFA00000) == 0x6060U) {
                u32_2 = 0x808080;
            } else {
                u32_2 = 0xFFFFFF;
            }
            efLib_SetTevKonstColor(jobj_2, 1, u32_2, u32_1);
            ((EF_Effect*) ret_obj)->scale_flags |= EF_SCALE_INHERIT;
            efLib_SetParamGfxId(gobj, gfx_id);
            ((EF_Effect*) ret_obj)->update = efLib_Cb_ApplyStoredAlpha;
        }
        break;
    case 0x418:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach(0xC, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            jobj_1 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            u32_1 = va_arg(vlist, u32);
            if (jobj_1 == NULL) {
                jobj_2 = NULL;
            } else {
                jobj_2 = jobj_1->child;
            }
            if ((u32) (u32_1 + 0xFFA00000) == 0x6060U) {
                u32_2 = 0x808080;
            } else {
                u32_2 = 0xFFFFFF;
            }
            efLib_SetTevKonstColor(jobj_2, 0, u32_2, u32_1);
            ((EF_Effect*) ret_obj)->scale_flags |= EF_SCALE_INHERIT;
            efLib_SetParamGfxId(gobj, gfx_id);
            ((EF_Effect*) ret_obj)->update = efLib_Cb_ApplyStoredAlpha;
        }
        break;
    case 0x419: {
        HSD_JObj* child;

        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach(0xD, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            jobj_1 = efAsync_GetEffectJObj(ret_obj);
            u32_1 = va_arg(vlist, u32);
            if ((u32) (u32_1 + 0xFFA00000) == 0x6060U) {
                color = 0x808080;
            } else {
                color = 0xFFFFFF;
            }
            u32_2 = color;
            child = HSD_JObjGetChild(jobj_1);
            efLib_SetTevKonstColor(child, 0, u32_2, u32_1);
            jobj_1 = HSD_JObjGetNext(child);
            efLib_SetTevKonstColor(jobj_1, 0, u32_2, u32_1);
            jobj_1 = HSD_JObjGetNext(jobj_1);
            efLib_SetTevKonstColor(jobj_1, 0, u32_2, u32_1);
            jobj_1 = HSD_JObjGetNext(jobj_1);
            efLib_SetTevKonstColor(jobj_1, 0, u32_2, u32_1);
            jobj_1 = HSD_JObjGetNext(jobj_1);
            efLib_SetTevKonstColor(jobj_1, 0, u32_2, u32_1);
            jobj_1 = HSD_JObjGetNext(jobj_1);
            efLib_SetTevKonstColor(jobj_1, 0, u32_2, u32_1);
            jobj_1 = HSD_JObjGetNext(jobj_1);
            efLib_SetTevKonstColor(jobj_1, 0, u32_2, u32_1);
            ((EF_Effect*) ret_obj)->scale_flags |= EF_SCALE_INHERIT;
            efLib_SetParamGfxId(gobj, gfx_id);
            ((EF_Effect*) ret_obj)->update = efLib_Cb_ApplyStoredAlpha;
        }
        break;
    }
    case 0x41A:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach(0xE, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            jobj_1 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            u32_1 = va_arg(vlist, u32);
            if (jobj_1 == NULL) {
                jobj_2 = NULL;
            } else {
                jobj_2 = jobj_1->child;
            }
            if ((u32) (u32_1 + 0xFFA00000) == 0x6060U) {
                u32_2 = 0x808080;
            } else {
                u32_2 = 0xFFFFFF;
            }
            efLib_SetTevKonstColor(jobj_2, 0, u32_2, u32_1);
            ((EF_Effect*) ret_obj)->scale_flags |= EF_SCALE_INHERIT;
            efLib_SetParamGfxId(gobj, gfx_id);
            ((EF_Effect*) ret_obj)->update = efLib_Cb_ApplyStoredAlpha;
        }
        break;
    case 0x41B: {
        HSD_Generator* result;
        HSD_JObj* jobj;

        efLib_LoadKind = EF_LOADKIND_SYNC;
        result = efLib_CreateGenerator_AddAppSRT(0x31);
        if (result != NULL) {
            jobj = va_arg(vlist, HSD_JObj*);
            lb_8000B1CC(jobj, NULL, &result->appsrt->translate);
            HSD_JObjGetScale(jobj, &scale);
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = scale.y;
        }
        break;
    }
    case 0x41C:
        ret_obj = efLib_CreateGenerator(0x5D, va_arg(vlist, Vec3*));
        break;
    case 0x41D:
        ret_obj = efLib_Create_Attach_Pos(0xF, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->state_flags |= EF_STATE_ASYNC;
        }
        break;
    case 0x41E:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x55, vlist);
        break;
    case 0x41F:
        ret_obj = efLib_CreateGenerator(0x5C, va_arg(vlist, Vec3*));
        break;
    case 0x420:
        ret_obj = efLib_CreateGenerator(0x159, va_arg(vlist, Vec3*));
        break;
    case 0x421:
        ret_obj = efLib_CreateGenerator(0x3F, va_arg(vlist, Vec3*));
        break;
    case 0x422:
        ret_obj = hsd_8039EFAC(0, 0, 0x5B, va_arg(vlist, HSD_JObj*));
        break;
    case 0x423:
        ret_obj = efLib_Create_Attach(1, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x424:
        ret_obj = efLib_Create_Attach(2, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            efAsync_SetEffectRotationZFromPtr(ret_obj, va_arg(vlist, f32*));
        }
        break;
    case 0x425:
        ret_obj = efLib_CreateGenerator(0x7E, va_arg(vlist, Vec3*));
        break;
    case 0x426:
        ret_obj = efLib_CreateGenerator(0x7F, va_arg(vlist, Vec3*));
        break;
    case 0x427: {
        EF_Effect* node;
        s32 i;

        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        for (i = 0; i < 6; i++) {
            node = efLib_Create_Attach_Pos(0x1B, gobj, &translate);
            if (node == NULL) {
                break;
            }
            node->update = efLib_Cb_Fall_FromParamY;
            node->lifetime = 0x28;
            node->params.y = (0.4f * HSD_Randf()) + 2.8f;
            f32_1 = (M_TAU * HSD_Randf());
            jobj_1 = GET_JOBJ(node->gobj);
            HSD_JObjSetRotationY(jobj_1, f32_1);
            if (i != 0) {
                effect->next = node;
                effect = effect->next;
            } else {
                ret_obj = effect = node;
            }
        }
        break;
    }
    case 0x428:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0xCA, vlist);
        break;
    case 0x429:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0xCE, vlist);
        break;
    case 0x42A:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0xCF, vlist);
        break;
    case 0x42B: {
        HSD_JObj* setter_jobj;

        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach_Pos(0x19, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            jobj_1 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            setter_jobj = jobj_1;
            f32_2 = *va_arg(vlist, f32*);
            HSD_JObjSetRotationZ(setter_jobj, f32_2);
            f32_2 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_2;
            HSD_JObjSetScale(setter_jobj, &scale);
            if (jobj_1 == NULL) {
                jobj_2 = NULL;
            } else {
                jobj_2 = jobj_1->child;
            }
            efLib_SetTevKonstColor(jobj_2, 0, va_arg(vlist, u32),
                                   va_arg(vlist, u32));
        }
        break;
    }
    case 0x42C:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach_Pos(0x1A, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            f32_2 = *va_arg(vlist, f32*);
            HSD_JObjSetRotationZ(jobj_2, f32_2);
        }
        break;
    case 0x42D:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_CreateGenerator(0x121, va_arg(vlist, Vec3*));
        break;
    case 0x42E:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        f32_1 = *va_arg(vlist, f32*);
        ret_obj = efLib_CreateGenerator_Translate_FacingDir(0x13C, &translate,
                                                            f32_1);
        break;
    case 0x42F:
        ret_obj = efLib_Create_Attach_Pos(0x20, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            if (*va_arg(vlist, f32*) < 0.0f) {
                rot_y = -M_PI_2;
            } else {
                rot_y = M_PI_2;
            }
            f32_2 = rot_y;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetRotationY(jobj_2, f32_2);
        }
        break;
    case 0x430:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        f32_1 = *va_arg(vlist, f32*);
        ret_obj = efLib_CreateGenerator_Translate_FacingDir(0x140, &translate,
                                                            f32_1);
        break;
    case 0x431:
        ret_obj = efLib_Create_Attach_Pos(0x21, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            if (*va_arg(vlist, f32*) < 0.0f) {
                rot_y = -M_PI_2;
            } else {
                rot_y = M_PI_2;
            }
            f32_2 = rot_y;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetRotationY(jobj_2, f32_2);
        }
        break;
    case 0x432: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x145);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            f32_1 = *va_arg(vlist, f32*);
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x433:
        ret_obj = hsd_8039EFAC(0, 0, 0x115, va_arg(vlist, HSD_JObj*));
        break;
    case 0x434:
        ret_obj = efLib_CreateGenerator(0x14D, va_arg(vlist, Vec3*));
        break;
    case 0x435:
        u32_1 = 0x14E;
        goto block_515;
    case 0x436:
        u32_1 = 0x153;
        goto block_515;
    case 0x437:
        u32_1 = 0x156;
    block_515: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(u32_1);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            result->appsrt->rot.y = M_PI_2;
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->rot.z = f32_1;
        }
        break;
    }
    case 0x438:
        PAD_STACK(4);
        ret_obj = efLib_Create_Attach(0x22, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            jobj_3 = GET_JOBJ(gobj);
            (void) jobj_3;
            HSD_JObjGetRotationY(jobj_3);
            f32_2 = jobj_3->rotate.y;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetRotationY(jobj_2, f32_2);
            ((EF_Effect*) ret_obj)->update = efLib_Cb_SetScale_FromParamX;
            ((EF_Effect*) ret_obj)->params.x = *va_arg(vlist, f32*);
        }
        break;
    case 0x439:
        ret_obj = efLib_Create_Attach(0x23, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_2 = *va_arg(vlist, f32*);
            efAsync_SetEffectScaleXYZ(ret_obj, f32_2);
        }
        break;
    case 0x43A:
        ret_obj = efLib_CreateGenerator(0x193, va_arg(vlist, Vec3*));
        break;
    case 0x43B:
        ret_obj = efLib_CreateGenerator(0x192, va_arg(vlist, Vec3*));
        break;
    case 0x43C:
        ret_obj = efLib_CreateGenerator(0x1A0, va_arg(vlist, Vec3*));
        break;
    case 0x43D:
        ret_obj = hsd_8039EFAC(0, 0, 0x1AF, va_arg(vlist, HSD_JObj*));
        break;
    case 0x43E:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach(0x24, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_2 = *va_arg(vlist, f32*);
            efAsync_SetEffectScaleXYZ(ret_obj, f32_2);
        }
        break;
    case 0x43F: {
        HSD_Generator* result;

        efLib_LoadKind = EF_LOADKIND_SYNC;
        result = efLib_CreateGenerator_AddAppSRT(0xCA);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            f32_1 = *va_arg(vlist, f32*);
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x440:
        va_vec3 = va_arg(vlist, Vec3*);
        translate = *va_vec3;
        f32_1 = *va_arg(vlist, f32*);
        ret_obj = efLib_CreateGenerator_Translate_FacingDir(0x1D8, &translate,
                                                            f32_1);
        break;
    case 0x441:
        ret_obj = efLib_CreateGenerator(0x1FB, va_arg(vlist, Vec3*));
        break;
    case 0x442:
        ret_obj = efLib_CreateGenerator(0x1DC, va_arg(vlist, Vec3*));
        break;
    case 0x443: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x1F1);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            f32_1 = *va_arg(vlist, f32*);
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x444:
        ret_obj = efLib_CreateGenerator(0x1FF, va_arg(vlist, Vec3*));
        break;
    case 0x445:
        ret_obj = efLib_CreateGenerator(0x209, va_arg(vlist, Vec3*));
        break;
    case 0x446: {
        HSD_JObj* source_jobj = va_arg(vlist, HSD_JObj*);

        va_vec3 = va_arg(vlist, Vec3*);
        ret_obj = efLib_CreateGenerator_AppSRT_SetPos(0x1B, gobj, source_jobj,
                                                      va_vec3);
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->lifetime = 0x16;
        }
        break;
    }
    case 0x447:
        efLib_LoadKind = EF_LOADKIND_SYNC;
        ret_obj = efLib_Create_Attach_Pos(0x28, gobj, va_arg(vlist, Vec3*));
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->update = efLib_Cb_ftCo_Bury;
            if (gfx_id == 0x447) {
                f32_1 = *va_arg(vlist, f32*);
                scale.x = scale.y = scale.z = f32_1;
                jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
                HSD_JObjSetScale(jobj_2, &scale);
            }
        }
        break;
    case 0x448:
        va_jobj = va_arg(vlist, void*);
        va_pos = va_arg(vlist, void*);
        ret_obj =
            efLib_CreateGenerator_AppSRT_SetPos(0x92, gobj, va_jobj, va_pos);
        if (ret_obj != NULL) {
            ((EF_Effect*) ret_obj)->lifetime = 0xB4;
        }
        break;
    case 0x449:
        ret_obj = efLib_Create_Attach(0x26, gobj, va_arg(vlist, HSD_JObj*));
        break;
    case 0x44A:
        f32_2 = 0.5f;
        goto block_636;
    case 0x44B:
        f32_2 = 1.0f;
        goto block_636;
    case 0x44C:
        f32_2 = 2.0f;
    block_636: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x237);
        if (result != NULL) {
            va_vec3 = va_arg(vlist, Vec3*);
            psAppSRT = result->appsrt;
            psAppSRT->translate = *va_vec3;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_2;
        }
        break;
    }
    case 0x44D:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x48, vlist);
        break;
    case 0x44E:
    case 0x457:
        ret_obj = efLib_CreateGenerator_AppSRT_SetFacingDirScale(0xDA, vlist);
        break;
    case 0x44F:
    case 0x458:
        ret_obj = efLib_CreateGenerator_AppSRT_SetFacingDirScale(0xDB, vlist);
        break;
    case 0x450:
    case 0x459:
        ret_obj = efLib_CreateGenerator_AppSRT_SetFacingDirScale(0xDC, vlist);
        break;
    case 0x451:
    case 0x45A:
        ret_obj = efLib_CreateGenerator_AppSRT_SetFacingDirScale(0xDD, vlist);
        break;
    case 0x453: {
        HSD_Generator* result;

        lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL, &translate);
        result = efLib_CreateGenerator_Translate_FacingDir(
            0x234, &translate, *va_arg(vlist, f32*));
        if (result != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x454: {
        HSD_Generator* result;

        lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL, &translate);
        result = efLib_CreateGenerator_Translate_FacingDir(
            0x235, &translate, *va_arg(vlist, f32*));
        if (result != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x455: {
        HSD_Generator* result;

        lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL, &translate);
        result = efLib_CreateGenerator_Translate_FacingDir(
            0x236, &translate, *va_arg(vlist, f32*));
        if (result != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x456: {
        HSD_Generator* result;

        lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL, &translate);
        result = efLib_CreateGenerator_Translate_FacingDir(
            0x23D, &translate, *va_arg(vlist, f32*));
        if (result != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x452: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x21E);
        if (result != NULL) {
            lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL,
                        &result->appsrt->translate);
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x45B:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x8C, vlist);
        break;
    case 0x45C:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x8D, vlist);
        break;
    case 0x45D: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x23F);
        if (result != NULL) {
            lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL,
                        &result->appsrt->translate);
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x45E: {
        HSD_Generator* result;

        result = efLib_CreateGenerator_AddAppSRT(0x240);
        if (result != NULL) {
            lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL,
                        &result->appsrt->translate);
            f32_1 = *va_arg(vlist, f32*);
            ret_obj = result;
            result->appsrt->scale.x = result->appsrt->scale.y =
                result->appsrt->scale.z = f32_1;
        }
        break;
    }
    case 0x45F:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x8E, vlist);
        break;
    case 0x460:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x99, vlist);
        break;
    case 0x461:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x95, vlist);
        break;
    case 0x462:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x219, vlist);
        break;
    case 0x463:
        ret_obj = efLib_Create_Attach(0x29, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetScale(jobj_2, &scale);
        }
        break;
    case 0x464:
        ret_obj = efLib_Create_Attach(0x2A, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetScale(jobj_2, &scale);
        }
        break;
    case 0x465:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x88, vlist);
        break;
    case 0x466:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x89, vlist);
        break;
    case 0x467:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x8A, vlist);
        break;
    case 0x468:
        ret_obj = efLib_Create_Attach(0x2C, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetScale(jobj_2, &scale);
        }
        break;
    case 0x469:
        ret_obj = efLib_Create_Attach(0x2E, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetScale(jobj_2, &scale);
        }
        break;
    case 0x46A:
        generator = efLib_CreateGenerator_AddAppSRT(0xAC);
        if (generator != NULL) {
            lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL,
                        &generator->appsrt->translate);
            f32_1 = *va_arg(vlist, f32*);
            generator->appsrt->scale.x = generator->appsrt->scale.y =
                generator->appsrt->scale.z = f32_1;
            jobj_3 = GET_JOBJ(gobj);
            (void) jobj_3;
            f32_2 = HSD_JObjGetRotationY(jobj_3);
            ret_obj = generator;
            generator->appsrt->rot.y = jobj_3->rotate.y;
        }
        break;
    case 0x46B:
        ret_obj = efLib_Create_Attach(0x2B, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetScale(jobj_2, &scale);
        }
        break;
    case 0x46C:
        generator = efLib_CreateGenerator_AddAppSRT(0x9E);
        if (generator != NULL) {
            lb_8000B1CC(va_arg(vlist, HSD_JObj*), NULL,
                        &generator->appsrt->translate);
            f32_1 = *va_arg(vlist, f32*);
            generator->appsrt->scale.x = generator->appsrt->scale.y =
                generator->appsrt->scale.z = f32_1;
            jobj_3 = gobj->hsd_obj;
            (void) jobj_3;
            f32_2 = HSD_JObjGetRotationY(jobj_3);
            ret_obj = generator;
            generator->appsrt->rot.y = jobj_3->rotate.y;
        }
        break;
    case 0x46D:
        ret_obj = efLib_Create_Attach(0x2D, gobj, va_arg(vlist, HSD_JObj*));
        if (ret_obj != NULL) {
            f32_1 = *va_arg(vlist, f32*);
            scale.x = scale.y = scale.z = f32_1;
            jobj_2 = GET_JOBJ(((EF_Effect*) ret_obj)->gobj);
            HSD_JObjSetScale(jobj_2, &scale);
        }
        break;
    case 0x46E:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0xAE, vlist);
        break;
    case 0x46F:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0xA0, vlist);
        break;
    case 0x470:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x21B, vlist);
        break;
    case 0x471:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x220, vlist);
        break;
    case 0x472:
        ret_obj = efLib_CreateGenerator_AppSRT_SetScale(0x131, vlist);
        break;
    case 0x473:
        ret_obj = hsd_8039EFAC(0, 0, 0x82, va_arg(vlist, HSD_JObj*));
        break;
    case 0x477:
        ret_obj = efLib_CreateGenerator(0x7918, va_arg(vlist, Vec3*));
        break;
    case 0x474:
        ret_obj = efLib_CreateGenerator(0xF7, va_arg(vlist, Vec3*));
        break;
    case 0x475:
        ret_obj = efLib_CreateGenerator(0xFC, va_arg(vlist, Vec3*));
        break;
    case 0x476:
        ret_obj = efLib_CreateGenerator(0xFF, va_arg(vlist, Vec3*));
        break;
    }
    while (efLib_AnimCount != 0) {
        count = efLib_AnimCount - 1;
        efLib_AnimCount = count;
        HSD_JObjAnimAll(((HSD_JObj**) efLib_AnimQueue)[count]);
    }
#if 1
#else
    va_end(vlist);
#endif
    return ret_obj;
}
