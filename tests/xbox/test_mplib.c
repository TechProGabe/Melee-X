/* test_mplib.c - host check that the per-line rejects in the stage-collision
 * line tests (src/melee/mp/mplib.c, TARGET_XBOX) change nothing. The current
 * functions, cut out of mplib.c by tools/xbox/test_mplib.py, run side by side
 * with mplib_ref.c, the code before, on the same stage and the same query,
 * and must agree on everything they leave behind: the return value, every
 * output (bit for bit, written or not), the joints' flags, didCheckBounding
 * and the sequence of mpCheckFloor's callback calls. Stages:
 *   - Fountain of Dreams-like: the main stage outline (floor, walls, the
 *     underside), three platforms on joints of their own, moving ones
 *     remapped (B8/B9/B10) with last-frame positions, queries shaped like the
 *     fighters' (ECB sweeps, the floor probes' vertical segments, ECB edges
 *     for the vertex sweeps);
 *   - random polylines and lines with random flags, links and group ranges;
 *   - chains of short floor lines (0.002 to 3 long), probed across their
 *     ends, where mpLib_8004ED5C lengthens lines most;
 *   - adversarial: lines shorter than 1 and of length 0, near-vertical and
 *     near-horizontal ones, coordinates at the +-2^16 guard, huge ones,
 *     infinities and NaNs, and queries sitting on the reject margins (a
 *     vertex +-margin, +-1 ulp).
 * It also prints how many of the full line tests the rejects skip on the
 * Fountain-like stage. Built and run by tools/xbox/test_mplib.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Runtime/platform.h>
#include <placeholder.h>

#include <dolphin/mtx.h>
#include <melee/ft/forward.h>
#include <melee/mp/types.h>

/* mplib.c's globals, shared by both copies */
static CollVtx* groundCollVtx;
static CollLine* groundCollLine;
static CollJoint* groundCollJoint;
static CollJoint* jointListStart;
static bool didCheckBounding;

#include "mplib_ref.c"

#include "mplib_cur.c"

/* ---- random numbers */

