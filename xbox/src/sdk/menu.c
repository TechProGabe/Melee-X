/* menu.c - the Melee-X settings menu: settings.ini without a text editor.
 *
 * BACK on the title screen opens it. gmtitle.c calls xsdk_menu_title_frame
 * every title frame (PORT); while the menu is up the title stands still (no
 * attract demo, no START) and PADRead hands the game neutral input. The
 * platform draws it over the finished frame (xgx_set_overlay, the CPU
 * writing into the framebuffer in xhw_overlay.c) rather than the game:
 * Melee's own menus are models and prebaked text, and a page in its Options
 * menu would mean new menu data. Layout and the restart handling follow
 * OpenCrossing-Xbox's Options page (xbox_settings_menu.c).
 *
 *   Up/Down (D-pad or left stick)   select
 *   Left/Right, A                   change
 *   B, BACK                         save settings.ini and close
 *
 * Buttons are read raw, before the per-port mapping, so a remapped
 * controller still finds its way. The frame-rate counter and rumble apply at
 * once (rumble with a short pulse at the new strength), the dead zones and
 * trigger click as soon as the menu closes. Video output, widescreen and
 * the 128 MB setting are read at boot (the video mode, the NV2A's buffers
 * and the memory pools are set up then): they are saved and marked for a
 * restart, which "Save and restart" does at once. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "xgx.h"
#include "xhw.h"
#include "xsdk.h"
#include "xsdk_settings.h"

enum {
    ROW_VIDEO, ROW_WIDE, ROW_FPS, ROW_SHOTS, ROW_RAM, ROW_RUMBLE, ROW_PORT, ROW_STICK, ROW_CSTICK, ROW_TRIGGER,
    ROW_RESTART, ROW_CLOSE, N_ROWS
};
static const char* const k_label[N_ROWS] = {
    "Video output", "Widescreen (16:9)", "Frame-rate counter", "BACK screenshots", "Use 128 MB RAM", "Rumble", "Controller",
    "  Stick dead zone", "  C-stick dead zone", "  Trigger click", "Save and restart", "Save and close",
};

#define DIRS (XHW_BTN_UP | XHW_BTN_DOWN | XHW_BTN_LEFT | XHW_BTN_RIGHT)
#define STICK_DIR 16000      /* left stick as a d-pad: about half tilt */
#define REPEAT_DELAY 18      /* held direction: first repeat, then every REPEAT_EVERY frames */
#define REPEAT_EVERY 5
#define MSG_FRAMES 240       /* the hint line's message after closing */
#define RELEASE_FRAMES 60    /* after closing, input stays blocked until release, at most this long */

static int s_open, s_sel, s_port;
static uint32_t s_prev;      /* raw buttons last title frame, stick folded into the d-pad bits */
static int s_hold;           /* frames the same direction has been held */
static unsigned s_seen;      /* xsdk_frame_count() at the last title frame */
static int s_release;        /* frames left to wait for the closing press to be let go */
static int s_rumble;         /* frames left of the rumble preview */
static int s_msg_frames;
static int s_unsaved;        /* closed without saving: s_at_open still holds what settings.ini has */
static char s_msg[XGX_OVERLAY_COLS];
static int s_dash_720p, s_dash_480p, s_dash_wide;   /* the dashboard's video settings */
static int s_has_128;        /* RAM above 64 MB: the 128 MB row can be turned on */
static xsdk_settings s_at_open;
static xgx_overlay s_ovl;

static int pct(float f) { return (int)(f * 100.0f + 0.5f); }

/* one step of `inc` to the next multiple of it; clamped (never against
 * `dir`: a file value below `lo` stays put on Left), or wrapped round */
static int step(int v, int dir, int inc, int lo, int hi, int wrap) {
    int old = v;
    v = dir > 0 ? (v / inc + 1) * inc : ((v + inc - 1) / inc - 1) * inc;
    if (v > hi) v = wrap ? lo : old > hi ? old : hi;
    if (v < lo) v = wrap ? hi : old < lo ? old : lo;
    return v;
}

