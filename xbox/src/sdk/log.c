/* log.c - pdclib's stdout and stderr are dead handles on nxdk, so the game's
 * printf-family output is routed to the Xbox log (COM1 + boot.log). The game
 * units are compiled with printf/fprintf/vfprintf/puts renamed to these
 * (xbox/include/game/xbox_game_prelude.h); the SDK and src/pc units too. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xhw.h"
#include "xsdk.h"

/* OSReport text arrives in pieces ("# ", then the rest, then "\n"); keep a
 * line buffer so the log gets whole lines. */
static char s_line[512];
static size_t s_len;

void xsdk_log_raw(const char* text) {
    for (; *text; text++) {
        if (*text == '\n' || s_len == sizeof s_line - 1) {
            s_line[s_len] = '\0';
            xhw_log(s_line);
            s_len = 0;
            if (*text == '\n') continue;
        }
        s_line[s_len++] = *text;
    }
}

int xsdk_vprintf(const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    xsdk_log_raw(buf);
    return n;
}

int xsdk_printf(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = xsdk_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int xsdk_vfprintf(FILE* f, const char* fmt, va_list ap) {
    if (f == stdout || f == stderr) return xsdk_vprintf(fmt, ap);
    return vfprintf(f, fmt, ap);
}

int xsdk_fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = xsdk_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int xsdk_puts(const char* s) {
    xsdk_log_raw(s);
    xsdk_log_raw("\n");
    return 0;
}

int xsdk_fputs(const char* s, FILE* f) {
    if (f == stdout || f == stderr) {
        xsdk_log_raw(s);
        return 0;
    }
    return fputs(s, f);
}