static u64 rng_state = 0x9E3779B97F4A7C15ull;
static u32 rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (u32) ((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static u32 rnd_below(u32 n)
{
    return (u32) (((u64) rnd() * n) >> 32);
}
static bool chance(u32 percent)
{
    return rnd_below(100) < percent;
}
/* uniform in [lo, hi) */
static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float) (rnd() >> 8) * (1.0F / 16777216.0F);
}
static float f_of(u32 u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static u32 u_of(float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}
/* n ulps away (n may be negative), through zero */
static float ulps(float f, int n)
{
    while (n > 0) {
        f = nextafterf(f, INFINITY);
        n--;
    }
    while (n < 0) {
        f = nextafterf(f, -INFINITY);
        n++;
    }
    return f;
}

static const u32 special_bits[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x00800000, 0x3F800000,
    0xBF800000, 0x38D1B717 /* 1e-4 */, 0x47800000 /* 65536 */,
    0xC7800000, 0x477FFF00, 0x47800080, 0x48000000, 0xC8000000,
    0x49800000 /* 2^20 */, 0xC9800000, 0x5F800000 /* 2^64 */, 0xDF800000,
    0x7149F2CA /* 1e30 */, 0xF149F2CA, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000,
    0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001,
};
#define N_SPECIAL (sizeof(special_bits) / sizeof(special_bits[0]))
static float special(void)
{
    return f_of(special_bits[rnd_below(N_SPECIAL)]);
}

/* ---- the stage */

#define MAX_VTX 512
#define MAX_LINES 384
#define MAX_JOINTS 12

static CollVtx vtx[MAX_VTX];
static MapLine maplines[MAX_LINES];
static CollLine lines[MAX_LINES];
static MapJoint mapjoints[MAX_JOINTS];
static CollJoint joints[MAX_JOINTS];
static int n_vtx, n_lines, n_joints;

static int add_vtx(float x, float y)
{
    CollVtx* v = &vtx[n_vtx];
    v->x0 = x;
    v->x4 = y;
    v->pos.x = x;
    v->pos.y = y;
    v->x10 = x;
    v->x14 = y;
    return n_vtx++;
}

static u32 kind_flag(int group)
{
    switch (group) {
    case MapLineGroup_Floor:
        return CollLine_Floor;
    case MapLineGroup_Ceiling:
        return CollLine_Ceiling;
    case MapLineGroup_RightWall:
        return CollLine_RightWall;
    case MapLineGroup_LeftWall:
        return CollLine_LeftWall;
    default:
        return 1u << rnd_below(4);
    }
}

static int add_line(int v0, int v1, int group)
{
    MapLine* m = &maplines[n_lines];
    CollLine* l = &lines[n_lines];
    memset(m, 0, sizeof(*m));
    m->v0_idx = (u16) v0;
    m->v1_idx = (u16) v1;
    m->prev_id0 = m->next_id0 = m->prev_id1 = m->next_id1 = -1;
    m->lo_flags = (u16) rnd();
    m->hi_flags = (u16) rnd();
    l->x0 = m;
    l->flags = kind_flag(group) | LINE_FLAG_ENABLED;
    return n_lines++;
}

/* a joint over lines [first, n_lines), which must be in group order */
static void add_joint(int first, const int* group_of, int vtx_first)
{
    MapJoint* mj = &mapjoints[n_joints];
    CollJoint* j = &joints[n_joints];
    int g, i;
    memset(mj, 0, sizeof(*mj));
    memset(j, 0, sizeof(*j));
    for (g = 0; g < MapLineGroup_Count; g++) {
        int start = -1, count = 0;
        for (i = first; i < n_lines; i++) {
            if (group_of[i] == g) {
                if (start < 0) {
                    start = i;
                }
                count++;
            }
        }
        mj->ranges[g].start = (s16) (start < 0 ? first : start);
        mj->ranges[g].count = (s16) count;
    }
    mj->vtx_start = (s16) vtx_first;
    mj->vtx_count = (s16) (n_vtx - vtx_first);
    j->inner = mj;
    j->flags = CollJoint_Enabled;
    j->bounding_min.x = j->bounding_min.y = F32_MAX;
    j->bounding_max.x = j->bounding_max.y = -F32_MAX;
    for (i = vtx_first; i < n_vtx; i++) {
        float x = vtx[i].pos.x, y = vtx[i].pos.y;
        if (j->bounding_min.x > x - 30.0F) {
            j->bounding_min.x = x - 30.0F;
        }
        if (j->bounding_max.x < x + 30.0F) {
            j->bounding_max.x = x + 30.0F;
        }
        if (j->bounding_min.y > y - 30.0F) {
            j->bounding_min.y = y - 30.0F;
        }
        if (j->bounding_max.y < y + 30.0F) {
            j->bounding_max.y = y + 30.0F;
        }
    }
    n_joints++;
}

static int group_of[MAX_LINES];

/* a closed or open polyline through pts, its lines in group order; the
 * group of each edge from its direction, as the stage files have them
 * (floors left to right on top, walls, ceilings right to left) */
static void add_polyline(const float* pts, int n, bool closed, float dx,
                         float dy)
{
    int vfirst = n_vtx, first = n_lines, i, k, g;
    int edge_group[64];
    int line_of_edge[64];
    int n_edges = closed ? n : n - 1;
    for (i = 0; i < n; i++) {
        add_vtx(pts[2 * i] + dx, pts[2 * i + 1] + dy);
    }
    for (k = 0; k < n_edges; k++) {
        float x0 = pts[2 * k], y0 = pts[2 * k + 1];
        float x1 = pts[2 * ((k + 1) % n)], y1 = pts[2 * ((k + 1) % n) + 1];
        float ex = x1 - x0, ey = y1 - y0;
        if (ABS(ex) >= ABS(ey)) {
            g = ex > 0 ? MapLineGroup_Floor : MapLineGroup_Ceiling;
        } else {
            g = ey > 0 ? MapLineGroup_RightWall : MapLineGroup_LeftWall;
        }
        edge_group[k] = g;
    }
    /* lines in group order; walls go bottom to top for the left wall
     * check's v0 < v1 convention, as in the game's data */
    for (g = 0; g < MapLineGroup_Count; g++) {
        for (k = 0; k < n_edges; k++) {
            if (edge_group[k] == g) {
                int v0 = vfirst + k, v1 = vfirst + (k + 1) % n;
                line_of_edge[k] = add_line(v0, v1, g);
                group_of[line_of_edge[k]] = g;
            }
        }
    }
    for (k = 0; k < n_edges; k++) {
        MapLine* m = &maplines[line_of_edge[k]];
        int prev = k > 0 ? k - 1 : (closed ? n_edges - 1 : -1);
        int next = k + 1 < n_edges ? k + 1 : (closed ? 0 : -1);
        m->prev_id0 = (s16) (prev < 0 ? -1 : line_of_edge[prev]);
        m->next_id0 = (s16) (next < 0 ? -1 : line_of_edge[next]);
        m->prev_id1 = chance(30) ? m->prev_id0 : -1;
        m->next_id1 = chance(30) ? m->next_id0 : -1;
    }
    add_joint(first, group_of, vfirst);
}

static void link_joints(void)
{
    int order[MAX_JOINTS], i;
    for (i = 0; i < n_joints; i++) {
        order[i] = i;
    }
    for (i = n_joints - 1; i > 0; i--) {
        int k = rnd_below(i + 1), t = order[i];
        order[i] = order[k];
        order[k] = t;
    }
    jointListStart = NULL;
    for (i = n_joints - 1; i >= 0; i--) {
        joints[order[i]].next = jointListStart;
        jointListStart = &joints[order[i]];
    }
}

static void reset_stage(void)
{
    n_vtx = n_lines = n_joints = 0;
    memset(vtx, 0, sizeof(vtx));
    memset(lines, 0, sizeof(lines));
    memset(maplines, 0, sizeof(maplines));
    groundCollVtx = vtx;
    groundCollLine = lines;
    groundCollJoint = joints;
}

/* moving joint j: last frame's positions differ by a small move (and maybe
 * a turn), and the joint is flagged for the remap */
static void move_joint(int j, float mx, float my, float spin)
{
    MapJoint* mj = &mapjoints[j];
    int i;
    for (i = mj->vtx_start; i < mj->vtx_start + mj->vtx_count; i++) {
        float x = vtx[i].pos.x, y = vtx[i].pos.y;
        vtx[i].x10 = x - mx - spin * y;
        vtx[i].x14 = y - my + spin * x;
    }
    joints[j].flags |= CollJoint_B8;
    if (spin != 0.0F) {
        joints[j].flags |= CollJoint_B9;
    }
    if (chance(20)) {
        joints[j].flags |= CollJoint_B10;
    }
}

/* Fountain of Dreams: a main stage 127 wide with an outline of ~30 lines,
 * and three platforms, the side ones moving up and down */
static const float fod_main[] = {
    -63.35F, 0.62F,   -21.0F, 0.62F,   21.0F, 0.62F,    63.35F, 0.62F,
    63.35F,  -3.5F,   61.0F,  -6.0F,   55.0F, -9.0F,    50.0F,  -14.0F,
    46.0F,   -20.0F,  42.0F,  -28.0F,  38.0F, -36.0F,   33.0F,  -44.0F,
    26.0F,   -52.0F,  18.0F,  -58.0F,  9.0F,  -62.0F,   0.0F,   -63.5F,
    -9.0F,   -62.0F,  -18.0F, -58.0F,  -26.0F, -52.0F,  -33.0F, -44.0F,
    -38.0F,  -36.0F,  -42.0F, -28.0F,  -46.0F, -20.0F,  -50.0F, -14.0F,
    -55.0F,  -9.0F,   -61.0F, -6.0F,   -63.35F, -3.5F,
};
static const float fod_platform[] = {
    -14.25F, 0.0F, 14.25F, 0.0F, 14.25F, -2.0F, -14.25F, -2.0F,
};

static void build_fountain(void)
{
    reset_stage();
    add_polyline(fod_main, sizeof(fod_main) / sizeof(float) / 2, true,
                 frand(-0.01F, 0.01F), 0.0F);
    add_polyline(fod_platform, 4, true, -35.0F, frand(4.0F, 28.0F));
    add_polyline(fod_platform, 4, true, 35.0F, frand(4.0F, 28.0F));
    add_polyline(fod_platform, 4, true, 0.0F, 42.75F);
    if (chance(70)) {
        move_joint(1, 0.0F, frand(-1.0F, 1.0F), 0.0F);
        move_joint(2, 0.0F, frand(-1.0F, 1.0F), 0.0F);
    }
    if (chance(30)) {
        move_joint(0, frand(-0.5F, 0.5F), frand(-0.5F, 0.5F),
                   chance(30) ? frand(-0.01F, 0.01F) : 0.0F);
    }
    link_joints();
}

static float coord(float scale)
{
    if (chance(3)) {
        return special();
    }
    return frand(-scale, scale);
}

/* random polylines and stray lines, with random flags and links */
static void build_random(bool adversarial)
{
    int n_poly = 1 + rnd_below(5), p, i, k;
    reset_stage();
    for (p = 0; p < n_poly && n_joints < MAX_JOINTS - 1; p++) {
        float pts[2 * 24];
        int n = 2 + rnd_below(20);
        float scale = chance(50) ? 100.0F : 300.0F;
        float cx = frand(-150.0F, 150.0F), cy = frand(-100.0F, 100.0F);
        for (i = 0; i < n; i++) {
            float x, y;
            if (adversarial && chance(25)) {
                x = coord(scale);
                y = coord(scale);
                if (chance(30) && i > 0) {
                    /* short, null, near-vertical, near-horizontal */
                    switch (rnd_below(5)) {
                    case 0:
                        x = pts[2 * i - 2];
                        y = pts[2 * i - 1];
                        break;
                    case 1:
                        x = pts[2 * i - 2] + frand(-0.7F, 0.7F);
                        y = pts[2 * i - 1] + frand(-0.7F, 0.7F);
                        break;
                    case 2:
                        x = pts[2 * i - 2] + frand(-0.0002F, 0.0002F);
                        break;
                    case 3:
                        y = pts[2 * i - 1] + frand(-0.0002F, 0.0002F);
                        break;
                    default:
                        x = ulps(pts[2 * i - 2], (int) rnd_below(5) - 2);
                        break;
                    }
                }
            } else {
                x = cx + frand(-scale, scale) * 0.3F;
                y = cy + frand(-scale, scale) * 0.3F;
            }
            pts[2 * i] = x;
            pts[2 * i + 1] = y;
        }
        add_polyline(pts, n, n > 2 && chance(50), 0.0F, 0.0F);
        if (chance(40)) {
            move_joint(n_joints - 1, frand(-3.0F, 3.0F), frand(-3.0F, 3.0F),
                       chance(30) ? frand(-0.05F, 0.05F) : 0.0F);
        }
        if (adversarial && chance(30)) {
            MapJoint* mj = &mapjoints[n_joints - 1];
            for (i = mj->vtx_start; i < mj->vtx_start + mj->vtx_count; i++) {
                if (chance(20)) {
                    vtx[i].x10 = coord(400.0F);
                }
                if (chance(20)) {
                    vtx[i].x14 = coord(400.0F);
                }
            }
        }
    }
    /* scramble: flags, links, ranges overlapping other joints' lines */
    for (i = 0; i < n_lines; i++) {
        if (chance(10)) {
            lines[i].flags ^= 1u << rnd_below(4);
        }
        if (chance(5)) {
            lines[i].flags &= ~LINE_FLAG_ENABLED;
        }
        if (chance(5)) {
            lines[i].flags |= LINE_FLAG_EMPTY;
        }
        if (chance(5)) {
            lines[i].flags |= LINE_FLAG_HIDDEN;
        }
        if (chance(5)) {
            maplines[i].prev_id1 = (s16) rnd_below(n_lines);
        }
        if (chance(5)) {
            maplines[i].next_id1 = (s16) rnd_below(n_lines);
        }
    }
    for (k = 0; k < n_joints; k++) {
        if (chance(10)) {
            int g = rnd_below(MapLineGroup_Count);
            int start = rnd_below(n_lines);
            mapjoints[k].ranges[g].start = (s16) start;
            mapjoints[k].ranges[g].count = (s16) rnd_below(n_lines - start + 1);
        }
        if (chance(5)) {
            joints[k].flags |= CollJoint_Hidden;
        }
        if (chance(5)) {
            joints[k].flags &= ~CollJoint_Enabled;
        }
        if (chance(10)) {
            joints[k].flags |= CollJoint_B10;
        }
    }
    link_joints();
}

/* ---- queries */

/* a point on or near a random line of the stage, or a special value */
static void near_point(float* x, float* y, float spread)
{
    if (n_lines > 0 && !chance(5)) {
        CollLine* l = &lines[rnd_below(n_lines)];
        CollVtx* v0 = &vtx[l->x0->v0_idx];
        CollVtx* v1 = &vtx[l->x0->v1_idx];
        float t = frand(-0.3F, 1.3F);
        *x = v0->pos.x + t * (v1->pos.x - v0->pos.x) + frand(-spread, spread);
        *y = v0->pos.y + t * (v1->pos.y - v0->pos.y) + frand(-spread, spread);
        if (chance(15)) {
            /* on a reject margin: a vertex coordinate +- {0, 1, 2, 4,
             * 3.00001, 5, 0.0001} +- a few ulps */
            static const float m[] = { 0.0F, 1.0F, 4.0F, 3.00001F, 0.0001F,
                                       2.0F, 5.0F };
            CollVtx* v = chance(50) ? v0 : v1;
            float d = m[rnd_below(7)] * (chance(50) ? 1.0F : -1.0F);
            if (chance(50)) {
                *x = ulps(v->pos.x + d, (int) rnd_below(5) - 2);
            } else {
                *y = ulps(v->pos.y + d, (int) rnd_below(5) - 2);
            }
        }
    } else {
        *x = frand(-250.0F, 250.0F);
        *y = frand(-150.0F, 150.0F);
    }
    if (chance(2)) {
        *x = special();
    }
    if (chance(2)) {
        *y = special();
    }
}

typedef struct Query {
    float ax, ay, bx, by, cx, cy, dx, dy, y_offset;
    int line_id_skip, joint_id_skip, joint_id_only;
    bool use_cb, null_out[4], checked;
    u32 cb_seed;
} Query;

static void make_query(Query* q)
{
    float spread = chance(50) ? 3.0F : 30.0F;
    near_point(&q->ax, &q->ay, spread);
    switch (rnd_below(4)) {
    case 0: /* the floor probes: a vertical segment through the point */
        q->bx = q->ax;
        q->ay += 5.0F;
        q->by = q->ay - frand(5.0F, 25.0F);
        break;
    case 1: /* an ECB sweep */
        q->bx = q->ax + frand(-6.0F, 6.0F);
        q->by = q->ay + frand(-6.0F, 6.0F);
        break;
    case 2:
        near_point(&q->bx, &q->by, spread);
        break;
    default:
        q->bx = q->ax;
        q->by = q->ay;
        if (chance(50)) {
            q->bx = ulps(q->bx, (int) rnd_below(5) - 2);
        }
        break;
    }
    /* the vertex sweeps: an ECB edge a-b moving to c-d */
    q->cx = q->ax + frand(-4.0F, 4.0F);
    q->cy = q->ay + frand(-4.0F, 4.0F);
    q->dx = q->bx + frand(-4.0F, 4.0F);
    q->dy = q->by + frand(-4.0F, 4.0F);
    if (chance(5)) {
        q->cx = q->ax;
        q->cy = q->ay;
        q->dx = q->bx;
        q->dy = q->by;
    }
    if (chance(2)) {
        q->dx = special();
    }
    q->y_offset = chance(70) ? 0.0F : chance(80) ? frand(-10.0F, 10.0F)
                                                 : special();
    q->line_id_skip = chance(20) && n_lines ? (int) rnd_below(n_lines) : -1;
    q->joint_id_skip = chance(10) && n_joints ? (int) rnd_below(n_joints) : -1;
    q->joint_id_only = chance(10) && n_joints ? (int) rnd_below(n_joints) : -1;
    q->use_cb = chance(30);
    q->cb_seed = rnd();
    q->null_out[0] = chance(10);
    q->null_out[1] = chance(10);
    q->null_out[2] = chance(10);
    q->null_out[3] = chance(10);
    q->checked = chance(15);
}

/* chains of short floor lines (0.002 to 3 long, some flat), where
 * mpLib_8004ED5C's lengthening is largest: up to 1 + 1/length at the end
 * after a lengthened start */
static void build_short(void)
{
    float pts[2 * 24];
    int n = 3 + rnd_below(18), i;
    float x = frand(-100.0F, 100.0F), y = frand(-50.0F, 50.0F);
    reset_stage();
    for (i = 0; i < n; i++) {
        pts[2 * i] = x;
        pts[2 * i + 1] = y;
        x += chance(30) ? frand(0.002F, 0.05F) : frand(0.5F, 3.0F);
        y += chance(30) ? 0.0F : frand(-0.5F, 0.5F);
    }
    add_polyline(pts, n, false, 0.0F, 0.0F);
    if (chance(30)) {
        move_joint(0, frand(-1.0F, 1.0F), frand(-1.0F, 1.0F), 0.0F);
    }
    link_joints();
}

/* a vertical or slanted probe across a short line's end, or far along a
 * tiny line's direction */
static void short_query(Query* q)
{
    CollLine* l = &lines[rnd_below(n_lines)];
    CollVtx* v0 = &vtx[l->x0->v0_idx];
    CollVtx* v1 = &vtx[l->x0->v1_idx];
    CollVtx* v = chance(50) ? v0 : v1;
    float x, y;
    memset(q, 0, sizeof(*q));
    q->line_id_skip = q->joint_id_skip = q->joint_id_only = -1;
    if (chance(80)) {
        x = v->pos.x + frand(-10.0F, 10.0F);
    } else {
        float d = frand(-600.0F, 600.0F);
        x = v->pos.x + d * (v1->pos.x - v0->pos.x) * 100.0F;
    }
    y = v->pos.y + frand(-1.0F, 1.0F);
    q->ax = x;
    q->ay = y + frand(0.0F, 10.0F);
    q->bx = chance(70) ? x : x + frand(-2.0F, 2.0F);
    q->by = y - frand(0.0F, 10.0F);
    if (chance(20)) {
        q->ay = q->by = y;
        q->bx = x + frand(-10.0F, 10.0F);
    }
    q->cx = q->ax;
    q->cy = q->ay;
    q->dx = q->bx;
    q->dy = q->by;
    q->y_offset = chance(80) ? 0.0F : frand(-2.0F, 2.0F);
    q->checked = false;
}

/* ---- running both */

typedef struct Result {
    int ret;
    Vec3 vec;
    int line_id;
    u32 flags;
    Vec3 normal;
    u32 joint_flags[MAX_JOINTS];
    bool did_check;
    int n_cb;
    u32 cb_hash;
} Result;

static Result* cb_result;
static u32 cb_seed;
static bool floor_cb(Fighter_GObj* gobj, int line_id)
{
    u32 h = (u32) line_id * 0x9E3779B1u ^ cb_seed;
    cb_result->n_cb++;
    cb_result->cb_hash = (cb_result->cb_hash ^ (u32) line_id) * 0x01000193u;
    (void) gobj;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (h & 7) != 0;
}

enum {
    F_FLOOR,
    F_FLOOR_REMAP,
    F_LEFT,
    F_LEFT_REMAP,
    F_RIGHT,
    F_RIGHT_REMAP,
    F_800511A4,
    F_800515A0,
    F_COUNT
};
static const char* const f_name[F_COUNT] = {
    "mpCheckFloor",          "mpCheckFloorRemap",
    "mpCheckLeftWall",       "mpCheckLeftWallRemap",
    "mpCheckRightWall",      "mpCheckRightWallRemap",
    "mpLib_800511A4_RightWall", "mpLib_800515A0_LeftWall",
};

static u32 saved_flags[MAX_JOINTS];

static void run(int f, bool ref, const Query* q, Result* r)
{
    Vec3* vec = q->null_out[0] ? NULL : &r->vec;
    int* lid = q->null_out[1] ? NULL : &r->line_id;
    u32* fl = q->null_out[2] ? NULL : &r->flags;
    Vec3* nrm = q->null_out[3] ? NULL : &r->normal;
    bool (*cb)(Fighter_GObj*, int) = q->use_cb ? floor_cb : NULL;
    Fighter_GObj* gobj = (Fighter_GObj*) (uintptr_t) 0x1234;
    int i;

    memset(r, 0xA5, sizeof(*r));
    r->n_cb = 0;
    r->cb_hash = 0;
    cb_result = r;
    cb_seed = q->cb_seed;
    for (i = 0; i < n_joints; i++) {
        joints[i].flags = saved_flags[i];
    }
    didCheckBounding = q->checked;

    switch (f) {
    case F_FLOOR:
        r->ret = (ref ? ref_mpCheckFloor : mpCheckFloor)(
            q->ax, q->ay, q->bx, q->by, q->y_offset, vec, lid, fl, nrm,
            q->line_id_skip, q->joint_id_skip, q->joint_id_only, cb, gobj);
        break;
    case F_FLOOR_REMAP:
        r->ret = (ref ? ref_mpCheckFloorRemap : mpCheckFloorRemap)(
            q->ax, q->ay, q->bx, q->by, q->y_offset, vec, lid, fl, nrm,
            q->line_id_skip, q->joint_id_skip, q->joint_id_only, cb, gobj);
        break;
    case F_LEFT:
        r->ret = (ref ? ref_mpCheckLeftWall : mpCheckLeftWall)(
            q->ax, q->ay, q->bx, q->by, vec, lid, fl, nrm, q->joint_id_skip,
            q->joint_id_only);
        break;
    case F_LEFT_REMAP:
        r->ret = (ref ? ref_mpCheckLeftWallRemap : mpCheckLeftWallRemap)(
            q->ax, q->ay, q->bx, q->by, vec, lid, fl, nrm, q->joint_id_skip,
            q->joint_id_only);
        break;
    case F_RIGHT:
        r->ret = (ref ? ref_mpCheckRightWall : mpCheckRightWall)(
            q->ax, q->ay, q->bx, q->by, vec, lid, fl, nrm, q->joint_id_skip,
            q->joint_id_only);
        break;
    case F_RIGHT_REMAP:
        r->ret = (ref ? ref_mpCheckRightWallRemap : mpCheckRightWallRemap)(
            q->ax, q->ay, q->bx, q->by, vec, lid, fl, nrm, q->joint_id_skip,
            q->joint_id_only);
        break;
    case F_800511A4:
        r->ret = (ref ? ref_mpLib_800511A4_RightWall
                      : mpLib_800511A4_RightWall)(
            q->ax, q->ay, q->bx, q->by, q->cx, q->cy, q->dx, q->dy, lid,
            q->joint_id_skip, q->joint_id_only);
        break;
    default:
        r->ret = (ref ? ref_mpLib_800515A0_LeftWall
                      : mpLib_800515A0_LeftWall)(
            q->ax, q->ay, q->bx, q->by, q->cx, q->cy, q->dx, q->dy, lid,
            q->joint_id_skip, q->joint_id_only);
        break;
    }
    for (i = 0; i < n_joints; i++) {
        r->joint_flags[i] = joints[i].flags;
    }
    r->did_check = didCheckBounding;
}

static long n_runs[F_COUNT], n_hits[F_COUNT];
static int n_fail;

static void print_query(const Query* q)
{
    fprintf(stderr,
            "  a (%a, %a) b (%a, %a) c (%a, %a) d (%a, %a) y_offset %a\n"
            "  skip line %d joint %d only %d cb %d null %d%d%d%d checked %d\n",
            q->ax, q->ay, q->bx, q->by, q->cx, q->cy, q->dx, q->dy,
            q->y_offset, q->line_id_skip, q->joint_id_skip, q->joint_id_only,
            q->use_cb, q->null_out[0], q->null_out[1], q->null_out[2],
            q->null_out[3], q->checked);
}

static void compare(const Query* q)
{
    int f, i;
    for (i = 0; i < n_joints; i++) {
        saved_flags[i] = joints[i].flags;
        if (q->checked) {
            /* mpCheckedBounding: the caller's TooFar flags stand */
            if (chance(20)) {
                saved_flags[i] |= CollJoint_TooFar;
            } else {
                saved_flags[i] &= ~CollJoint_TooFar;
            }
        }
    }
    for (f = 0; f < F_COUNT; f++) {
        Result a, b;
        run(f, true, q, &a);
        run(f, false, q, &b);
        n_runs[f]++;
        n_hits[f] += a.ret != 0;
        if (memcmp(&a, &b, sizeof(a)) != 0) {
            if (n_fail++ < 10) {
                fprintf(stderr,
                        "%s differs: ret %d/%d line %d/%d vec %a,%a/%a,%a "
                        "cb %d/%d\n",
                        f_name[f], a.ret, b.ret, a.line_id, b.line_id, a.vec.x,
                        a.vec.y, b.vec.x, b.vec.y, a.n_cb, b.n_cb);
                print_query(q);
            }
        }
    }
    for (i = 0; i < n_joints; i++) {
        joints[i].flags = saved_flags[i] & ~CollJoint_TooFar;
    }
    didCheckBounding = false;
}

/* ---- how much the rejects skip on the Fountain-like stage */

static long st_lines[F_COUNT], st_skipped[F_COUNT];

static void tally(const Query* q)
{
    CollJoint* j;
    mpLineBox box, fbox, edge, reach;
    int f;
    mpLineBoxInit(&box, q->ax, q->ay, q->bx, q->by, 0.0F, 1.0F);
    mpLineBoxInit(&fbox, q->ax, q->ay, q->bx, q->by, q->y_offset, 4.0F);
    mpEdgeBoxInit(&edge, &reach, q->ax, q->ay, q->bx, q->by, q->cx, q->cy,
                  q->dx, q->dy);
    mpBoundingCheck2(q->ax, q->ay, q->bx, q->by);
    for (f = 0; f < F_COUNT; f++) {
        int group = f <= F_FLOOR_REMAP ? MapLineGroup_Floor
                    : f == F_LEFT || f == F_LEFT_REMAP || f == F_800515A0
                        ? MapLineGroup_LeftWall
                        : MapLineGroup_RightWall;
        u32 kind = kind_flag(group);
        if (f == F_800511A4) {
            mpBoundingCheck3(q->ax, q->ay, q->bx, q->by, q->cx, q->cy, q->dx,
                             q->dy);
        }
        for (j = jointListStart; j != NULL; j = j->next) {
            int g, i;
            bool moving = j->flags & (CollJoint_B10 | CollJoint_B9 | CollJoint_B8);
            if (j->flags & CollJoint_TooFar) {
                continue;
            }
            for (g = 0; g < 2; g++) {
                struct MapLineRange r =
                    j->inner->ranges[g ? MapLineGroup_Dynamic : group];
                for (i = r.start; i < r.start + r.count; i++) {
                    CollLine* l = &lines[i];
                    CollVtx* v0 = &vtx[l->x0->v0_idx];
                    CollVtx* v1 = &vtx[l->x0->v1_idx];
                    bool out;
                    if (!(l->flags & kind) || !(l->flags & LINE_FLAG_ENABLED) ||
                        (l->flags & LINE_FLAG_EMPTY))
                    {
                        continue;
                    }
                    switch (f) {
                    case F_FLOOR:
                        out = mpLineBoxOut(&fbox, 0.0F, 0.0F, v0->pos.x,
                                           v0->pos.y, v1->pos.x, v1->pos.y);
                        break;
                    case F_800511A4:
                    case F_800515A0:
                        st_lines[f]++;
                        st_skipped[f] += mpEdgeBoxOut(&edge, &reach, v0->pos.x,
                                                      v0->pos.y, v0->x10,
                                                      v0->x14);
                        out = mpEdgeBoxOut(&edge, &reach, v1->pos.x, v1->pos.y,
                                           v1->x10, v1->x14);
                        break;
                    default:
                        if (moving && f != F_LEFT && f != F_RIGHT) {
                            out = mpLineBoxOut(
                                &box,
                                mpRemapReach(v0->x10, v1->x10, v0->pos.x,
                                             v1->pos.x),
                                mpRemapReach(v0->x14, v1->x14, v0->pos.y,
                                             v1->pos.y),
                                v0->pos.x, v0->pos.y, v1->pos.x, v1->pos.y);
                        } else {
                            out = mpLineBoxOut(&box, 0.0F, 0.0F, v0->pos.x,
                                               v0->pos.y, v1->pos.x,
                                               v1->pos.y);
                        }
                        break;
                    }
                    st_lines[f]++;
                    st_skipped[f] += out;
                }
            }
        }
    }
    mpUncheckBounding();
}

/* a query from a fighter on the Fountain-like stage: standing, running or
 * in the air over it, probing down, sweeping its ECB */
static void fighter_query(Query* q)
{
    float x = frand(-75.0F, 75.0F);
    float y = chance(60) ? 0.62F : frand(-20.0F, 80.0F);
    float h = frand(6.0F, 18.0F), w = frand(3.0F, 6.0F);
    memset(q, 0, sizeof(*q));
    q->line_id_skip = q->joint_id_skip = q->joint_id_only = -1;
    switch (rnd_below(3)) {
    case 0: /* floor probe */
        q->ax = q->bx = x;
        q->ay = y + 5.0F;
        q->by = y - 5.0F;
        break;
    case 1: /* the ECB's bottom moving */
        q->ax = x;
        q->ay = y;
        q->bx = x + frand(-2.5F, 2.5F);
        q->by = y + frand(-3.0F, 3.0F);
        break;
    default: /* a side of the ECB moving */
        q->ax = x + (chance(50) ? w : -w);
        q->ay = y + h * 0.5F;
        q->bx = q->ax + frand(-2.5F, 2.5F);
        q->by = q->ay + frand(-3.0F, 3.0F);
        break;
    }
    q->cx = x - w;
    q->cy = y + h * 0.5F;
    q->dx = x;
    q->dy = y + h;
}

int main(int argc, char** argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 40000;
    long r;
    int f, k;
    Query q;

    for (r = 0; r < rounds; r++) {
        int kind = r % 4;
        if (kind == 0) {
            build_fountain();
        } else if (kind == 3) {
            build_short();
        } else {
            build_random(kind == 2);
        }
        for (k = 0; k < 8; k++) {
            make_query(&q);
            compare(&q);
            if (kind == 0) {
                fighter_query(&q);
                compare(&q);
                tally(&q);
            } else if (kind == 3) {
                short_query(&q);
                compare(&q);
            }
        }
    }

    for (f = 0; f < F_COUNT; f++) {
        printf("%-26s %8ld calls, %7ld hits; Fountain: %5.1f%% of the "
               "line%s tests skipped\n",
               f_name[f], n_runs[f], n_hits[f],
               st_lines[f] ? 100.0 * st_skipped[f] / st_lines[f] : 0.0,
               f >= F_800511A4 ? " (vertex)" : "");
    }
    if (n_fail) {
        printf("test_mplib: %d mismatches\n", n_fail);
        return 1;
    }
    printf("test_mplib: ok\n");
    return 0;
}