static int restart_needed(void) {
    const xsdk_settings *g = &g_xsdk_settings, *b = &g_xsdk_settings_boot;
    return g->video_720p != b->video_720p || g->progressive != b->progressive || g->widescreen != b->widescreen ||
           g->ram128 != b->ram128;
}

static int video_index(const xsdk_settings* st) { return st->video_720p ? 2 : st->progressive ? 1 : 0; }
/* what the console would run with these settings: the ini only allows a
 * mode, the dashboard has to allow it too (xhw_video.c) */
static int video_used(const xsdk_settings* st) {
    if (st->video_720p && s_dash_720p) return 2;
    return st->progressive && s_dash_480p ? 1 : 0;
}
static const char* const k_video_short[3] = { "480i", "480p", "720p" };
static const char* const k_video[3] = { "480i", "480p", "720p (experimental)" };

static int stick_pct(int16_t x, int16_t y) {
    float fx = x / 32767.0f, fy = y / 32767.0f, m = sqrtf(fx * fx + fy * fy);
    return m >= 1.0f ? 100 : (int)(m * 100.0f);
}

static void row_text(int r, char* out, size_t cap) {
    xsdk_settings* g = &g_xsdk_settings;
    const xsdk_port_settings* ps = &g->port[s_port];
    char v[32] = "";
    int sel = r == s_sel, boot_only = 0;
    switch (r) {
        case ROW_VIDEO:
            if (video_used(g) == video_index(g)) snprintf(v, sizeof v, "%s", k_video[video_index(g)]);
            else snprintf(v, sizeof v, "%s -> %s", k_video_short[video_index(g)], k_video_short[video_used(g)]);
            boot_only = video_index(g) != video_index(&g_xsdk_settings_boot);
            break;
        case ROW_WIDE:
            /* 720p is 16:9 whatever this says; at 480 the dashboard must agree */
            if (g->widescreen && !s_dash_wide && video_used(g) < 2) snprintf(v, sizeof v, "On -> Off");
            else snprintf(v, sizeof v, "%s", g->widescreen ? "On" : "Off");
            boot_only = g->widescreen != g_xsdk_settings_boot.widescreen;
            break;
        case ROW_FPS: snprintf(v, sizeof v, "%s", g->fps ? "On" : "Off"); break;
        case ROW_SHOTS: snprintf(v, sizeof v, "%s", g->shots ? "On" : "Off"); break;
        case ROW_RAM:
            snprintf(v, sizeof v, "%s", !s_has_128 ? "Off (64 MB console)" : g->ram128 ? "On" : "Off");
            boot_only = g->ram128 != g_xsdk_settings_boot.ram128;
            break;
        case ROW_RUMBLE:
            if (pct(g->rumble)) snprintf(v, sizeof v, "%d%%", pct(g->rumble));
            else snprintf(v, sizeof v, "Off");
            break;
        case ROW_PORT:
            snprintf(v, sizeof v, "Port %d%s", s_port + 1, xsdk_pad_raw(s_port) ? "" : " (none)");
            break;
        case ROW_STICK: snprintf(v, sizeof v, "%d%%", pct(ps->stick_deadzone)); break;
        case ROW_CSTICK: snprintf(v, sizeof v, "%d%%", pct(ps->cstick_deadzone)); break;
        case ROW_TRIGGER: snprintf(v, sizeof v, "%d", ps->trigger_click); break;
    }
    if (!v[0]) snprintf(out, cap, "%s %s", sel ? ">" : " ", k_label[r]);
    else if (sel && !(r == ROW_RAM && !s_has_128))   /* no arrows on the locked row */
        snprintf(out, cap, "> %-20s < %s >%s", k_label[r], v, boot_only ? " *" : "");
    else if (sel) snprintf(out, cap, "> %-20s   %s", k_label[r], v);
    else snprintf(out, cap, "  %-20s   %s%s", k_label[r], v, boot_only ? " *" : "");
}

