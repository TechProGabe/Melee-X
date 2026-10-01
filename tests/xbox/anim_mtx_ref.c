/* anim_mtx_ref.c - sysdolphin's animation and matrix code as it was before
 * the Pentium III rewrites (fobj.c, spline.c's splGetHelmite, mtx.c's
 * HSD_MtxSRT and HSD_MtxScaledAdd at 16ad06a, "v26"), kept unchanged as the
 * reference for tests/xbox/test_anim_mtx.c: the current code must give the
 * same bits. Only the names are prefixed with ref_ (and made static), and
 * HSD_MtxSRT calls pc_sinf/pc_cosf by name, as the Xbox prelude's macros
 * made it do. Don't edit it to follow the game code: a behaviour change
 * there needs its own reference. */

/* ---- spline.c */

static f32 ref_splGetHelmite(f32 fterm, f32 time, f32 p0, f32 p1, f32 d0, f32 d1)
{
    f32 _3t2_T2;
    f32 _2t3_T3;
    f32 t3_T2;
    f32 t2_T;
    f32 t2;
    f32 _1_T2;

    _1_T2 = time * time;
    t2 = fterm * fterm;
    t2_T = _1_T2 * fterm;
    t3_T2 = t2 * (_1_T2 * time);
    _2t3_T3 = 2.0f * t3_T2 * fterm;
    _3t2_T2 = 3.0f * _1_T2 * t2;

    return (d1 * (t3_T2 - t2_T)) + ((d0 * (time + ((t3_T2 - t2_T) - t2_T))) +
                                    ((p0 * (1.0f + (_2t3_T3 - _3t2_T2))) +
                                     (p1 * (-_2t3_T3 + _3t2_T2))));
}

/* ---- fobj.c */

/* fobj.h declared it ahead of FObj_FlushKeyData */
static void ref_HSD_FObjInterpretAnim(HSD_FObj* fobj, void* obj,
                                      HSD_ObjUpdateFunc obj_update, f32 rate);

static u32 ref_HSD_FObjSetState(HSD_FObj* fobj, u32 state)
{
    if (fobj) {
        fobj->flags = (state & 0xF) | (fobj->flags & 0xF0);
    }
    return state;
}

static u32 ref_HSD_FObjGetState(HSD_FObj* fobj)
{
    if (!fobj) {
        return 0;
    }
    return fobj->flags & 0xF;
}

static inline void ref_HSD_FObjReqAnim(HSD_FObj* fobj, f32 startframe)
{
    if (fobj == NULL) {
        return;
    }

    fobj->ad = fobj->ad_head;
    fobj->time = (f32) fobj->startframe + startframe;
    fobj->op = 0;
    fobj->op_intrp = 0;
    fobj->flags &= ~0x40;
    fobj->nb_pack = 0;
    fobj->fterm = 0;
    fobj->p0 = 0.f;
    fobj->p1 = 0.f;
    fobj->d0 = 0.f;
    fobj->d1 = 0.f;
    ref_HSD_FObjSetState(fobj, 1);
}

static void ref_HSD_FObjReqAnimAll(HSD_FObj* fobj, f32 startframe)
{
    HSD_FObj* fp;

    if (fobj == NULL) {
        return;
    }

    for (fp = fobj; fp != NULL; fp = fp->next) {
        ref_HSD_FObjReqAnim(fp, startframe);
    }
}

static inline void ref_FObj_FlushKeyData(HSD_FObj* fobj, void* obj,
                                     HSD_ObjUpdateFunc obj_update, f32 rate)
{
    if (fobj->op_intrp == HSD_A_OP_KEY) {
        ref_HSD_FObjInterpretAnim(fobj, obj, obj_update, rate);
    }
}

static void ref_HSD_FObjStopAnim(HSD_FObj* fobj, void* obj, HSD_ObjUpdateFunc obj_update,
                      f32 rate)
{
    if (fobj == NULL) {
        return;
    }

    ref_FObj_FlushKeyData(fobj, obj, obj_update, rate);
    ref_HSD_FObjSetState(fobj, 0);
}

static void ref_HSD_FObjStopAnimAll(HSD_FObj* fobj, void* obj,
                         HSD_ObjUpdateFunc obj_update, f32 rate)
{
    for (; fobj != NULL; fobj = fobj->next) {
        ref_HSD_FObjStopAnim(fobj, obj, obj_update, rate);
    }
}

