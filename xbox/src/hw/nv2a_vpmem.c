/* nv2a_vpmem.c - vertex-program residency (nv2a_vpmem.h).
 *
 * A match frame selects its programs in nearly the same order as the frame
 * before, and together they are several times the 136 instructions of
 * program memory, so some are loaded again every frame. Least recently
 * used does badly on that: in a frame that cycles through more than fits,
 * the program it evicts is the one needed next. So a program is placed by
 * Belady's rule (evict what is needed furthest in the future), with the
 * previous frame as the forecast: a resident program's next use is where the
 * previous frame selected it next after the current position.
 * tools/xbox/vp_policy.py replays traces through this file; on its
 * synthetic 4-CPU match that loads 15-20% fewer programs than LRU, about as
 * few as the offline optimum.
 *
 * The program goes into the smallest free gap it fits. Failing that it
 * goes into the contiguous window whose programs are needed last (ties:
 * least recently used, then fewest instructions overwritten); windows start
 * at 0, at 136 - n, or at the start or end of a resident program. Per
 * switch that is a few dozen comparisons, and a frame end sorts its selects
 * by program. */
#include <string.h>

#include "nv2a_vpmem.h"

#define FAR 0x7FFFFFFF

void vpm_init(VpMem* m) {
    memset(m, 0, sizeof *m);
    memset(m->start, 0xFF, sizeof m->start);
}

int vpm_size(void) { return (int)sizeof(VpMem); }

static void evict(VpMem* m, int r) {
    m->start[m->res[r]] = -1;
    m->res[r] = m->res[--m->nres];
}

void vpm_drop(VpMem* m, int id) {
    int r, i;
    for (r = 0; r < m->nres; r++)
        if (m->res[r] == id) {
            evict(m, r);
            break;
        }
    /* forget its selects: they were another program's */
    m->stale[id] = 1;
    for (i = 0; i < m->i && i < VPM_SEQ; i++)
        if (m->cur[i] == id) m->cur[i] = 0xFF;
}

void vpm_frame(VpMem* m) {
    int i, p, n = m->i < VPM_SEQ ? m->i : VPM_SEQ;
    uint16_t at[VPM_PROGS + 1];
    memset(m->prev_first, 0, sizeof m->prev_first);
    for (i = 0; i < n; i++)
        if (m->cur[i] < VPM_PROGS) m->prev_first[m->cur[i] + 1]++;
    for (p = 0; p < VPM_PROGS; p++) m->prev_first[p + 1] += m->prev_first[p];
    memcpy(at, m->prev_first, sizeof at);
    for (i = 0; i < n; i++)
        if (m->cur[i] < VPM_PROGS) m->prev_pos[at[m->cur[i]]++] = (uint16_t)i;
    m->prev_len = m->i;
    m->i = 0;
    memset(m->stale, 0, sizeof m->stale);
}

/* selects from now until the previous frame's next select of id */
static int next_use(const VpMem* m, int id) {
    int lo = m->prev_first[id], hi = m->prev_first[id + 1], first = lo;
    if (lo == hi || m->stale[id]) return FAR;
    while (lo < hi) {   /* first position after this one */
        int mid = (lo + hi) / 2;
        if (m->prev_pos[mid] <= m->i) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->prev_first[id + 1]) return m->prev_pos[lo] - m->i;
    return m->prev_len - m->i + m->prev_pos[first];
}

int vpm_select(VpMem* m, int id, int n, int* start) {
    int r, s, best = -1, best_gap = VPM_CAP + 1;
    int soon_best = -1, ins_best = 0;
    uint32_t lru_best = 0;
    m->now++;
    if (m->i < VPM_SEQ) m->cur[m->i] = (uint8_t)id;
    if (m->start[id] >= 0 && m->len[id] == n) {
        m->last[id] = m->now;
        m->i++;
        *start = m->start[id];
        return 0;
    }
    if (m->start[id] >= 0) vpm_drop(m, id);
    /* the smallest gap it fits */
    for (s = 0; s < VPM_CAP;) {
        int next = VPM_CAP, end = s;
        for (r = 0; r < m->nres; r++) {
            int ps = m->start[m->res[r]];
            if (ps <= s && s < ps + m->len[m->res[r]]) end = ps + m->len[m->res[r]];
            if (ps >= s && ps < next) next = ps;
        }
        if (end > s) {   /* s is inside a program */
            s = end;
            continue;
        }
        if (next - s >= n && next - s < best_gap) best = s, best_gap = next - s;
        s = next;
    }
    if (best < 0) {
        /* the window whose programs are needed last */
        int cand[2 * VPM_PROGS + 2], nc = 0, c;
        cand[nc++] = 0;
        cand[nc++] = VPM_CAP - n;
        for (r = 0; r < m->nres; r++) {
            cand[nc++] = m->start[m->res[r]];
            cand[nc++] = m->start[m->res[r]] + m->len[m->res[r]];
        }
        for (c = 0; c < nc; c++) {
            int soon = FAR, ins = 0;
            uint32_t lru = 0;
            s = cand[c];
            if (s + n > VPM_CAP) continue;
            for (r = 0; r < m->nres; r++) {
                int p = m->res[r], ps = m->start[p];
                if (ps < s + n && s < ps + m->len[p]) {
                    int u = next_use(m, p);
                    if (u < soon) soon = u;
                    if (m->last[p] > lru) lru = m->last[p];
                    ins += m->len[p];
                }
            }
            if (best < 0 || soon > soon_best ||
                (soon == soon_best && (lru < lru_best || (lru == lru_best && (ins < ins_best ||
                                                                              (ins == ins_best && s < best))))))
                best = s, soon_best = soon, lru_best = lru, ins_best = ins;
        }
        for (r = 0; r < m->nres;) {
            int p = m->res[r], ps = m->start[p];
            if (ps < best + n && best < ps + m->len[p]) evict(m, r);
            else r++;
        }
    }
    m->start[id] = (int16_t)best;
    m->len[id] = (uint8_t)n;
    m->res[m->nres++] = (uint8_t)id;
    m->last[id] = m->now;
    m->i++;
    *start = best;
    return 1;
}
