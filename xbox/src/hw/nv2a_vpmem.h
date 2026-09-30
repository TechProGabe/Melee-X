/* nv2a_vpmem.h - which vertex programs are resident in the NV2A's program
 * memory (136 instructions), and where a program goes when it has to be
 * loaded. Bookkeeping only, no hardware access: nv2a.c calls it on every
 * program switch, and tools/xbox/vp_policy.py replays traces through the
 * same code on the host. */
#ifndef NV2A_VPMEM_H
#define NV2A_VPMEM_H
#include <stdint.h>

#define VPM_CAP 136      /* program memory, instructions */
#define VPM_PROGS 64     /* program ids: the back end's cache entries */
#define VPM_SEQ 1024     /* selects of a frame remembered for the next */

typedef struct {
    int16_t start[VPM_PROGS];   /* first instruction, -1: not resident */
    uint8_t len[VPM_PROGS];
    uint8_t res[VPM_PROGS];     /* the resident ids */
    int nres;
    uint32_t last[VPM_PROGS];   /* select count at its last use */
    uint32_t now;
    /* this frame's selects, and the previous frame's grouped by program */
    int i;                                /* selects this frame */
    uint8_t cur[VPM_SEQ];
    uint16_t prev_first[VPM_PROGS + 1];   /* program p: prev_pos[prev_first[p] .. prev_first[p + 1]) */
    uint16_t prev_pos[VPM_SEQ];
    int prev_len;
    uint8_t stale[VPM_PROGS];             /* dropped this frame: prev_pos is another program's */
} VpMem;

void vpm_init(VpMem* m);
/* Program `id` (n instructions) is selected. Returns 1 if it has to be
 * loaded (at *start), 0 if it is resident there already. */
int vpm_select(VpMem* m, int id, int n, int* start);
/* id's cache entry is about to hold another program: its memory is free
 * and what the previous frame says about it is void */
void vpm_drop(VpMem* m, int id);
/* at the end of every frame */
void vpm_frame(VpMem* m);
int vpm_size(void);

#endif
