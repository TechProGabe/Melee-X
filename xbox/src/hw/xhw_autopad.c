/* xhw_autopad.c - scripted input for unattended xemu runs (debug builds).
 *
 * Built only with -DXHW_AUTOPAD=1. At boot it reads D:\autopad.txt (stage it
 * with MX_STAGE_EXTRA, tools/xbox/xemu_run.sh); port 1 then takes its input
 * from the script, merged over the real controller. One event per line:
 *
 *     <frame> <token>... [for <frames>]     # comment
 *
 * <frame> counts retraces (VIWaitForRetrace calls, [BEAT] "retrace"). A press
 * lasts 6 frames unless "for N" says otherwise. Tokens:
 *     A B X Y START BACK WHITE BLACK UP DOWN LEFT RIGHT   buttons (Duke names)
 *     L R                                                 triggers, fully in
 *     SL SR SU SD                                         left stick, full tilt
 *     CL CR CU CD                                         right stick (C-stick)
 *     SHOT                                                [FBDUMP] of the next frame
 *     NEXT                                                launch MX_NEXT_XBE now
 * Example: "600 A" presses A at frame 600; "900 SHOT" takes a screenshot.
 * "600 TSHOT" takes one at the match's tick 600 instead (counted as [SIMH]
 * counts, test builds): with env MX_LOCKSTEP=1 the same moment in every
 * run, however long the loading took (a console's disk varies).
 *
 * "env NAME=VALUE" lines set what getenv() returns, which reaches melee-pc's
 * test hooks: MELEE_BOOT_SCENE=vs, MELEE_DEBUG_VS_STAGE=<StKind>,
 * MELEE_DEBUG_VS=cpu4, MELEE_SEED=<n>, MELEE_NO_ATTRACT=1 (grep src/ for
 * getenv). Without the script, getenv() returns NULL as before.
 *
 * "env MX_NEXT_XBE=F:\Applications\<folder>\default.xbe": 12 s after the
 * match ends, launch that XBE (with its own folder's script). A console
 * round chains its builds this way and runs unattended; each boot keeps the
 * one before's log as boot_prev.log (docs/fps-plan.md, round 2). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xgx.h"
#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_AUTOPAD
#define XHW_AUTOPAD 0
#endif

#if XHW_AUTOPAD
typedef struct {
    unsigned frame, len;
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t lt, rt, shot, shot_done;   /* shot 2: TSHOT, frame is a match tick */
    uint8_t next;
} Event;

#define MAX_EVENTS 512
static Event s_ev[MAX_EVENTS];
static int s_nev;

static int parse_token(Event* e, const char* t) {
    static const struct { const char* name; uint32_t bit; } k_btn[] = {
        { "A", XHW_BTN_A }, { "B", XHW_BTN_B }, { "X", XHW_BTN_X }, { "Y", XHW_BTN_Y },
        { "START", XHW_BTN_START }, { "BACK", XHW_BTN_BACK }, { "WHITE", XHW_BTN_WHITE },
        { "BLACK", XHW_BTN_BLACK }, { "UP", XHW_BTN_UP }, { "DOWN", XHW_BTN_DOWN },
        { "LEFT", XHW_BTN_LEFT }, { "RIGHT", XHW_BTN_RIGHT },
    };
    size_t i;
    for (i = 0; i < sizeof k_btn / sizeof k_btn[0]; i++)
        if (!strcmp(t, k_btn[i].name)) {
            e->buttons |= k_btn[i].bit;
            return 1;
        }
    if (!strcmp(t, "L")) e->lt = 255;
    else if (!strcmp(t, "R")) e->rt = 255;
    else if (!strcmp(t, "SL")) e->lx = -32767;
    else if (!strcmp(t, "SR")) e->lx = 32767;
    else if (!strcmp(t, "SU")) e->ly = 32767;
    else if (!strcmp(t, "SD")) e->ly = -32767;
    else if (!strcmp(t, "CL")) e->rx = -32767;
    else if (!strcmp(t, "CR")) e->rx = 32767;
    else if (!strcmp(t, "CU")) e->ry = 32767;
    else if (!strcmp(t, "CD")) e->ry = -32767;
    else if (!strcmp(t, "SHOT")) e->shot = 1;
    else if (!strcmp(t, "TSHOT")) e->shot = 2;
    else if (!strcmp(t, "NEXT")) e->next = 1;
    else return 0;
    return 1;
}

/* strtok_r without relying on pdclib having it */
static char* next_tok(char** save) {
    char* p = *save;
    char* start;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (!*p) return NULL;
    start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
    if (*p) *p++ = '\0';
    *save = p;
    return start;
}

#define MAX_ENV 16
static const char* s_env[MAX_ENV];
static int s_nenv;

/* Replaces pdclib's getenv (the linker takes this object's definition). */
char* getenv(const char* name) {
    size_t n = strlen(name);
    int i;
    for (i = 0; i < s_nenv; i++)
        if (!strncmp(s_env[i], name, n) && s_env[i][n] == '=') return (char*)s_env[i] + n + 1;
    return NULL;
}