/* the line under the list: about the selected row */
static void info_text(char* out, size_t cap) {
    const xsdk_settings* g = &g_xsdk_settings;
    const xsdk_port_settings* ps = &g->port[s_port];
    const xhw_pad* p = xsdk_pad_raw(s_port);
    int m;
    switch (s_sel) {
        case ROW_VIDEO:
            if (g->video_720p && !s_dash_720p) snprintf(out, cap, "720p is off in the dashboard: 480 is used");
            else if (!g->video_720p && g->progressive && !s_dash_480p)
                snprintf(out, cap, "480p is off in the dashboard: 480i is used");
            else snprintf(out, cap, "Applies after a restart");
            break;
        case ROW_WIDE:
            if (!s_dash_wide) snprintf(out, cap, "Also needs widescreen set in the dashboard");
            else snprintf(out, cap, "At 480 lines; 720p is always 16:9");
            break;
        case ROW_FPS: snprintf(out, cap, "Frames per second, top-left corner"); break;
        case ROW_SHOTS: snprintf(out, cap, "BACK saves shotNN.bmp next to settings.ini"); break;
        case ROW_RAM:
            snprintf(out, cap, "%s", s_has_128 ? "Upgraded consoles only (untested)" : "This console has 64 MB");
            break;
        case ROW_RUMBLE: snprintf(out, cap, "Controller motor strength"); break;
        case ROW_PORT: snprintf(out, cap, "The three settings below are per port"); break;
        case ROW_STICK:
        case ROW_CSTICK:
            if (!p) {
                snprintf(out, cap, "No controller in port %d", s_port + 1);
                break;
            }
            m = s_sel == ROW_STICK ? stick_pct(p->lx, p->ly) : stick_pct(p->rx, p->ry);
            snprintf(out, cap, "%s stick now %d%% (%s)", s_sel == ROW_STICK ? "Left" : "Right", m,
                     m > pct(s_sel == ROW_STICK ? ps->stick_deadzone : ps->cstick_deadzone) ? "moves" : "ignored");
            break;
        case ROW_TRIGGER:
            if (!p) snprintf(out, cap, "No controller in port %d", s_port + 1);
            else snprintf(out, cap, "L/R click from %d of 255 (now %d, %d)", ps->trigger_click, p->lt, p->rt);
            break;
        case ROW_RESTART: snprintf(out, cap, "Writes settings.ini, restarts Melee-X"); break;
        default: snprintf(out, cap, "Writes settings.ini (B does too)"); break;
    }
}

static void add_row(const char* text, uint32_t rgb) {
    if (s_ovl.rows >= XGX_OVERLAY_ROWS) return;
    snprintf(s_ovl.text[s_ovl.rows], XGX_OVERLAY_COLS, "%s", text);
    s_ovl.rgb[s_ovl.rows++] = rgb;
}

static void draw_menu(void) {
    const xhw_video_mode* vm = xhw_video();
    char line[XGX_OVERLAY_COLS];
    int r;
    memset(&s_ovl, 0, sizeof s_ovl);
    s_ovl.kind = XGX_OVERLAY_BOX;
    s_ovl.cols = 48;   /* the widest row: the panel keeps its size as values change */
    snprintf(line, sizeof line, "Melee-X settings         (running %s %s)",
             vm->height >= 720 ? "720p" : vm->progressive ? "480p" : "480i",
             vm->height >= 720 || (vm->widescreen && g_xsdk_settings_boot.widescreen) ? "16:9" : "4:3");
    add_row(line, 0xFFFFFF);
    add_row("", 0);
    for (r = 0; r < N_ROWS; r++) {
        row_text(r, line, sizeof line);
        add_row(line, r == s_sel                      ? 0xFFE070
                      : r == ROW_RAM && !s_has_128 ? 0x707890   /* can't be changed */
                      : r >= ROW_RESTART           ? 0xB0C0E0
                                                   : 0xE0E4F0);
    }
    add_row("", 0);
    info_text(line, sizeof line);
    add_row(line, 0xA0B4D8);
    if (s_msg_frames > 0) {
        s_msg_frames--;
        add_row(s_msg, 0xFFA040);
    } else {
        add_row(restart_needed() ? "* used after a restart" : "", 0xFFA040);
    }
    add_row("A/Left/Right change   B save and close", 0x8090B0);
    xgx_set_overlay(&s_ovl);
}

