/* settings.c - E:\UDATA\<title>\settings.ini
 *
 *   [video]
 *   720p = 1            ; use 720p (16:9) when the dashboard allows it (default)
 *   widescreen = 1      ; 16:9 at 480 when the dashboard is set to widescreen
 *   fps = 1             ; frame-rate counter in the top-left corner
 *   [system]
 *   ram128 = 0          ; 1: use all 128 MB on an upgraded console (untested)
 *   led_effects = 0     ; front LED effects (xbox/src/hw/xhw_led.c); off by
 *                       ; default: a modchip that drives the LED fights it
 *   [input]
 *   rumble = 100        ; percent
 *   [port1] .. [port4]
 *   stick_deadzone = 20 ; percent, radial
 *   cstick_deadzone = 25
 *   trigger_click = 230 ; 0-255: analog L/R from which the digital click fires
 *   a = A               ; Xbox button = GameCube button (A B X Y Z L R START
 *   b = X               ; UP DOWN LEFT RIGHT, or NONE)
 *   ...
 *
 * Written with the defaults on first boot so it is there to edit, and by the
 * settings menu (menu.c). A save writes settings.tmp, ending in an END_MARK
 * line, and reads it back; only a copy that is complete and parses to the
 * same settings replaces settings.ini (xhw_replace_file), so a failed write
 * or a power cut leaves the old file. A settings.tmp found without a
 * settings.ini is taken if it is complete: a save whose rename was cut off
 * after settings.ini had been deleted. The writer regenerates the whole
 * file: comments and keys it doesn't know are not kept. */
#include <dolphin/pad.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xgx.h"
#include "xhw.h"
#include "xsdk_settings.h"

/* the frame-rate counter's default when settings.ini has no fps line: on in
 * test builds, off in a release (XHW_TEST_BUILD, xhw.h) */
#ifndef XSDK_FPS_DEFAULT
#define XSDK_FPS_DEFAULT XHW_TEST_BUILD
#endif

xsdk_settings g_xsdk_settings;
xsdk_settings g_xsdk_settings_boot;

static const struct { const char* name; uint32_t bit; } k_xbox[] = {
    { "a", XHW_BTN_A }, { "b", XHW_BTN_B }, { "x", XHW_BTN_X }, { "y", XHW_BTN_Y },
    { "white", XHW_BTN_WHITE }, { "black", XHW_BTN_BLACK }, { "start", XHW_BTN_START },
    { "back", XHW_BTN_BACK }, { "lstick", XHW_BTN_LSTICK }, { "rstick", XHW_BTN_RSTICK },
    { "up", XHW_BTN_UP }, { "down", XHW_BTN_DOWN }, { "left", XHW_BTN_LEFT }, { "right", XHW_BTN_RIGHT },
};
#define N_XBOX (int)(sizeof k_xbox / sizeof k_xbox[0])

static const struct { const char* name; uint16_t bits; } k_gc[] = {
    { "NONE", 0 }, { "A", PAD_BUTTON_A }, { "B", PAD_BUTTON_B }, { "X", PAD_BUTTON_X },
    { "Y", PAD_BUTTON_Y }, { "Z", PAD_TRIGGER_Z }, { "L", PAD_TRIGGER_L }, { "R", PAD_TRIGGER_R },
    { "START", PAD_BUTTON_START }, { "UP", PAD_BUTTON_UP }, { "DOWN", PAD_BUTTON_DOWN },
    { "LEFT", PAD_BUTTON_LEFT }, { "RIGHT", PAD_BUTTON_RIGHT },
};
#define N_GC (int)(sizeof k_gc / sizeof k_gc[0])

/* the default layout: GameCube-like by position */
static const char* const k_default[N_XBOX] = {
    "A", "X", "B", "Y", "Z", "Z", "START", "NONE", "NONE", "NONE", "UP", "DOWN", "LEFT", "RIGHT",
};

static uint16_t gc_bits(const char* name) {
    int i;
    for (i = 0; i < N_GC; i++)
        if (_stricmp(name, k_gc[i].name) == 0) return k_gc[i].bits;
    return 0;
}

