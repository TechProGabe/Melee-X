/* xgx_probe.h - the frame-rate probes the game's own code takes part in
 * (docs/fps-plan.md steps 0.3 and 1.3). Included by game code (PORT: gobj.c,
 * lbshadow.c, grizumi.c) and by the platform code.
 *
 * Draw census (-DXGX_CENSUS=1, xbox/src/hw/nv2a.c): the game's render paths
 * keep xgx_census_tag current: bits 0-7 the p_link class of the GObj whose
 * render callback is running (0xFF: none), bits 8-15 the pass. It is a plain
 * store in every build; only a census build reads it.
 *
 * Ablations (test builds, xbox/src/hw/xhw_pmc.c): xhw_ablate(n) is 1 while
 * the probe's rotation runs window n (an autopad script's `env MX_ABLATE=1`)
 * and 0 otherwise, always 0 in a release. */
#ifndef XGX_PROBE_H
#define XGX_PROBE_H

extern unsigned int xgx_census_tag;

enum {
    XGX_PASS_MAIN = 0,
    XGX_PASS_SHADOW = 1,    /* a fighter's shadow map (lbshadow.c) */
    XGX_PASS_REFLECT = 2,   /* Fountain of Dreams' reflection (grizumi.c) */
};

#define XGX_CENSUS_OWNER(tag, owner) (((tag) & ~0xFFu) | ((owner) & 0xFFu))
#define XGX_CENSUS_PASS(tag, pass) (((tag) & 0xFFu) | ((unsigned int) (pass) << 8))

/* the probe's windows, in rotation order; 7 is the baseline again (drift) */
enum {
    XHW_AB_NONE = 0,
    XHW_AB_FTZ = 1,       /* MXCSR flush-to-zero on the game and mixer threads */
    XHW_AB_SHADOW = 2,    /* fighter shadow maps not rendered */
    XHW_AB_REFLECT = 3,   /* Fountain of Dreams' reflection not rendered */
    XHW_AB_BACKEND = 4,   /* xgx_draw returns at once: black frames */
    XHW_AB_RECHECK = 5,   /* display-list content rechecks off */
    XHW_AB_AUDIO = 6,     /* audio mixer off: silence */
    XHW_AB_WINDOWS = 8
};
int xhw_ablate(int window);

#endif
