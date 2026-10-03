/* mplib_ref.c - the line tests of src/melee/mp/mplib.c as they were before
 * the per-line rejects (fps-exec 46cfa00), cut out by tools/xbox/test_mplib.py
 * and renamed ref_*: the reference tests/xbox/test_mplib.c compares the
 * current code against, bit for bit. It shares the test's globals
 * (groundCollVtx, groundCollLine, groundCollJoint, jointListStart,
 * didCheckBounding). Don't edit it to follow the game code: a behaviour
 * change there needs its own reference. */

#define mpLineGetNext ref_mpLineGetNext
#define mpLineGetPrev ref_mpLineGetPrev
#define mpRemap2d ref_mpRemap2d
#define mpLineIntersection ref_mpLineIntersection
#define mpLineIntersectionH ref_mpLineIntersectionH
#define mpLineGetCollLine ref_mpLineGetCollLine
#define mpLib_8004ED5C ref_mpLib_8004ED5C
#define mpCheckFloor ref_mpCheckFloor
#define mpCheckFloorRemap ref_mpCheckFloorRemap
#define mpLineIntersectionV ref_mpLineIntersectionV
#define mpCheckLeftWall ref_mpCheckLeftWall
#define mpCheckLeftWallRemap ref_mpCheckLeftWallRemap
#define mpCheckRightWall ref_mpCheckRightWall
#define mpCheckRightWallRemap ref_mpCheckRightWallRemap
#define mpLib_800511A4_RightWall ref_mpLib_800511A4_RightWall
#define mpLib_800515A0_LeftWall ref_mpLib_800515A0_LeftWall
#define mpCheckedBounding ref_mpCheckedBounding
#define mpBoundingCheck ref_mpBoundingCheck
#define mpBoundingCheck2 ref_mpBoundingCheck2
#define mpBoundingCheck3 ref_mpBoundingCheck3
#define mpUncheckBounding ref_mpUncheckBounding

/* ---- mplib.c at 46cfa00 */

int mpLineGetNext(int line_id);
int mpLineGetPrev(int line_id);
static void mpRemap2d(float* x_out, float* y_out, float ax0, float ay0,
                      float ax1, float ay1, float bx0, float by0, float bx1,
                      float by1, float px, float py);
bool mpLineIntersection(float a0x, float a0y, float a1x, float a1y, float b0x,
                        float b0y, float b1x, float b1y, float* int_x,
                        float* int_y);
bool mpLineIntersectionH(float* int_x, float* int_y, float a0x, float a0y,
                         float a1x, float b0x, float b0y, float b1x, float b1y);
static inline CollLine* mpLineGetCollLine(int line_id);
void mpLib_8004ED5C(int line_id, float* x0_out, float* y0_out, float* x1_out,
                    float* y1_out);
bool mpCheckFloor(float ax, float ay, float bx, float by, float y_offset,
                  Vec3* vec_out, int* line_id_out, u32* flags_out,
                  Vec3* normal_out, int line_id_skip, int joint_id_skip,
                  int joint_id_only, bool (*cb)(Fighter_GObj*, int),
                  Fighter_GObj* gobj);
bool mpCheckFloorRemap(float ax, float ay, float bx, float by, float y_offset,
                       Vec3* vec_out, int* line_id_out, u32* flags_out,
                       Vec3* normal_out, int line_id_skip, int joint_id_skip,
                       int joint_id_only, bool (*cb)(Fighter_GObj*, int),
                       Fighter_GObj* gobj);
bool mpLineIntersectionV(float* int_x, float* int_y, float a0x, float a0y,
                         float a1y, float b0x, float b0y, float b1x, float b1y);
bool mpCheckLeftWall(float ax, float ay, float bx, float by, Vec3* vec_out,
                     int* line_id_out, u32* flags_out, Vec3* normal_out,
                     int joint_id_skip, int joint_id_only);
bool mpCheckLeftWallRemap(float ax, float ay, float bx, float by,
                          Vec3* vec_out, int* line_id_out, u32* flags_out,
                          Vec3* normal_out, int joint_id_skip,
                          int joint_id_only);
bool mpCheckRightWall(float ax, float ay, float bx, float by, Vec3* vec_out,
                      int* line_id_out, u32* flags_out, Vec3* normal_out,
                      int joint_id_skip, int joint_id_only);
bool mpCheckRightWallRemap(float ax, float ay, float bx, float by,
                           Vec3* vec_out, int* line_id_out, u32* flags_out,
                           Vec3* normal_out, int joint_id_skip,
                           int joint_id_only);
bool mpLib_800511A4_RightWall(float ax, float ay, float bx, float by, float cx,
                              float cy, float dx, float dy, int* line_id_out,
                              int joint_id_skip, int joint_id_only);
bool mpLib_800515A0_LeftWall(float a0x, float a0y, float a1x, float a1y,
                             float b0x, float b0y, float b1x, float b1y,
                             int* line_id_out, int joint_id_skip,
                             int joint_id_only);
bool mpCheckedBounding(void);
void mpBoundingCheck(float left, float bottom, float right, float top);
void mpBoundingCheck2(float x1, float y1, float x2, float y2);
void mpBoundingCheck3(float x0, float y0, float x1, float y1, float x2,
                      float y2, float x3, float y3);
void mpUncheckBounding(void);

int mpLineGetNext(int line_id)
{
    s16 result = groundCollLine[line_id].x0->next_id1;
    int ret = result;

    if (result != -1) {
        u32 flags = groundCollLine[result].flags;

        if ((flags & LINE_FLAG_ENABLED) && !(flags & LINE_FLAG_HIDDEN)) {
            CollVtx* v1 = &groundCollVtx[groundCollLine[line_id].x0->v1_idx];
            CollVtx* v0 = &groundCollVtx[groundCollLine[result].x0->v0_idx];

            if (SQ(v1->pos.x - v0->pos.x) + SQ(v1->pos.y - v0->pos.y) < 4.0) {
                return ret;
            }
        }
    }

    return groundCollLine[line_id].x0->next_id0;
}