static const char* gc_name(uint16_t bits) {
    int i;
    for (i = 0; i < N_GC; i++)
        if (k_gc[i].bits == bits) return k_gc[i].name;
    return "NONE";
}

static void defaults(xsdk_settings* st) {
    int p, i;
    memset(st, 0, sizeof *st);
    st->video_720p = 1;   /* only where the dashboard allows 720p (xhw_video.c); BACK at boot: 480i */
    st->progressive = 1;
    st->widescreen = 1;
    st->fps = XSDK_FPS_DEFAULT;
    st->shots = XHW_TEST_BUILD;
    st->led = 0;   /* a Kronos-modded console needed a Cerbios recovery (docs/decisions.md) */
    st->rumble = 1.0f;
    for (p = 0; p < 4; p++) {
        xsdk_port_settings* ps = &st->port[p];
        ps->stick_deadzone = 0.20f;
        ps->cstick_deadzone = 0.25f;
        ps->trigger_click = 230;
        for (i = 0; i < N_XBOX; i++) {
            ps->bind[i].xbox = k_xbox[i].bit;
            ps->bind[i].gc = gc_bits(k_default[i]);
        }
    }
}

static void path(char* out, size_t cap) { snprintf(out, cap, "%ssettings.ini", xhw_save_dir()); }
static void tmp_path(char* out, size_t cap) { snprintf(out, cap, "%ssettings.tmp", xhw_save_dir()); }

static char* trim(char* s) {
    char* e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int pct(float f) { return (int)(f * 100.0f + 0.5f); }

enum { SAW_FPS = 1, SAW_PROGRESSIVE = 2, SAW_RAM128 = 4, SAW_END = 8, SAW_LED = 16 };
#define SAW_KEYS (SAW_FPS | SAW_PROGRESSIVE | SAW_RAM128 | SAW_LED)   /* a file without one of these is rewritten */
#define END_MARK "; end of settings"   /* the writer's last line: the file is whole */

/* the file's keys over the defaults into *st; returns the SAW_* found */
static int parse(FILE* f, xsdk_settings* st) {
    char line[256], section[32] = "";
    int saw = 0;
    defaults(st);
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq, *key, *val;
        char* semi;
        if (strcmp(s, END_MARK) == 0) saw |= SAW_END;
        semi = strchr(s, ';');
        if (semi) *semi = '\0';
        s = trim(s);
        if (!*s) continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end) *end = '\0';
            snprintf(section, sizeof section, "%s", s + 1);
            continue;
        }
        if (!(eq = strchr(s, '='))) continue;
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        if (_stricmp(section, "video") == 0) {
            if (_stricmp(key, "720p") == 0) st->video_720p = atoi(val) != 0;
            else if (_stricmp(key, "progressive") == 0) st->progressive = atoi(val) != 0, saw |= SAW_PROGRESSIVE;
            else if (_stricmp(key, "widescreen") == 0) st->widescreen = atoi(val) != 0;
            else if (_stricmp(key, "fps") == 0) st->fps = atoi(val) != 0, saw |= SAW_FPS;
        } else if (_stricmp(section, "system") == 0) {
            if (_stricmp(key, "ram128") == 0) st->ram128 = atoi(val) != 0, saw |= SAW_RAM128;
            else if (_stricmp(key, "screenshots") == 0) st->shots = atoi(val) != 0;
            /* not "led": v43-v48 wrote led = 1 as the default, and that
             * line is ignored so the effects start off for everyone */
            else if (_stricmp(key, "led_effects") == 0) st->led = atoi(val) != 0, saw |= SAW_LED;
        } else if (_stricmp(section, "input") == 0) {
            if (_stricmp(key, "rumble") == 0) st->rumble = clampi(atoi(val), 0, 100) / 100.0f;
        } else if (_strnicmp(section, "port", 4) == 0 && section[4] >= '1' && section[4] <= '4') {
            xsdk_port_settings* ps = &st->port[section[4] - '1'];
            int i;
            if (_stricmp(key, "stick_deadzone") == 0) ps->stick_deadzone = clampi(atoi(val), 0, 60) / 100.0f;
            else if (_stricmp(key, "cstick_deadzone") == 0) ps->cstick_deadzone = clampi(atoi(val), 0, 60) / 100.0f;
            else if (_stricmp(key, "trigger_click") == 0) ps->trigger_click = (uint8_t)clampi(atoi(val), 1, 255);
            else
                for (i = 0; i < N_XBOX; i++)
                    if (_stricmp(key, k_xbox[i].name) == 0) ps->bind[i].gc = gc_bits(val);
        }
    }
    return saw;
}