static f32 ref_parseFloat(u8** pos, u8 frac)
{
    union {
        f32 f;
        u32 d;
    } u;
    f32 numer;
    s32 denom;

    if (frac == HSD_A_FRAC_FLOAT) {
        u.d = (s32) ((*pos)++)[0];
        u.d |= ((*pos)++)[0] << 8;
        u.d |= ((*pos)++)[0] << 16;
        u.d |= (u32) ((*pos)++)[0] << 24;
        return u.f;
    }

    denom = (1 << (frac & 0x1F));
    switch (frac & 0xE0) {
    case HSD_A_FRAC_S8:
        numer = (s8) (*pos)[0];
        *pos += 1;
        break;
    case HSD_A_FRAC_U8:
        numer = (*pos)[0];
        *pos += 1;
        break;
    case HSD_A_FRAC_S16:
        numer = (s16) (((u16) (*pos)[1] << 8) | (*pos)[0]);
        *pos += 2;
        break;
    case HSD_A_FRAC_U16:
        numer = ((*pos)[1] << 8) | (*pos)[0];
        *pos += 2;
        break;
    default:
        return 0.0f;
    }
    return numer / denom;
}

static u8 ref_parseOpCode(u8** curr_parse)
{
    return (**curr_parse) & 0xF;
}

static u32 ref_parsePackInfo(u8** adp)
{
    u8 d;
    u32 nb_pack;
    s32 shift;

    d = *(*adp)++;
    nb_pack = ((d >> 4) & 7) + 1;
    shift = 3;
    if (!(d & 0x80)) {
        return nb_pack;
    }
    do {
        d = *(*adp)++;
        nb_pack += (d & 0x7F) << shift;
        shift += 7;
    } while (d & 0x80);
    return nb_pack;
}

static void ref_FObjLaunchKeyData(HSD_FObj* fobj)
{
    if ((fobj->flags & 0x40) != 0) {
        fobj->op_intrp = fobj->op;
        fobj->flags &= ~0x40;
        fobj->flags |= 0x80;
        fobj->p0 = fobj->p1;
    }
}

static s32 ref_parseWait(u8** adp)
{
    u8 d;
    s32 wait = 0;
    s32 shift = 0;

    do {
        d = *(*adp)++;
        wait |= (d & 0x7f) << shift;
        shift += 7;
    } while (d & 0x80);

    return wait;
}

static u32 ref_FObjLoadWait(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x16C, st == FOBJ_LOAD_WAIT);

    if ((unsigned) (fobj->ad - fobj->ad_head) >= fobj->length) {
        return 6;
    } else {
        fobj->fterm = ref_parseWait(&fobj->ad);
        fobj->flags |= 0x20;
        return ref_HSD_FObjSetState(fobj, 2);
    }
}