static void draw_hint(void) {
    memset(&s_ovl, 0, sizeof s_ovl);
    s_ovl.kind = XGX_OVERLAY_HINT;
    s_ovl.rows = 1;
    if (s_msg_frames > 0) {
        s_msg_frames--;
        snprintf(s_ovl.text[0], XGX_OVERLAY_COLS, "%s", s_msg);
        s_ovl.rgb[0] = 0xFFE070;
    } else {
        snprintf(s_ovl.text[0], XGX_OVERLAY_COLS, "BACK: Melee-X settings");
        s_ovl.rgb[0] = 0xC8D0E8;
    }
    xgx_set_overlay(&s_ovl);
}

static void rumble_all(float strength) {
    uint16_t lvl = (uint16_t)(strength * 0.75f * 65535.0f);   /* as PADRead drives the motors */
    int i;
    for (i = 0; i < 4; i++)
        if (xsdk_pad_raw(i)) xhw_pad_rumble(i, lvl, lvl);
}

static void set_msg(const char* m) {
    snprintf(s_msg, sizeof s_msg, "%s", m);
    s_msg_frames = MSG_FRAMES;
}

static void open_menu(void) {
    s_open = 1;
    s_sel = 0;
    s_port = 0;
    if (!s_unsaved) s_at_open = g_xsdk_settings;
    s_msg_frames = 0;
    /* the dashboard's settings don't change while the game runs */
    s_dash_720p = xhw_video_720p_allowed();
    s_dash_480p = xhw_video_480p_allowed();
    s_dash_wide = xhw_video_widescreen_set();
    s_has_128 = xhw_mem_has_upper();
    xhw_logf("[MENU] settings menu opened");
}

/* settings.ini holds the menu's settings: 1 as it was, 2 written; 0: the
 * write failed and the file is as it was */
static int save_if_changed(void) {
    if (xsdk_settings_equal(&g_xsdk_settings, &s_at_open)) return 1;
    if (!xsdk_settings_save()) return 0;
    s_at_open = g_xsdk_settings;
    s_unsaved = 0;
    return 2;
}

static void close_menu(int save) {
    int ok;
    if (s_rumble) {
        s_rumble = 0;
        rumble_all(0.0f);
    }
    s_open = 0;
    s_release = RELEASE_FRAMES;
    if (!save) {
        s_unsaved = !xsdk_settings_equal(&g_xsdk_settings, &s_at_open);   /* saved at the next close */
        return;
    }
    ok = save_if_changed();
    if (!ok) set_msg("settings.ini could not be saved");
    else if (restart_needed()) set_msg("Saved: restart Melee-X to apply");
    else if (ok == 2) set_msg("Settings saved");
    xhw_logf("[MENU] settings menu closed (%s)", !ok ? "save failed" : ok == 2 ? "saved" : "nothing changed");
}