/* equal as the file holds them (percentages rounded) */
int xsdk_settings_equal(const xsdk_settings* a, const xsdk_settings* b) {
    int p, i;
    if (a->video_720p != b->video_720p || a->progressive != b->progressive || a->widescreen != b->widescreen ||
        a->fps != b->fps || a->ram128 != b->ram128 || a->shots != b->shots || a->led != b->led ||
        pct(a->rumble) != pct(b->rumble))
        return 0;
    for (p = 0; p < 4; p++) {
        const xsdk_port_settings *x = &a->port[p], *y = &b->port[p];
        if (pct(x->stick_deadzone) != pct(y->stick_deadzone) || pct(x->cstick_deadzone) != pct(y->cstick_deadzone) ||
            x->trigger_click != y->trigger_click)
            return 0;
        for (i = 0; i < N_XBOX; i++)
            if (x->bind[i].gc != y->bind[i].gc) return 0;
    }
    return 1;
}

static void log_summary(const char* what) {
    const xsdk_settings* st = &g_xsdk_settings;
    const xsdk_port_settings* p1 = &st->port[0];
    xhw_logf("[SETTINGS] %s: 720p %d, progressive %d, widescreen %d, fps %d, ram128 %d, screenshots %d, led %d, "
             "rumble %d, port 1 dead zones %d/%d, trigger click %d",
             what, st->video_720p, st->progressive, st->widescreen, st->fps, st->ram128, st->shots, st->led,
             pct(st->rumble),
             pct(p1->stick_deadzone), pct(p1->cstick_deadzone), p1->trigger_click);
}

static xsdk_settings s_scratch;   /* read-backs and recovery */

void xsdk_settings_load(void) {
    char p[260], t[260];
    FILE* f;
    int saw;
    path(p, sizeof p);
    tmp_path(t, sizeof t);
#ifdef XSDK_SETTINGS_RESET
    /* test builds: start from the settings.ini staged next to default.xbe
     * (MX_STAGE_EXTRA), or from none, as on a first boot */
    remove(p);
    remove(t);
    if (xhw_copy_file("D:\\settings.ini", p)) xhw_logf("[SETTINGS] %s from D:\\settings.ini (XSDK_SETTINGS_RESET)", p);
    else xhw_logf("[SETTINGS] removed %s (XSDK_SETTINGS_RESET)", p);
#endif
    f = fopen(p, "r");
    if (!f && (f = fopen(t, "r")) != NULL) {
        /* settings.ini is only deleted once settings.tmp has been read back
         * whole (xhw_replace_file's fallback): a power cut between the two.
         * A settings.tmp cut off mid-write has no END_MARK and is left. */
        int whole = (parse(f, &s_scratch) & SAW_END) != 0;
        fclose(f);
        f = NULL;
        if (!whole) xhw_logf("[SETTINGS] settings.tmp is incomplete: not used");
        else if (xhw_replace_file(t, p) == XHW_REPLACE_OK) {
            xhw_logf("[SETTINGS] %s taken from settings.tmp (an unfinished save)", p);
            f = fopen(p, "r");
        }
    }
    if (!f) {
        defaults(&g_xsdk_settings);
        xsdk_settings_save();
        g_xsdk_settings_boot = g_xsdk_settings;
        xgx_set_fps_overlay(g_xsdk_settings.fps);
        xhw_pad_set_shots(g_xsdk_settings.shots);
        xhw_led_enable(g_xsdk_settings.led);
        return;
    }
    saw = parse(f, &g_xsdk_settings);
    fclose(f);
    xhw_logf("[SETTINGS] loaded %s", p);
    /* a hand-edited ram128 = 1 on a 64 MB console: off (written as 0 by
     * the next save) */
    if (g_xsdk_settings.ram128 && !xhw_mem_has_upper()) {
        g_xsdk_settings.ram128 = 0;
        xhw_logf("[SETTINGS] ram128 = 1 ignored: this console has 64 MB");
    }
    log_summary("in use");
    if ((saw & SAW_KEYS) != SAW_KEYS)
        xsdk_settings_save();   /* add the missing lines */
    g_xsdk_settings_boot = g_xsdk_settings;
    xgx_set_fps_overlay(g_xsdk_settings.fps);
    xhw_pad_set_shots(g_xsdk_settings.shots);
    xhw_led_enable(g_xsdk_settings.led);
}