int mpLineGetPrev(int line_id)
{
    s16 result = groundCollLine[line_id].x0->prev_id1;
    int ret = result;

    if (result != -1) {
        u32 flags = groundCollLine[result].flags;

        if ((flags & LINE_FLAG_ENABLED) && !(flags & LINE_FLAG_HIDDEN)) {
            CollVtx* v0 = &groundCollVtx[groundCollLine[line_id].x0->v0_idx];
            CollVtx* v1 = &groundCollVtx[groundCollLine[result].x0->v1_idx];

            if (SQ(v0->pos.x - v1->pos.x) + SQ(v0->pos.y - v1->pos.y) < 4.0) {
                return ret;
            }
        }
    }

    return groundCollLine[line_id].x0->prev_id0;
}

static void mpRemap2d(float* x_out, float* y_out, float ax0, float ay0,
                      float ax1, float ay1, float bx0, float by0, float bx1,
                      float by1, float px, float py)
{
    double dx;
    double dy;
    double dist2;
    float f30;
    float f29;
    dx = ax1 - ax0;
    dy = ay1 - ay0;
    f30 = px - ax0;
    f29 = py - ay0;
    dist2 = (dy * dy) + (dx * dx);
    if (ABS(dist2) > 0.0001) {
        // how far along line a is point p
        double t = (dy * f29 + dx * f30) / dist2;
        if (t > 1.0) {
            t = 1.0;
        } else if (t < 0.0) {
            t = 0.0;
        }

        *x_out = px + (1.0 - t) * (bx0 - ax0) + t * (bx1 - ax1);
        *y_out = py + (1.0 - t) * (by0 - ay0) + t * (by1 - ay1);
    } else {
        *x_out = px + (bx0 - ax0) + (bx1 - ax0);
        *y_out = py + (by0 - ay0) + (by1 - ay0);
    }
}

bool mpLineIntersection(float a0x, float a0y, float a1x, float a1y, float b0x,
                        float b0y, float b1x, float b1y, float* int_x,
                        float* int_y)
{
    bool b1_below_a = false;
    bool b2_above_a = false;

    // b entirely left/right of a
    if (a0x <= a1x) {
        if ((b0x < a0x && b1x < a0x) || (a1x < b0x && a1x < b1x)) {
            return false;
        }
    } else {
        if ((b0x < a1x && b1x < a1x) || (a0x < b0x && a0x < b1x)) {
            return false;
        }
    }

    // b entirely above/below a
    if (a0y <= a1y) {
        if ((b0y < a0y && b1y < a0y) || (a1y < b0y && a1y < b1y)) {
            return false;
        }
    } else {
        if ((b0y < a1y && b1y < a1y) || (a0y < b0y && a0y < b1y)) {
            return false;
        }
    }

    {
        double ah = a1y - a0y;
        double d0x = b0x - a0x;
        double aw = a1x - a0x;
        double d0y = b0y - a0y;
        double hs_b0_a = (aw * d0y) - (ah * d0x);
        double d1y;
        double d1x;
        double det;
        double hs_b1_a;
        double bh;
        double bw;

        if (hs_b0_a < 0.0) {
            if (hs_b0_a < -0.1) {
                return false;
            }
            b1_below_a = true;
        }

        d1x = b1x - a1x;
        d1y = b1y - a1y;

        hs_b1_a = (aw * d1y) - (ah * d1x);
        if (hs_b1_a > 0.0) {
            if (hs_b1_a > 0.1) {
                return false;
            }
            b2_above_a = true;
        }

        // check if a and b are colinear
        if (hs_b0_a == 0.0 && hs_b1_a == 0.0) {
            return false;
        }

        det = (d0x * d1y) - (d0y * d1x);
        if (det < hs_b0_a) {
            if (det < hs_b1_a) {
                return false;
            }
        } else if (det > hs_b0_a) {
            if (det > hs_b1_a) {
                return false;
            }
        }

        bw = b1x - b0x;
        bh = b1y - b0y;
        if (!((bw == 0.0 && bh == 0.0) || (b1_below_a && b2_above_a) ||
              (hs_b0_a >= 0.0 && b2_above_a)))
        {
            double area = (bw * ah) - (bh * aw);

            if (ABS(area) > 0.0001F) {
                double t =
                    ((bw * d0y) - (bh * d0x)) / area; // barycentric weight
                if (t > 0.0) {
                    if (t < 1.0) {
                        *int_x = (aw * t) + a0x;
                        *int_y = (ah * t) + a0y;
                    } else {
                        *int_x = a1x;
                        *int_y = a1y;
                    }
                } else {
                    *int_x = a0x;
                    *int_y = a0y;
                }

                goto tlabel;
            }
        }
        return false;
    tlabel:
        return true;
    }
}

bool mpLineIntersectionH(float* int_x, float* int_y, float a0x, float a0y,
                         float a1x, float b0x, float b0y, float b1x, float b1y)
{
    float max_ax;
    float min_ax;
    double dbx;
    double dby;
    double new_x;
    double dx;

    if (a0x < a1x) {
        if ((b0x < a0x && b1x < a0x) || (a1x < b0x && a1x < b1x)) {
            return false;
        }
        if (b0y - a0y < -0.0001 || b1y - a0y > 0.0001) {
            return false;
        }
        min_ax = a0x;
        max_ax = a1x;
    } else {
        if ((b0x < a1x && b1x < a1x) || (a0x < b0x && a0x < b1x)) {
            return false;
        }
        if (b1y - a0y < -0.0001 || b0y - a0y > 0.0001) {
            return false;
        }
        min_ax = a1x;
        max_ax = a0x;
    }
    dby = b1y - b0y;
    dbx = b1x - b0x;
    if (ABS(dby) < 0.0001) {
        return false;
    }
    new_x = dbx / dby * (a0y - b0y) + b0x;
    dx = new_x - min_ax;
    if (dx < 0.0) {
        if (dx < -0.1) {
            return false;
        }
        new_x = min_ax;
    }
    if (new_x - max_ax > 0.0) {
        if (new_x - max_ax > 0.1) {
            return false;
        }
        new_x = max_ax;
    }
    *int_x = new_x;
    *int_y = a0y;
    return true;
}

static inline CollLine* mpLineGetCollLine(int line_id)
{
    return &groundCollLine[line_id];
}