static void change(int dir, int wrap) {
    xsdk_settings* g = &g_xsdk_settings;
    xsdk_port_settings* ps = &g->port[s_port];
    char line[XGX_OVERLAY_COLS];
    int v;
    switch (s_sel) {
        case ROW_VIDEO:
            v = step(video_index(g), dir, 1, 0, 2, wrap);
            g->video_720p = v == 2;
            if (v < 2) g->progressive = v == 1;   /* 720p keeps the 480 fallback's choice */
            break;
        case ROW_WIDE: g->widescreen = !g->widescreen; break;
        case ROW_FPS:
            g->fps = !g->fps;
            xgx_set_fps_overlay(g->fps);
            break;
        case ROW_SHOTS:
            g->shots = !g->shots;
            xhw_pad_set_shots(g->shots);
            break;
        case ROW_RAM:
            if (!s_has_128) return;   /* 64 MB: stays off */
            g->ram128 = !g->ram128;
            break;
        case ROW_RUMBLE:
            g->rumble = step(pct(g->rumble), dir, 25, 0, 100, wrap) / 100.0f;
            s_rumble = 20;   /* a third of a second at the new strength */
            break;
        case ROW_PORT: s_port = step(s_port, dir, 1, 0, 3, 1); break;
        case ROW_STICK: ps->stick_deadzone = step(pct(ps->stick_deadzone), dir, 5, 0, 60, wrap) / 100.0f; break;
        case ROW_CSTICK: ps->cstick_deadzone = step(pct(ps->cstick_deadzone), dir, 5, 0, 60, wrap) / 100.0f; break;
        case ROW_TRIGGER: ps->trigger_click = (uint8_t)step(ps->trigger_click, dir, 5, 5, 255, wrap); break;
        default: return;
    }
    row_text(s_sel, line, sizeof line);
    xhw_logf("[MENU] %s", line + 2);
}

/* raw buttons of every controller, the left stick folded into the d-pad */
static uint32_t read_buttons(void) {
    uint32_t b = 0;
    int i;
    for (i = 0; i < 4; i++) {
        const xhw_pad* p = xsdk_pad_raw(i);
        if (!p) continue;
        b |= p->buttons;
        if (p->ly > STICK_DIR) b |= XHW_BTN_UP;
        else if (p->ly < -STICK_DIR) b |= XHW_BTN_DOWN;
        if (p->lx > STICK_DIR) b |= XHW_BTN_RIGHT;
        else if (p->lx < -STICK_DIR) b |= XHW_BTN_LEFT;
    }
    return b;
}

int xsdk_menu_title_frame(void) {
    uint32_t cur = read_buttons(), press;
    unsigned now = xsdk_frame_count();
    int was_open = s_open;
    /* back on the title after a while: only presses from here on count */
    if (now - s_seen > 2) s_prev = cur;
    s_seen = now;
    press = cur & ~s_prev;
    if ((cur & DIRS) && (cur & DIRS) == (s_prev & DIRS)) {
        if (++s_hold >= REPEAT_DELAY && (s_hold - REPEAT_DELAY) % REPEAT_EVERY == 0) press |= cur & DIRS;
    } else {
        s_hold = 0;
    }
    s_prev = cur;

    if (!s_open) {
        if (press & XHW_BTN_BACK) open_menu();
    } else if (press & (XHW_BTN_B | XHW_BTN_BACK)) {
        close_menu(1);
    } else if (press & XHW_BTN_UP) {
        s_sel = (s_sel + N_ROWS - 1) % N_ROWS;
    } else if (press & XHW_BTN_DOWN) {
        s_sel = (s_sel + 1) % N_ROWS;
    } else if (press & XHW_BTN_LEFT) {
        change(-1, 0);
    } else if (press & XHW_BTN_RIGHT) {
        change(+1, 0);
    } else if (press & XHW_BTN_A) {
        if (s_sel == ROW_CLOSE) {
            close_menu(1);
        } else if (s_sel == ROW_RESTART) {
            if (save_if_changed()) {
                xhw_logf("[MENU] restart");
                xhw_reboot_self();
            }
            set_msg("settings.ini could not be saved");
        } else {
            change(+1, 1);
        }
    }

    if (s_rumble > 0) rumble_all(--s_rumble ? g_xsdk_settings.rumble : 0.0f);
    if (s_open) draw_menu();
    else draw_hint();
    return was_open || s_open;
}

int xsdk_menu_block(uint32_t raw_buttons) {
    if (s_open && xsdk_frame_count() - s_seen > 30) {
        /* the title went away under the menu (it shouldn't: it stands
         * still). No file writes from inside PADRead: the changes stay in
         * memory and are written when the menu next closes. */
        xhw_logf("[MENU] title screen gone, menu closed without saving");
        close_menu(0);
    }
    if (s_open) return 1;
    if (s_release > 0 && raw_buttons) {
        s_release--;
        return 1;
    }
    s_release = 0;
    return 0;
}