static u32 ref_FObjAnimCON(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x17F, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->p1 = ref_parseFloat(&fobj->ad, fobj->frac_value);
    if (fobj->op_intrp != 5) {
        fobj->d0 = fobj->d1;
        fobj->d1 = 0.0F;
    }

    return ref_HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 ref_FObjAnimLinear(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x193, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->p1 = ref_parseFloat(&fobj->ad, fobj->frac_value);
    if (fobj->op_intrp != 5) {
        fobj->d0 = fobj->d1;
        fobj->d1 = 0.0F;
    }

    return ref_HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 ref_FObjAnimSPL0(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1A7, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->d0 = fobj->d1;
    fobj->p1 = ref_parseFloat(&fobj->ad, fobj->frac_value);
    fobj->d1 = 0.0F;

    return ref_HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 ref_FObjAnimSPL(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1B9, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->p0 = fobj->p1;
    fobj->p1 = ref_parseFloat(&fobj->ad, fobj->frac_value);
    fobj->d0 = fobj->d1;
    fobj->d1 = ref_parseFloat(&fobj->ad, fobj->frac_slope);

    return ref_HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static u32 ref_FObjAnimSLP(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1CC, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    fobj->d0 = fobj->d1;
    fobj->d1 = ref_parseFloat(&fobj->ad, fobj->frac_slope);

    return ref_HSD_FObjGetState(fobj);
}

static u32 ref_FObjAnimKey(HSD_FObj* fobj)
{
    u32 st = ref_HSD_FObjGetState(fobj);
    HSD_ASSERT(0x1E9, st == FOBJ_LOAD_DATA0 || st == FOBJ_LOAD_DATA);

    ref_FObjLaunchKeyData(fobj);
    fobj->p1 = ref_parseFloat(&fobj->ad, fobj->frac_value);
    fobj->flags |= 0x40;

    return ref_HSD_FObjSetState(fobj, st == FOBJ_LOAD_DATA0 ? 3 : 4);
}

static inline u32 ref_FObjLoadData(HSD_FObj* fobj)
{
    if ((unsigned) (fobj->ad - fobj->ad_head) >= fobj->length) {
        return 6;
    } else {
        fobj->op_intrp = fobj->op;
        if (fobj->nb_pack == 0) {
            fobj->op = ref_parseOpCode(&fobj->ad);
            fobj->nb_pack = ref_parsePackInfo(&fobj->ad);
        }

        fobj->nb_pack -= 1;

        switch (fobj->op) {
        case HSD_A_OP_CON:
            return ref_FObjAnimCON(fobj);

        case HSD_A_OP_LIN:
            return ref_FObjAnimLinear(fobj);

        case HSD_A_OP_SPL0:
            return ref_FObjAnimSPL0(fobj);

        case HSD_A_OP_SPL:
            return ref_FObjAnimSPL(fobj);

        case HSD_A_OP_SLP:
            return ref_FObjAnimSLP(fobj);

        case HSD_A_OP_KEY:
            return ref_FObjAnimKey(fobj);

        default:
            return 0;
        }
    }
}

static void ref_FObjUpdateAnim(HSD_FObj* fobj, void* obj, HSD_ObjUpdateFunc obj_update)
{
    f32 phi_f0;
    HSD_ObjData fobjdata;

    if (obj_update == NULL) {
        return;
    }
    switch (fobj->op_intrp) {
    case HSD_A_OP_KEY:
        if (fobj->flags & 0x80) {
            fobjdata.fv = fobj->p0;
            fobj->flags &= 0xFFFFFF7F;
        } else {
            return;
        }
        break;
    case HSD_A_OP_CON:
        if (fobj->time >= fobj->fterm) {
            phi_f0 = fobj->p1;
        } else {
            phi_f0 = fobj->p0;
        }
        fobjdata.fv = phi_f0;
        break;
    case HSD_A_OP_LIN:
        if (fobj->flags & 0x20) {
            fobj->flags = fobj->flags & 0xFFFFFFDF;
            if (fobj->fterm != 0) {
                fobj->d0 = (fobj->p1 - fobj->p0) / fobj->fterm;
            } else {
                fobj->d0 = 0;
                fobj->p0 = fobj->p1;
            }
        }
        fobjdata.fv = fobj->d0 * fobj->time + fobj->p0;
        break;
    case HSD_A_OP_SPL0:
    case HSD_A_OP_SPL:
    case HSD_A_OP_SLP:
        if (fobj->fterm != 0) {
            fobjdata.fv =
                ref_splGetHelmite(1.0 / fobj->fterm, fobj->time, fobj->p0,
                              fobj->p1, fobj->d0, fobj->d1);
        } else {
            fobjdata.fv = fobj->p1;
        }
        break;
    default:
        break;
    }
    obj_update(obj, fobj->obj_type, &fobjdata);
}

static void ref_HSD_FObjInterpretAnim(HSD_FObj* fobj, void* obj,
                           HSD_ObjUpdateFunc obj_update, f32 rate)
{
    f32 fterm;
    u32 state;

    fterm = 0.0F;
    state = fobj != NULL ? ref_HSD_FObjGetState(fobj) : 0;
    if (state != 0 && !(fobj->time += rate, (fobj->time < 0.0))) {
        for (;;) {
            switch (state) {
            case 6: {
                fobj->time += fterm;
                ref_FObjLaunchKeyData(fobj);
                ref_FObjUpdateAnim(fobj, obj, obj_update);
                return;
            }
            case 1:
            case 2: {
                state = ref_FObjLoadData(fobj);
                break;
            }
            case 3: {
                if ((fobj->flags & 0x80) != 0) {
                    ref_FObjUpdateAnim(fobj, obj, obj_update);
                }
                state = ref_FObjLoadWait(fobj);
                break;
            }
            case 4: {
                if (fobj->fterm <= fobj->time) {
                    u8 _[8];
                    state =
#ifdef MUST_MATCH
                        state =
#endif
                            3;

                    fterm = fobj->fterm;
                    fobj->time -= fobj->fterm;
                    ref_HSD_FObjSetState(fobj, state);
                    break;
                }
                ref_FObjUpdateAnim(fobj, obj, obj_update);
                state =
#ifdef MUST_MATCH
                    state =
#endif
                        5;
                ref_HSD_FObjSetState(fobj, state);
                return;
            }
            case 5: {
                state =
#ifdef MUST_MATCH
                    state =
#endif
                        4;
                ref_HSD_FObjSetState(fobj, state);
                break;
            }
            case 0:
                return;
            }
        }
    }
}

static void ref_HSD_FObjInterpretAnimAll(void* fobj, void* obj,
                              HSD_ObjUpdateFunc obj_update, f32 rate)
{
    HSD_FObj* fobjNew = (HSD_FObj*) fobj;
    while (fobjNew != NULL) {
        ref_HSD_FObjInterpretAnim(fobjNew, obj, obj_update, rate);
        fobjNew = fobjNew->next;
    }
}

/* ---- mtx.c */

static void ref_HSD_MtxSRT(Mtx m, Vec3* vec1, Vec3* vec2, Vec3* vec3, Vec3* vec4)
{
    f32 vec1x_2;
    f32 vec1y_2;
    f32 vec1z_2;
    f32 vec1x_1;
    f32 vec1y_1;
    f32 vec1z_1;
    f32 vec1x;
    f32 vec1y;
    f32 vec1z;

    f32 sinX = pc_sinf(vec2->x);
    f32 cosX = pc_cosf(vec2->x);
    f32 sinY = pc_sinf(vec2->y);
    f32 cosY = pc_cosf(vec2->y);
    f32 sinZ = pc_sinf(vec2->z);
    f32 cosZ = pc_cosf(vec2->z);

    vec1x_2 = vec1x_1 = vec1x = vec1->x;
    vec1y_2 = vec1y_1 = vec1y = vec1->y;
    vec1z_2 = vec1z_1 = vec1z = vec1->z;

    if (vec4 != NULL) {
        f32 temp1 = 1.0 / vec4->x;
        f32 temp2 = 1.0 / vec4->y;
        f32 temp3 = 1.0 / vec4->z;

        vec1y_2 *= vec4->y * temp1;
        vec1z_2 *= vec4->z * temp1;
        vec1x_1 *= vec4->x * temp2;
        vec1z_1 *= vec4->z * temp2;
        vec1x *= vec4->x * temp3;
        vec1y *= vec4->y * temp3;
    }

    m[0][0] = cosZ * (vec1x_2 * cosY);
    m[1][0] = sinZ * (vec1x_1 * cosY);
    m[2][0] = -vec1x * sinY;
    m[0][1] = vec1y_2 * ((cosZ * (sinX * sinY)) - (cosX * sinZ));
    m[1][1] = vec1y_1 * ((sinZ * (sinX * sinY)) + (cosX * cosZ));
    m[2][1] = cosY * (vec1y * sinX);
    m[0][2] = vec1z_2 * ((cosZ * (cosX * sinY)) + (sinX * sinZ));
    m[1][2] = vec1z_1 * ((sinZ * (cosX * sinY)) - (sinX * cosZ));
    m[2][2] = cosY * (vec1z * cosX);
    m[0][3] = vec3->x;
    m[1][3] = vec3->y;
    m[2][3] = vec3->z;
}

static void ref_HSD_MtxScaledAdd(Mtx arg0, Mtx arg1, Mtx arg2, f32 arg3)
{
    f32* arr0 = (&arg0[0][0]);
    f32* arr1 = (&arg1[0][0]);
    f32* arr2 = (&arg2[0][0]);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);

    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
    *arr2++ = *arr1++ + (arg3 * *arr0++);
}