void mpLib_8004ED5C(int line_id, float* x0_out, float* y0_out, float* x1_out,
                    float* y1_out)
{
    bool calculated_distance = false;
    CollLine* line = mpLineGetCollLine(line_id);

    int i0;
    int i1;
    float x0_f0;
    float y0_f1;
    float x1_f2;
    float y1_f3;
    float distance;

    i0 = line->x0->v0_idx;
    x0_f0 = groundCollVtx[i0].pos.x;
    y0_f1 = groundCollVtx[i0].pos.y;
    i1 = line->x0->v1_idx;
    x1_f2 = groundCollVtx[i1].pos.x;
    y1_f3 = groundCollVtx[i1].pos.y;

    if (mpLineGetPrev(line_id) != -1) {
        distance = sqrtf(SQ(x0_f0 - x1_f2) + SQ(y0_f1 - y1_f3));
        if (distance > 0.001F) {
            x0_f0 += (x0_f0 - x1_f2) / distance;
            y0_f1 += (y0_f1 - y1_f3) / distance;
        }
        calculated_distance = true;
    }

    if (mpLineGetNext(line_id) != -1) {
        if (!calculated_distance) {
            distance = sqrtf(SQ(x0_f0 - x1_f2) + SQ(y0_f1 - y1_f3));
        }
        if (distance > 0.001F) {
            x1_f2 += (x1_f2 - x0_f0) / distance;
            y1_f3 += (y1_f3 - y0_f1) / distance;
        }
    }

    *x0_out = x0_f0;
    *y0_out = y0_f1;
    *x1_out = x1_f2;
    *y1_out = y1_f3;
}

bool mpCheckFloor(float ax, float ay, float bx, float by, float y_offset,
                  Vec3* vec_out, int* line_id_out, u32* flags_out,
                  Vec3* normal_out, int line_id_skip, int joint_id_skip,
                  int joint_id_only, bool (*cb)(Fighter_GObj*, int),
                  Fighter_GObj* gobj)
{
    float min_dist2_f30;
    CollJoint* joint; // r29
    int i_r28;
    bool result_r27;
    bool already_checked;
    PAD_STACK(8);

    result_r27 = false;
    min_dist2_f30 = F32_MAX;
    already_checked = mpCheckedBounding();
    if (!already_checked) {
        mpBoundingCheck2(ax, ay, bx, by);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        CollLine* line_r26;
        int var_r25;
        int var_r24;
        MapJoint* j_inner; // r4
        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == joint - groundCollJoint ||
            (joint_id_only != -1 && joint_id_only != joint - groundCollJoint))
        {
            continue;
        }

        j_inner = joint->inner;
        i_r28 = 0;
        var_r25 = j_inner->ranges[MapLineGroup_Floor].count;
        var_r24 = j_inner->ranges[MapLineGroup_Dynamic].count;
        line_r26 = &groundCollLine[j_inner->ranges[MapLineGroup_Floor].start];
        for (; i_r28 < var_r25; i_r28 += 1, line_r26 += 1) {
            float px_sp54;
            float py_sp50;
            u8 pad[4];
            float x0_sp48;
            float y0_sp44;
            float x1_sp40;
            float y1_sp3C;
            float dist2;
            ssize_t line_offset;
        block_8:
            if (cb != NULL && !cb(gobj, line_r26 - groundCollLine)) {
                continue;
            }

            if (line_id_skip == (line_offset = (intptr_t) line_r26 -
                                               (intptr_t) groundCollLine) /
                                    (ssize_t) sizeof(CollLine))
            {
                continue;
            }

            if (!(line_r26->flags & CollLine_Floor) ||
                !(line_r26->flags & LINE_FLAG_ENABLED) ||
                line_r26->flags & LINE_FLAG_EMPTY)
            {
                continue;
            }

            mpLib_8004ED5C(line_offset / (ssize_t) sizeof(CollLine), &x0_sp48,
                           &y0_sp44, &x1_sp40, &y1_sp3C);
            y0_sp44 += y_offset;
            y1_sp3C += y_offset;
            if (ABS(y0_sp44 - y1_sp3C) > 0.0001) {
                if (mpLineIntersection(x0_sp48, y0_sp44, x1_sp40, y1_sp3C, ax,
                                       ay, bx, by, &px_sp54, &py_sp50))
                {
                    dist2 = SQ(px_sp54 - ax) + SQ(py_sp50 - ay);
                    if (min_dist2_f30 > dist2) {
                        min_dist2_f30 = dist2;
                        if (vec_out != NULL) {
                            vec_out->x = px_sp54;
                            vec_out->y = py_sp50;
                            vec_out->z = 0.0F;
                        }
                        if (line_id_out != NULL) {
                            *line_id_out = line_r26 - groundCollLine;
                        }
                        if (flags_out != NULL) {
                            *flags_out = line_r26->x0->lo_flags;
                        }
                        if (normal_out != NULL) {
                            normal_out->x = -(y1_sp3C - y0_sp44);
                            normal_out->y = x1_sp40 - x0_sp48;
                            normal_out->z = 0.0F;
                            PSVECNormalize(normal_out, normal_out);
                        }
                        result_r27 = true;
                    }
                }
            } else {
                if (ay >= by &&
                    mpLineIntersectionH(&px_sp54, &py_sp50, x0_sp48, y0_sp44,
                                        x1_sp40, ax, ay, bx, by))
                {
                    dist2 = SQ(px_sp54 - ax) + SQ(py_sp50 - ay);
                    if (min_dist2_f30 > dist2) {
                        min_dist2_f30 = dist2;
                        if (vec_out != NULL) {
                            vec_out->x = px_sp54;
                            vec_out->y = py_sp50;
                            vec_out->z = 0.0F;
                        }
                        if (line_id_out != NULL) {
                            *line_id_out = line_r26 - groundCollLine;
                        }
                        if (flags_out != NULL) {
                            *flags_out = line_r26->x0->lo_flags;
                        }
                        if (normal_out != NULL) {
                            normal_out->x = 0.0F;
                            normal_out->y = 1.0F;
                            normal_out->z = 0.0F;
                        }
                        result_r27 = true;
                    }
                }
            }
        }

        if (var_r24 != 0) {
            var_r25 = var_r24;
            i_r28 = 0;
            var_r24 = 0;
            line_r26 =
                &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                    .start];
            goto block_8;
        }
    }

    if (!already_checked) {
        mpUncheckBounding();
    }

    return result_r27;
}

