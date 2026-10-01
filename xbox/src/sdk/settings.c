/* settings.c - E:\UDATA\<title>\settings.ini
 *
 *   [video]
 *   720p = 1            ; use 720p (16:9) when the dashboard allows it
 *   widescreen = 1      ; 16:9 at 480 when the dashboard is set to widescreen
 *   fps = 1             ; frame-rate counter in the top-left corner
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
 * Written with the defaults on first boot so it is there to edit. */
#include <dolphin/pad.h>
#include <ctype.h>
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

static void defaults(void) {
    int p, i;
    memset(&g_xsdk_settings, 0, sizeof g_xsdk_settings);
    g_xsdk_settings.video_720p = 1;
    g_xsdk_settings.widescreen = 1;
    g_xsdk_settings.fps = XSDK_FPS_DEFAULT;
    g_xsdk_settings.rumble = 1.0f;
    for (p = 0; p < 4; p++) {
        xsdk_port_settings* ps = &g_xsdk_settings.port[p];
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

static char* trim(char* s) {
    char* e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void xsdk_settings_load(void) {
    char p[260], line[256], section[32] = "";
    FILE* f;
    int saw_fps = 0;
    defaults();
    path(p, sizeof p);
    f = fopen(p, "r");
    if (!f) {
        xsdk_settings_save();
        xgx_set_fps_overlay(g_xsdk_settings.fps);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq, *key, *val;
        char* semi = strchr(s, ';');
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
            if (_stricmp(key, "720p") == 0) g_xsdk_settings.video_720p = atoi(val) != 0;
            else if (_stricmp(key, "widescreen") == 0) g_xsdk_settings.widescreen = atoi(val) != 0;
            else if (_stricmp(key, "fps") == 0) g_xsdk_settings.fps = atoi(val) != 0, saw_fps = 1;
        } else if (_stricmp(section, "input") == 0) {
            if (_stricmp(key, "rumble") == 0) g_xsdk_settings.rumble = clampi(atoi(val), 0, 100) / 100.0f;
        } else if (_strnicmp(section, "port", 4) == 0 && section[4] >= '1' && section[4] <= '4') {
            xsdk_port_settings* ps = &g_xsdk_settings.port[section[4] - '1'];
            int i;
            if (_stricmp(key, "stick_deadzone") == 0) ps->stick_deadzone = clampi(atoi(val), 0, 60) / 100.0f;
            else if (_stricmp(key, "cstick_deadzone") == 0) ps->cstick_deadzone = clampi(atoi(val), 0, 60) / 100.0f;
            else if (_stricmp(key, "trigger_click") == 0) ps->trigger_click = (uint8_t)clampi(atoi(val), 1, 255);
            else
                for (i = 0; i < N_XBOX; i++)
                    if (_stricmp(key, k_xbox[i].name) == 0) ps->bind[i].gc = gc_bits(val);
        }
    }
    fclose(f);
    xhw_logf("[SETTINGS] loaded %s", p);
    if (!saw_fps) xsdk_settings_save();   /* a file from before the fps line: add it */
    xgx_set_fps_overlay(g_xsdk_settings.fps);
}

void xsdk_settings_save(void) {
    char p[260];
    FILE* f;
    int port, i;
    path(p, sizeof p);
    f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "; Melee-X settings. Buttons: Xbox = GameCube (A B X Y Z L R START UP DOWN LEFT RIGHT NONE)\n");
    fprintf(f, "[video]\n720p = %d\nwidescreen = %d\nfps = %d\n\n", g_xsdk_settings.video_720p,
            g_xsdk_settings.widescreen, g_xsdk_settings.fps);
    fprintf(f, "[input]\nrumble = %d\n\n", (int)(g_xsdk_settings.rumble * 100.0f + 0.5f));
    for (port = 0; port < 4; port++) {
        const xsdk_port_settings* ps = &g_xsdk_settings.port[port];
        fprintf(f, "[port%d]\nstick_deadzone = %d\ncstick_deadzone = %d\ntrigger_click = %d\n", port + 1,
                (int)(ps->stick_deadzone * 100.0f + 0.5f), (int)(ps->cstick_deadzone * 100.0f + 0.5f),
                ps->trigger_click);
        for (i = 0; i < N_XBOX; i++) fprintf(f, "%s = %s\n", k_xbox[i].name, gc_name(ps->bind[i].gc));
        fprintf(f, "\n");
    }
    xhw_flush(f);
    fclose(f);
}