static void parse_line(char* line) {
    char* tok;
    char* save = line;
    Event e;
    char* hash = strchr(line, '#');
    if (hash) *hash = '\0';
    tok = next_tok(&save);
    if (tok && !strcmp(tok, "env")) {
        tok = next_tok(&save);
        if (tok && strchr(tok, '=') && s_nenv < MAX_ENV) {
            s_env[s_nenv++] = tok;   /* points into the static file buffer */
            xhw_logf("[AUTOPAD] env %s", tok);
        }
        return;
    }
    if (!tok || s_nev >= MAX_EVENTS) return;
    memset(&e, 0, sizeof e);
    e.frame = (unsigned)strtoul(tok, NULL, 10);
    e.len = 6;
    while ((tok = next_tok(&save)) != NULL) {
        if (!strcmp(tok, "for")) {
            tok = next_tok(&save);
            if (tok) e.len = (unsigned)strtoul(tok, NULL, 10);
        } else if (!parse_token(&e, tok)) {
            xhw_logf("[AUTOPAD] unknown token '%s' at frame %u", tok, e.frame);
        }
    }
    s_ev[s_nev++] = e;
}

/* "env MX_FILL_E=1": E: filled to the last cluster with E:\mx_fillN.bin
 * (FATX files stop at 4 GB), so a run sees the full-disk paths (card saves,
 * screenshots, settings.ini, and on the boot after, boot.log in D:\). Use a
 * throwaway copy of xemu's HDD: the image keeps the size. Without the line
 * the files are deleted. */
#define FILL_FILES 16
static void fill_e(void) {
    static char zero[64 * 1024];
    const char* on = getenv("MX_FILL_E");
    char path[32];
    HANDLE h;
    DWORD chunk = sizeof zero, w;
    ULARGE_INTEGER free_b;
    int i;
    for (i = 0; i < FILL_FILES; i++) {
        snprintf(path, sizeof path, "E:\\mx_fill%d.bin", i);
        if (!on || strcmp(on, "1") != 0) {
            DeleteFileA(path);
            continue;
        }
        if (chunk < 512) break;   /* the disk is full */
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) break;
        SetFilePointer(h, 0, NULL, FILE_END);
        chunk = sizeof zero;
        while (chunk >= 512) {   /* down to the sector: FATX's clusters are 16 KB, the last one fills too */
            if (WriteFile(h, zero, chunk, &w, NULL) && w == chunk) continue;
            if (GetFileSize(h, NULL) >= 0xFFFF0000u) break;   /* this file is at FATX's limit: the next one */
            chunk /= 2;
        }
        CloseHandle(h);
    }
    if (!on || strcmp(on, "1") != 0) return;
    if (GetDiskFreeSpaceExA("E:\\", &free_b, NULL, NULL))
        xhw_logf("[AUTOPAD] MX_FILL_E: E: filled, %u KB free", (unsigned)(free_b.QuadPart >> 10));
}

void xhw_autopad_load(void) {
    static char buf[16 * 1024];
    HANDLE h = CreateFileA("D:\\autopad.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    char *p, *nl;
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = '\0';
    for (p = buf; *p; p = nl + 1) {
        nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        parse_line(p);
        if (!nl) break;
    }
    xhw_logf("[AUTOPAD] %d events", s_nev);
    fill_e();
}

static volatile uint32_t s_match_tick;   /* xhw_autopad_tick: ticks since the match's first */
void xhw_autopad_tick(uint32_t tick) { s_match_tick = tick; }

static DWORD s_match_end;   /* GetTickCount at the match's end, 0 before */

void xhw_autopad_match_end(void) {
    if (!s_match_end) s_match_end = GetTickCount() | 1;
}

void xhw_autopad_apply(int port, xhw_pad* out) {
    unsigned f = xhw_frame_count();
    int i;
    if (port != 0) return;
    /* before the events: a round 2 script has only env lines */
    if (s_match_end && (LONG)(GetTickCount() - s_match_end) > 12000) {   /* signed: s_match_end | 1 may be 1 ms ahead */
        const char* next = getenv("MX_NEXT_XBE");
        if (next) xhw_launch_xbe(next);
        s_match_end = 0;
    }
    if (!s_nev) return;
    out->connected = 1;
    for (i = 0; i < s_nev; i++) {
        Event* e = &s_ev[i];
        if (e->next) {   /* a run without a match (the title's settings menu) chains on here */
            const char* next = getenv("MX_NEXT_XBE");
            if (f >= e->frame && next) {
                xhw_logf("[AUTOPAD] NEXT at frame %u", f);
                xhw_launch_xbe(next);
            }
            continue;
        }
        if (e->shot) {
            if (!e->shot_done && (e->shot == 2 ? s_match_tick : f) >= e->frame) {
                e->shot_done = 1;
                if (e->shot == 2) xhw_logf("[AUTOPAD] SHOT at match tick %u (frame %u)", (unsigned)s_match_tick, f);
                else xhw_logf("[AUTOPAD] SHOT at frame %u", f);
                if (xhw_running_in_xemu())
                    xgx_fbdump_next();   /* [FBDUMP] over COM1 */
                else
                    xgx_shot_next();   /* the console has no COM1: a BMP (xhw_fbdump.c) */
            }
            continue;
        }
        if (f < e->frame || f >= e->frame + e->len) continue;
        out->buttons |= e->buttons;
        if (e->lx) out->lx = e->lx;
        if (e->ly) out->ly = e->ly;
        if (e->rx) out->rx = e->rx;
        if (e->ry) out->ry = e->ry;
        if (e->lt) out->lt = e->lt;
        if (e->rt) out->rt = e->rt;
    }
}
#else
void xhw_autopad_load(void) {}
void xhw_autopad_apply(int port, xhw_pad* out) {
    (void)port;
    (void)out;
}
void xhw_autopad_match_end(void) {}
void xhw_autopad_tick(uint32_t tick) { (void)tick; }
#endif