static void write_all(FILE* f, const xsdk_settings* st) {
    int port, i;
    fprintf(f, "; Melee-X settings. Buttons: Xbox = GameCube (A B X Y Z L R START UP DOWN LEFT RIGHT NONE)\n");
    fprintf(f, "; Video: 720p = 1 uses 720p when the dashboard allows it. progressive = 0 forces 480i.\n");
    fprintf(f, "; Hold BACK while Melee-X starts for 480i (sets 720p = 0 and progressive = 0).\n");
    fprintf(f, "; BACK on the title screen opens a menu for the settings above the buttons.\n");
    fprintf(f, "[video]\n720p = %d\nprogressive = %d\nwidescreen = %d\nfps = %d\n\n", st->video_720p, st->progressive,
            st->widescreen, st->fps);
    fprintf(f, "; ram128 = 1 uses the RAM above 64 MB on an upgraded console (untested; off: it runs as 64 MB).\n");
    fprintf(f, "; screenshots = 1: BACK saves a screenshot (shotNN.bmp, next to this file).\n");
    fprintf(f, "; led_effects = 1: the front LED flashes on KOs, in the last seconds and on GAME!.\n");
    fprintf(f, "; Leave it 0 on a console with a modchip that drives the LED (Kronos and similar).\n");
    fprintf(f, "[system]\nram128 = %d\nscreenshots = %d\nled_effects = %d\n\n", st->ram128, st->shots, st->led);
    fprintf(f, "[input]\nrumble = %d\n\n", pct(st->rumble));
    for (port = 0; port < 4; port++) {
        const xsdk_port_settings* ps = &st->port[port];
        fprintf(f, "[port%d]\nstick_deadzone = %d\ncstick_deadzone = %d\ntrigger_click = %d\n", port + 1,
                pct(ps->stick_deadzone), pct(ps->cstick_deadzone), ps->trigger_click);
        for (i = 0; i < N_XBOX; i++) fprintf(f, "%s = %s\n", k_xbox[i].name, gc_name(ps->bind[i].gc));
        fprintf(f, "\n");
    }
    fprintf(f, END_MARK "\n");
}

int xsdk_settings_save(void) {
    char p[260], t[260];
    FILE* f;
    int ok, r = XHW_REPLACE_FAILED;
    path(p, sizeof p);
    tmp_path(t, sizeof t);
    f = fopen(t, "w");
    if (!f) {
        xhw_logf("[SETTINGS] save failed: can't create %s (errno %d)", t, errno);
        return 0;
    }
    write_all(f, &g_xsdk_settings);
    xhw_flush(f);
    ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    /* only a complete copy replaces settings.ini */
    if (ok && (f = fopen(t, "r")) != NULL) {
        ok = (parse(f, &s_scratch) & SAW_END) && xsdk_settings_equal(&s_scratch, &g_xsdk_settings);
        fclose(f);
    } else {
        ok = 0;
    }
    if (ok) r = xhw_replace_file(t, p);
    if (r == XHW_REPLACE_LOST_TO) {
        /* settings.ini is gone, settings.tmp is complete: the next boot takes it */
        xhw_logf("[SETTINGS] save failed (rename): %s deleted, settings.tmp kept for the next boot", p);
        return 0;
    }
    if (r != XHW_REPLACE_OK) {
        xhw_logf("[SETTINGS] save failed (%s, errno %d): %s left as it was", ok ? "rename" : "write or read-back",
                 errno, p);
        remove(t);
        return 0;
    }
    log_summary("saved");
    return 1;
}