bool mpCheckFloorRemap(float ax, float ay, float bx, float by, float y_offset,
                       Vec3* vec_out, int* line_id_out, u32* flags_out,
                       Vec3* normal_out, int line_id_skip, int joint_id_skip,
                       int joint_id_only, bool (*cb)(Fighter_GObj*, int),
                       Fighter_GObj* gobj)
{
    float min_dist2 = F32_MAX;
    float old_x = ax;
    float old_y = ay;
    CollJoint* joint;
    int i;
    bool result = false;
    bool already_checked;

    already_checked = mpCheckedBounding();
    if (!already_checked) {
        mpBoundingCheck2(ax, ay, bx, by);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        CollLine* line;
        int count;
        int count2;
        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == joint - groundCollJoint ||
            (joint_id_only != -1 && joint_id_only != joint - groundCollJoint))
        {
            continue;
        }

        count = joint->inner->ranges[MapLineGroup_Floor].count;
        count2 = joint->inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[joint->inner->ranges[MapLineGroup_Floor].start];
        for (i = 0; i < count; i++, line++) {
        block_8:
            if (cb != NULL && !cb(gobj, line - groundCollLine)) {
                continue;
            }

            if (line_id_skip == line - groundCollLine) {
                continue;
            }

            if (!(line->flags & CollLine_Floor) ||
                !(line->flags & LINE_FLAG_ENABLED) ||
                line->flags & LINE_FLAG_EMPTY)
            {
                continue;
            }

            {
                CollVtx* v0_r5 = &groundCollVtx[line->x0->v0_idx];
                CollVtx* v1_r6 = &groundCollVtx[line->x0->v1_idx];
                float x0 = groundCollVtx[line->x0->v0_idx].pos.x;
                float y0 = y_offset + groundCollVtx[line->x0->v0_idx].pos.y;
                float x1 = groundCollVtx[line->x0->v1_idx].pos.x;
                float y1 = y_offset + groundCollVtx[line->x0->v1_idx].pos.y;
                float dx;
                float dy;
                float dx2;
                float dy2;
                float dist2;
                float int_x;
                float int_y;
                PAD_STACK(4);

                if (joint->flags &
                    (CollJoint_B10 | CollJoint_B9 | CollJoint_B8))
                {
                    mpRemap2d(&ax, &ay, groundCollVtx[line->x0->v0_idx].x10,
                              groundCollVtx[line->x0->v0_idx].x14,
                              groundCollVtx[line->x0->v1_idx].x10,
                              groundCollVtx[line->x0->v1_idx].x14, x0, y0, x1,
                              y1, old_x, old_y);
                } else {
                    ax = old_x;
                    ay = old_y;
                }

                dx = bx - ax;
                dy = by - ay;

                if (ABS(y0 - y1) > 0.0001) {
                    if (mpLineIntersection(x0, y0, x1, y1, ax, ay, bx, by,
                                           &int_x, &int_y))
                    {
                        dx2 = SQ(int_x - old_x);
                        dy2 = SQ(int_y - old_y);
                        dist2 = dx2 + dy2;

                        if (dx * (int_x - old_x) + dy * (int_y - old_y) < 0.0F)
                        {
                            dist2 = -dist2;
                        }

                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;
                            if (vec_out) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }
                            if (line_id_out) {
                                *line_id_out = line - groundCollLine;
                            }
                            if (flags_out) {
                                *flags_out = line->x0->lo_flags;
                            }
                            if (normal_out) {
                                normal_out->x = -(y1 - y0);
                                normal_out->y = x1 - x0;
                                normal_out->z = 0.0F;
                                PSVECNormalize(normal_out, normal_out);
                            }
                            result = true;
                        }
                    }
                } else if (ay >= by &&
                           mpLineIntersectionH(&int_x, &int_y, x0, y0, x1, ax,
                                               ay, bx, by))
                {
                    dx2 = SQ(int_x - old_x);
                    dy2 = SQ(int_y - old_y);
                    dist2 = dx2 + dy2;

                    if (dx * (int_x - old_x) + dy * (int_y - old_y) < 0.0F) {
                        dist2 = -dist2;
                    }

                    if (min_dist2 > dist2) {
                        min_dist2 = dist2;
                        if (vec_out) {
                            vec_out->x = int_x;
                            vec_out->y = int_y;
                            vec_out->z = 0.0F;
                        }
                        if (line_id_out) {
                            *line_id_out = line - groundCollLine;
                        }
                        if (flags_out) {
                            *flags_out = line->x0->lo_flags;
                        }
                        if (normal_out) {
                            normal_out->x = 0.0F;
                            normal_out->y = 1.0F;
                            normal_out->z = 0.0F;
                        }
                        result = true;
                    }
                }
            }
        }

        if (count2 != 0) {
            count = count2;
            i = 0;
            count2 = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }

    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpLineIntersectionV(float* int_x, float* int_y, float a0x, float a0y,
                         float a1y, float b0x, float b0y, float b1x, float b1y)
{
    float min_ay;
    float max_ay;
    double dbx;
    double dby;
    double new_y;
    double dy;

    if (a0y < a1y) {
        if ((b0y < a0y && b1y < a0y) || (a1y < b0y && a1y < b1y)) {
            return false;
        }
        if (b1x - a0x < -0.0001 || b0x - a0x > 0.0001) {
            return false;
        }
        min_ay = a0y;
        max_ay = a1y;
    } else {
        if ((b0y < a1y && b1y < a1y) || (a0y < b0y && a0y < b1y)) {
            return false;
        }
        if (b0x - a0x < -0.0001 || b1x - a0x > 0.0001) {
            return false;
        }
        min_ay = a1y;
        max_ay = a0y;
    }
    dby = b1y - b0y;
    dbx = b1x - b0x;
    if (ABS(dbx) < 0.0001) {
        return false;
    }
    new_y = (dby / dbx * (a0x - b0x)) + b0y;
    dy = new_y - min_ay;
    if (dy < 0.0) {
        if (dy < -0.1) {
            return false;
        }
        new_y = min_ay;
    }
    dy = new_y - max_ay;
    if (dy > 0.0) {
        if (dy > 0.1) {
            return false;
        }
        new_y = max_ay;
    }
    *int_x = a0x;
    *int_y = new_y;
    return true;
}

