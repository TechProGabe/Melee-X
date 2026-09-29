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
 * Example: "600 A" presses A at frame 600; "900 SHOT" takes a screenshot. */
#include <windows.h>
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
    uint8_t lt, rt, shot, shot_done;
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

static void parse_line(char* line) {
    char* tok;
    char* save = line;
    Event e;
    char* hash = strchr(line, '#');
    if (hash) *hash = '\0';
    tok = next_tok(&save);
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
}

void xhw_autopad_apply(int port, xhw_pad* out) {
    unsigned f = xhw_frame_count();
    int i;
    if (port != 0 || !s_nev) return;
    out->connected = 1;
    for (i = 0; i < s_nev; i++) {
        Event* e = &s_ev[i];
        if (e->shot) {
            if (!e->shot_done && f >= e->frame) {
                e->shot_done = 1;
                xhw_logf("[AUTOPAD] SHOT at frame %u", f);
                xgx_fbdump_next();
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
#endif