bool mpCheckLeftWall(float ax, float ay, float bx, float by, Vec3* vec_out,
                     int* line_id_out, u32* flags_out, Vec3* normal_out,
                     int joint_id_skip, int joint_id_only)
{
    float min_dist2;
    CollJoint* joint;
    int i;
    bool result;
    CollLine* line;
    int count;
    int dynamic_count;
    MapJoint* j_inner;
    bool already_checked;
    PAD_STACK(4);

    result = false;
    min_dist2 = F32_MAX;
    already_checked = mpCheckedBounding();

    if (!already_checked) {
        mpBoundingCheck2(ax, ay, bx, by);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == (joint - groundCollJoint) ||
            !(joint_id_only == -1 ||
              joint_id_only == (joint - groundCollJoint)))
        {
            continue;
        }

        j_inner = joint->inner;

        count = j_inner->ranges[MapLineGroup_LeftWall].count;
        dynamic_count = j_inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[j_inner->ranges[MapLineGroup_LeftWall].start];

        for (i = 0; i < count; i++, line++) {
        block_8:
            if (line->flags & CollLine_LeftWall &&
                line->flags & LINE_FLAG_ENABLED &&
                !(line->flags & LINE_FLAG_EMPTY))
            {
                MapLine* inner = line->x0;
                CollVtx* v0 = &groundCollVtx[inner->v0_idx];
                CollVtx* v1 = &groundCollVtx[inner->v1_idx];
                float x0 = v0->pos.x;
                float y0 = v0->pos.y;
                float x1 = v1->pos.x;
                float y1 = v1->pos.y;
                float dx2;
                float dy2;
                float dist2;
                float int_x;
                float int_y;
                if (ABS(x0 - x1) > 0.0001) {
                    if (mpLineIntersection(x0, y0, x1, y1, ax, ay, bx, by,
                                           &int_x, &int_y))
                    {
                        dx2 = SQ(int_x - ax);
                        dy2 = SQ(int_y - ay);
                        dist2 = dx2 + dy2;
                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;
                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }
                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }
                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }
                            if (normal_out != NULL) {
                                normal_out->x = -(y1 - y0);
                                normal_out->y = x1 - x0;
                                normal_out->z = 0.0F;
                                PSVECNormalize(normal_out, normal_out);
                            }
                            result = true;
                        }
                    }
                } else {
                    if ((ax <= bx) &&
                        mpLineIntersectionV(&int_x, &int_y, x0, y0, y1, ax, ay,
                                            bx, by))
                    {
                        dx2 = SQ(int_x - ax);
                        dy2 = SQ(int_y - ay);
                        dist2 = dx2 + dy2;
                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;
                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }
                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }
                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }
                            if (normal_out != NULL) {
                                normal_out->x = -1.0F;
                                normal_out->y = 0.0F;
                                normal_out->z = 0.0F;
                            }
                            result = true;
                        }
                    }
                }
            }
        }

        if (dynamic_count != 0) {
            count = dynamic_count;
            i = 0;
            dynamic_count = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }
    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpCheckLeftWallRemap(float ax, float ay, float bx, float by,
                          Vec3* vec_out, int* line_id_out, u32* flags_out,
                          Vec3* normal_out, int joint_id_skip,
                          int joint_id_only)
{
    float min_dist2;
    float old_x = ax;
    float old_y = ay;
    CollJoint* joint;
    int i;
    bool result;
    CollLine* line;
    int count;
    int dynamic_count;
    MapJoint* j_inner;
    bool already_checked;
    PAD_STACK(8);

    result = false;
    min_dist2 = F32_MAX;

    already_checked = mpCheckedBounding();
    if (!already_checked) {
        mpBoundingCheck2(ax, ay, bx, by);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == (joint - groundCollJoint) ||
            !(joint_id_only == -1 ||
              joint_id_only == (joint - groundCollJoint)))
        {
            continue;
        }

        j_inner = joint->inner;
        count = j_inner->ranges[MapLineGroup_LeftWall].count;
        dynamic_count = j_inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[j_inner->ranges[MapLineGroup_LeftWall].start];
        for (i = 0; i < count; i++, line++) {
        block_8:
            if (line->flags & CollLine_LeftWall &&
                line->flags & LINE_FLAG_ENABLED &&
                !(line->flags & LINE_FLAG_EMPTY))
            {
                float x0 = groundCollVtx[line->x0->v0_idx].pos.x;
                float y0 = groundCollVtx[line->x0->v0_idx].pos.y;
                float x1 = groundCollVtx[line->x0->v1_idx].pos.x;
                float y1 = groundCollVtx[line->x0->v1_idx].pos.y;
                float dx;
                float dy;
                float dx2;
                float dy2;
                float dist2;
                float int_x;
                float int_y;

                if (joint->flags &
                    (CollJoint_B10 | CollJoint_B9 | CollJoint_B8))
                {
                    mpRemap2d(&ax, &ay, groundCollVtx[line->x0->v0_idx].x10,
                              groundCollVtx[line->x0->v0_idx].x14,
                              groundCollVtx[line->x0->v1_idx].x10,
                              groundCollVtx[line->x0->v1_idx].x14, x0, y0, x1,
                              y1, old_x, old_y);
                } else {
                    ax = old_x;
                    ay = old_y;
                }

                dx = bx - ax;
                dy = by - ay;
                if (ABS(x0 - x1) > 0.0001) {
                    if (mpLineIntersection(x0, y0, x1, y1, ax, ay, bx, by,
                                           &int_x, &int_y))
                    {
                        dx2 = SQ(int_x - old_x);
                        dy2 = SQ(int_y - old_y);
                        dist2 = dx2 + dy2;
                        if ((dx * (int_x - old_x)) + (dy * (int_y - old_y)) <
                            0.0F)
                        {
                            dist2 = -dist2;
                        }

                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;

                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }

                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }

                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }

                            if (normal_out != NULL) {
                                normal_out->x = -(y1 - y0);
                                normal_out->y = x1 - x0;
                                normal_out->z = 0.0F;
                                PSVECNormalize(normal_out, normal_out);
                            }

                            result = true;
                        }
                    }
                } else {
                    if (ax <= bx && mpLineIntersectionV(&int_x, &int_y, x0, y0,
                                                        y1, ax, ay, bx, by))
                    {
                        dx2 = SQ(int_x - old_x);
                        dy2 = SQ(int_y - old_y);
                        dist2 = dx2 + dy2;
                        if ((dx * (int_x - old_x)) + (dy * (int_y - old_y)) <
                            0.0F)
                        {
                            dist2 = -dist2;
                        }

                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;

                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }

                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }

                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }

                            if (normal_out != NULL) {
                                normal_out->x = -1.0F;
                                normal_out->y = 0.0F;
                                normal_out->z = 0.0F;
                            }

                            result = true;
                        }
                    }
                }
            }
        }

        if (dynamic_count != 0) {
            count = dynamic_count;
            i = 0;
            dynamic_count = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }

    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpCheckRightWall(float ax, float ay, float bx, float by, Vec3* vec_out,
                      int* line_id_out, u32* flags_out, Vec3* normal_out,
                      int joint_id_skip, int joint_id_only)
{
    float min_dist2;
    CollJoint* joint;
    int i;
    bool result;
    CollLine* line;
    int count;
    int dynamic_count;
    MapJoint* j_inner;
    bool already_checked;
    PAD_STACK(4);

    result = false;
    min_dist2 = F32_MAX;
    already_checked = mpCheckedBounding();

    if (!already_checked) {
        mpBoundingCheck2(ax, ay, bx, by);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == (joint - groundCollJoint) ||
            !(joint_id_only == -1 ||
              joint_id_only == (joint - groundCollJoint)))
        {
            continue;
        }

        j_inner = joint->inner;

        count = j_inner->ranges[MapLineGroup_RightWall].count;
        dynamic_count = j_inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[j_inner->ranges[MapLineGroup_RightWall].start];

        for (i = 0; i < count; i++, line++) {
        block_8:
            if (line->flags & CollLine_RightWall &&
                line->flags & LINE_FLAG_ENABLED &&
                !(line->flags & LINE_FLAG_EMPTY))
            {
                MapLine* inner = line->x0;
                CollVtx* v0 = &groundCollVtx[inner->v0_idx];
                CollVtx* v1 = &groundCollVtx[inner->v1_idx];
                float x0 = v0->pos.x;
                float y0 = v0->pos.y;
                float x1 = v1->pos.x;
                float y1 = v1->pos.y;
                float dx2;
                float dy2;
                float dist2;
                float int_x;
                float int_y;
                if (ABS(x0 - x1) > 0.0001) {
                    if (mpLineIntersection(x0, y0, x1, y1, ax, ay, bx, by,
                                           &int_x, &int_y))
                    {
                        dx2 = SQ(int_x - ax);
                        dy2 = SQ(int_y - ay);
                        dist2 = dx2 + dy2;
                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;
                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }
                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }
                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }
                            if (normal_out != NULL) {
                                normal_out->x = -(y1 - y0);
                                normal_out->y = x1 - x0;
                                normal_out->z = 0.0F;
                                PSVECNormalize(normal_out, normal_out);
                            }
                            result = true;
                        }
                    }
                } else {
                    if (ax >= bx && mpLineIntersectionV(&int_x, &int_y, x0, y0,
                                                        y1, ax, ay, bx, by))
                    {
                        dx2 = SQ(int_x - ax);
                        dy2 = SQ(int_y - ay);
                        dist2 = dx2 + dy2;
                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;
                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }
                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }
                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }
                            if (normal_out != NULL) {
                                normal_out->x = 1.0F;
                                normal_out->y = 0.0F;
                                normal_out->z = 0.0F;
                            }
                            result = true;
                        }
                    }
                }
            }
        }

        if (dynamic_count != 0) {
            count = dynamic_count;
            i = 0;
            dynamic_count = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }
    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpCheckRightWallRemap(float ax, float ay, float bx, float by,
                           Vec3* vec_out, int* line_id_out, u32* flags_out,
                           Vec3* normal_out, int joint_id_skip,
                           int joint_id_only)
{
    float min_dist2;
    float old_x = ax;
    float old_y = ay;
    CollJoint* joint;
    int i;
    int result;
    bool already_checked;
    PAD_STACK(8);

    result = false;
    min_dist2 = F32_MAX;

    already_checked = mpCheckedBounding();
    if (!already_checked) {
        mpBoundingCheck2(ax, ay, bx, by);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        CollLine* line;
        int count;
        int dynamic_count;
        MapJoint* j_inner;

        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == (joint - groundCollJoint) ||
            !(joint_id_only == -1 ||
              joint_id_only == (joint - groundCollJoint)))
        {
            continue;
        }

        j_inner = joint->inner;
        count = j_inner->ranges[MapLineGroup_RightWall].count;
        dynamic_count = j_inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[j_inner->ranges[MapLineGroup_RightWall].start];
        for (i = 0; i < count; i++, line++) {
        block_8:
            if (line->flags & CollLine_RightWall &&
                line->flags & LINE_FLAG_ENABLED &&
                !(line->flags & LINE_FLAG_EMPTY))
            {
                float x0 = groundCollVtx[line->x0->v0_idx].pos.x;
                float y0 = groundCollVtx[line->x0->v0_idx].pos.y;
                float x1 = groundCollVtx[line->x0->v1_idx].pos.x;
                float y1 = groundCollVtx[line->x0->v1_idx].pos.y;
                float dx;
                float dy;
                float dx2;
                float dy2;
                float dist2;
                float int_x;
                float int_y;

                if (joint->flags &
                    (CollJoint_B10 | CollJoint_B9 | CollJoint_B8))
                {
                    mpRemap2d(&ax, &ay, groundCollVtx[line->x0->v0_idx].x10,
                              groundCollVtx[line->x0->v0_idx].x14,
                              groundCollVtx[line->x0->v1_idx].x10,
                              groundCollVtx[line->x0->v1_idx].x14, x0, y0, x1,
                              y1, old_x, old_y);
                } else {
                    ax = old_x;
                    ay = old_y;
                }

                dx = bx - ax;
                dy = by - ay;
                if (ABS(x0 - x1) > 0.0001) {
                    if (mpLineIntersection(x0, y0, x1, y1, ax, ay, bx, by,
                                           &int_x, &int_y))
                    {
                        dx2 = SQ(int_x - old_x);
                        dy2 = SQ(int_y - old_y);
                        dist2 = dx2 + dy2;
                        if ((dx * (int_x - old_x)) + (dy * (int_y - old_y)) <
                            0.0F)
                        {
                            dist2 = -dist2;
                        }

                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;

                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }

                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }

                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }

                            if (normal_out != NULL) {
                                normal_out->x = -(y1 - y0);
                                normal_out->y = x1 - x0;
                                normal_out->z = 0.0F;
                                PSVECNormalize(normal_out, normal_out);
                            }

                            result = true;
                        }
                    }
                } else {
                    if (ax >= bx && mpLineIntersectionV(&int_x, &int_y, x0, y0,
                                                        y1, ax, ay, bx, by))
                    {
                        dx2 = SQ(int_x - old_x);
                        dy2 = SQ(int_y - old_y);
                        dist2 = dx2 + dy2;
                        if ((dx * (int_x - old_x)) + (dy * (int_y - old_y)) <
                            0.0F)
                        {
                            dist2 = -dist2;
                        }

                        if (min_dist2 > dist2) {
                            min_dist2 = dist2;

                            if (vec_out != NULL) {
                                vec_out->x = int_x;
                                vec_out->y = int_y;
                                vec_out->z = 0.0F;
                            }

                            if (line_id_out != NULL) {
                                *line_id_out = line - groundCollLine;
                            }

                            if (flags_out != NULL) {
                                *flags_out = line->x0->lo_flags;
                            }

                            if (normal_out != NULL) {
                                normal_out->x = 1.0F;
                                normal_out->y = 0.0F;
                                normal_out->z = 0.0F;
                            }

                            result = true;
                        }
                    }
                }
            }
        }

        if (dynamic_count != 0) {
            count = dynamic_count;
            i = 0;
            dynamic_count = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }

    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpLib_800511A4_RightWall(float ax, float ay, float bx, float by, float cx,
                              float cy, float dx, float dy, int* line_id_out,
                              int joint_id_skip, int joint_id_only)
{
    float min_dist2;
    CollJoint* joint;
    int i;
    int result;
    bool already_checked;
    PAD_STACK(8);

    result = false;
    min_dist2 = F32_MAX;
    already_checked = mpCheckedBounding();

    if (!already_checked) {
        mpBoundingCheck3(ax, ay, bx, by, cx, cy, dx, dy);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        CollLine* line;
        int count;
        int dynamic_count;
        MapJoint* j_inner;

        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == (joint - groundCollJoint) ||
            !(joint_id_only == -1 ||
              joint_id_only == (joint - groundCollJoint)))
        {
            continue;
        }

        j_inner = joint->inner;

        count = j_inner->ranges[MapLineGroup_RightWall].count;
        dynamic_count = j_inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[j_inner->ranges[MapLineGroup_RightWall].start];
        for (i = 0; i < count; i++, line++) {
        block_8:
            if (line->flags & CollLine_RightWall &&
                line->flags & LINE_FLAG_ENABLED &&
                !(line->flags & LINE_FLAG_EMPTY))
            {
                CollVtx* vtx;
                float int_x;
                float int_y;
                float x;
                float y;
                float x0;
                float y0;
                float x1;
                float y1;
                float vdx;
                float vdy;
                float dist2;

                {
                    vtx = &groundCollVtx[line->x0->v0_idx];
                    x0 = vtx->pos.x;
                    y0 = vtx->pos.y;
                    x1 = vtx->x10;
                    y1 = vtx->x14;
                    mpRemap2d(&x, &y, ax, ay, bx, by, cx, cy, dx, dy, x1, y1);

                    vdx = x0 - x;
                    vdy = y0 - y;

                    if (SQ(vdx) + SQ(vdy) > 0.001F) {
                        if (mpLineIntersection(cx, cy, dx, dy, x, y, x0, y0,
                                               &int_x, &int_y))
                        {
                            dist2 = SQ(int_x - x1) + SQ(int_y - y1);
                            if ((vdx * (int_x - x1)) + (vdy * (int_y - y1)) <
                                0.0F)
                            {
                                dist2 = -dist2;
                            }
                            if (min_dist2 > dist2) {
                                min_dist2 = dist2;
                                if (line_id_out != NULL) {
                                    *line_id_out = line - groundCollLine;
                                }
                                result = true;
                            }
                        }
                    }
                }

                {
                    vtx = &groundCollVtx[line->x0->v1_idx];
                    x0 = vtx->pos.x;
                    y0 = vtx->pos.y;
                    x1 = vtx->x10;
                    y1 = vtx->x14;
                    mpRemap2d(&x, &y, ax, ay, bx, by, cx, cy, dx, dy, x1, y1);

                    vdx = x0 - x;
                    vdy = y0 - y;

                    if (SQ(vdx) + SQ(vdy) > 0.001F) {
                        if (mpLineIntersection(cx, cy, dx, dy, x, y, x0, y0,
                                               &int_x, &int_y))
                        {
                            dist2 = SQ(int_x - x1) + SQ(int_y - y1);
                            if ((vdx * (int_x - x1)) + (vdy * (int_y - y1)) <
                                0.0F)
                            {
                                dist2 = -dist2;
                            }
                            if (min_dist2 > dist2) {
                                min_dist2 = dist2;
                                if (line_id_out != NULL) {
                                    *line_id_out = line - groundCollLine;
                                }
                                result = true;
                            }
                        }
                    }
                }
            }
        }

        if (dynamic_count != 0) {
            count = dynamic_count;
            i = 0;
            dynamic_count = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }

    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpLib_800515A0_LeftWall(float a0x, float a0y, float a1x, float a1y,
                             float b0x, float b0y, float b1x, float b1y,
                             int* line_id_out, int joint_id_skip,
                             int joint_id_only)
{
    float min_dist2;
    CollJoint* joint;
    int i;
    int result;
    bool already_checked;
    PAD_STACK(8);

    result = false;
    min_dist2 = F32_MAX;
    already_checked = mpCheckedBounding();

    if (!already_checked) {
        mpBoundingCheck3(a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y);
    }

    for (joint = jointListStart; joint != NULL; joint = joint->next) {
        CollLine* line;
        int count;
        int dynamic_count;
        MapJoint* j_inner;

        if (joint->flags & CollJoint_TooFar) {
            continue;
        }

        if (joint_id_skip == (joint - groundCollJoint) ||
            !(joint_id_only == -1 ||
              joint_id_only == (joint - groundCollJoint)))
        {
            continue;
        }

        j_inner = joint->inner;

        count = j_inner->ranges[MapLineGroup_LeftWall].count;
        dynamic_count = j_inner->ranges[MapLineGroup_Dynamic].count;
        line = &groundCollLine[j_inner->ranges[MapLineGroup_LeftWall].start];
        for (i = 0; i < count; i++, line++) {
        block_8:
            if (line->flags & CollLine_LeftWall &&
                line->flags & LINE_FLAG_ENABLED &&
                !(line->flags & LINE_FLAG_EMPTY))
            {
                CollVtx* vtx;
                float int_x;
                float int_y;
                float x;
                float y;
                float x0;
                float y0;
                float x1;
                float y1;
                float vdx;
                float vdy;
                float dist2;

                {
                    vtx = &groundCollVtx[line->x0->v0_idx];
                    x0 = vtx->pos.x;
                    y0 = vtx->pos.y;
                    x1 = vtx->x10;
                    y1 = vtx->x14;
                    mpRemap2d(&x, &y, a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y,
                              x1, y1);

                    vdx = x0 - x;
                    vdy = y0 - y;

                    if (SQ(vdx) + SQ(vdy) > 0.001F) {
                        if (mpLineIntersection(b0x, b0y, b1x, b1y, x, y, x0,
                                               y0, &int_x, &int_y))
                        {
                            dist2 = SQ(int_x - x1) + SQ(int_y - y1);
                            if ((vdx * (int_x - x1)) + (vdy * (int_y - y1)) <
                                0.0F)
                            {
                                dist2 = -dist2;
                            }
                            if (min_dist2 > dist2) {
                                min_dist2 = dist2;
                                if (line_id_out != NULL) {
                                    *line_id_out = line - groundCollLine;
                                }
                                result = true;
                            }
                        }
                    }
                }

                {
                    vtx = &groundCollVtx[line->x0->v1_idx];
                    x0 = vtx->pos.x;
                    y0 = vtx->pos.y;
                    x1 = vtx->x10;
                    y1 = vtx->x14;
                    mpRemap2d(&x, &y, a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y,
                              x1, y1);

                    vdx = x0 - x;
                    vdy = y0 - y;

                    if (SQ(vdx) + SQ(vdy) > 0.001F) {
                        if (mpLineIntersection(b0x, b0y, b1x, b1y, x, y, x0,
                                               y0, &int_x, &int_y))
                        {
                            dist2 = SQ(int_x - x1) + SQ(int_y - y1);
                            if ((vdx * (int_x - x1)) + (vdy * (int_y - y1)) <
                                0.0F)
                            {
                                dist2 = -dist2;
                            }
                            if (min_dist2 > dist2) {
                                min_dist2 = dist2;
                                if (line_id_out != NULL) {
                                    *line_id_out = line - groundCollLine;
                                }
                                result = true;
                            }
                        }
                    }
                }
            }
        }

        if (dynamic_count != 0) {
            count = dynamic_count;
            i = 0;
            dynamic_count = 0;
            line = &groundCollLine[joint->inner->ranges[MapLineGroup_Dynamic]
                                       .start];
            goto block_8;
        }
    }

    if (!already_checked) {
        mpUncheckBounding();
    }

    return result;
}

bool mpCheckedBounding(void)
{
    return didCheckBounding;
}

void mpBoundingCheck(float left, float bottom, float right, float top)
{
    CollJoint* curr = jointListStart;

    while (curr != NULL) {
        if (curr->flags & CollJoint_Enabled &&
            !(curr->flags & CollJoint_Hidden))
        {
            if (curr->flags & CollJoint_B10) {
                curr->flags &= ~CollJoint_TooFar;
            } else if (left > curr->bounding_max.x ||
                       right < curr->bounding_min.x ||
                       bottom > curr->bounding_max.y ||
                       top < curr->bounding_min.y)
            {
                curr->flags |= CollJoint_TooFar;
            } else {
                curr->flags &= ~CollJoint_TooFar;
            }
        } else {
            curr->flags |= CollJoint_TooFar;
        }

        curr = curr->next;
    }

    didCheckBounding = true;
}

void mpBoundingCheck2(float x1, float y1, float x2, float y2)
{
    float right;
    float left;
    float bottom;
    float top;

    if (x1 > x2) {
        left = x2;
        right = x1;
    } else {
        right = x2;
        left = x1;
    }
    if (y1 > y2) {
        bottom = y2;
        top = y1;
    } else {
        top = y2;
        bottom = y1;
    }
    mpBoundingCheck(left, bottom, right, top);
}

void mpBoundingCheck3(float x0, float y0, float x1, float y1, float x2,
                      float y2, float x3, float y3)
{
    float right;
    float left;
    float bottom;
    float top;

    left = x1;
    bottom = y1;
    if (x0 > left) {
        right = x0;
    } else {
        right = left;
        left = x0;
    }
    if (y0 > bottom) {
        top = y0;
    } else {
        top = bottom;
        bottom = y0;
    }
    if (right < x2) {
        right = x2;
    } else if (left > x2) {
        left = x2;
    }
    if (top < y2) {
        top = y2;
    } else if (bottom > y2) {
        bottom = y2;
    }
    if (right < x3) {
        right = x3;
    } else if (left > x3) {
        left = x3;
    }
    if (top < y3) {
        top = y3;
    } else if (bottom > y3) {
        bottom = y3;
    }
    mpBoundingCheck(left, bottom, right, top);
}

void mpUncheckBounding(void)
{
    CollJoint* curr = jointListStart;

    while (curr != NULL) {
        curr->flags &= ~CollJoint_TooFar;
        curr = curr->next;
    }

    didCheckBounding = false;
}

#undef mpLineGetNext
#undef mpLineGetPrev
#undef mpRemap2d
#undef mpLineIntersection
#undef mpLineIntersectionH
#undef mpLineGetCollLine
#undef mpLib_8004ED5C
#undef mpCheckFloor
#undef mpCheckFloorRemap
#undef mpLineIntersectionV
#undef mpCheckLeftWall
#undef mpCheckLeftWallRemap
#undef mpCheckRightWall
#undef mpCheckRightWallRemap
#undef mpLib_800511A4_RightWall
#undef mpLib_800515A0_LeftWall
#undef mpCheckedBounding
#undef mpBoundingCheck
#undef mpBoundingCheck2
#undef mpBoundingCheck3
#undef mpUncheckBounding
