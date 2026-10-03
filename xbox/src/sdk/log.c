/* log.c - pdclib's stdout and stderr are dead handles on nxdk, so the game's
 * printf-family output is routed to the Xbox log (COM1 + boot.log). The game
 * units are compiled with printf/fprintf/vfprintf/puts renamed to these
 * (xbox/include/game/xbox_game_prelude.h); the SDK and src/pc units too. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xgx.h"
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

/* ---- vsnprintf with floating point ----
 * nxdk's pdclib printf has no %f/%e/%g: it skips the conversion without
 * taking its double, so every later argument is read 4 bytes off (a "%s"
 * after a "%f" crashed the console on a pointer made of float bits). The
 * float conversions are formatted here; every other conversion goes to
 * pdclib one at a time, with its own argument, so nothing can shift. */
static void put_c(char* out, size_t cap, size_t* at, char c) {
    if (*at + 1 < cap) out[*at] = c;
    (*at)++;
}

static void put_s(char* out, size_t cap, size_t* at, const char* s) {
    for (; *s; s++) put_c(out, cap, at, *s);
}

/* |v| with `prec` decimals (v >= 0, finite, below 1e18) */
static void fmt_fixed(char* dst, size_t cap, double v, int prec) {
    unsigned long long scale = 1, ip, fr;
    int i;
    if (prec > 9) prec = 9;
    for (i = 0; i < prec; i++) scale *= 10;
    ip = (unsigned long long)v;
    fr = (unsigned long long)((v - (double)ip) * (double)scale + 0.5);
    if (fr >= scale) {
        ip++;
        fr -= scale;
    }
    if (prec) snprintf(dst, cap, "%llu.%0*llu", ip, prec, fr);
    else snprintf(dst, cap, "%llu", ip);
}

static void fmt_float(char* dst, size_t cap, double v, char conv, int prec, int plus, int space, int alt) {
    char body[64], *d = dst;
    int neg = v < 0 || (v == 0 && 1 / v < 0), e = 0, upper = conv >= 'A' && conv <= 'Z';
    char c = (char)(upper ? conv + 32 : conv);
    if (neg) v = -v;
    if (neg) *d++ = '-';
    else if (plus) *d++ = '+';
    else if (space) *d++ = ' ';
    if (v != v) {
        snprintf(d, cap - (size_t)(d - dst), upper ? "NAN" : "nan");
        return;
    }
    if (v > 1.7976931348623157e308) {
        snprintf(d, cap - (size_t)(d - dst), upper ? "INF" : "inf");
        return;
    }
    if (prec < 0) prec = 6;
    if (c == 'g') {
        double t = v;
        if (prec == 0) prec = 1;
        if (t != 0) {
            while (t >= 10) t /= 10, e++;
            while (t < 1) t *= 10, e--;
        }
        if (e < -4 || e >= prec) c = 'e', prec--;
        else c = 'f', prec = prec - 1 - e;
        if (prec < 0) prec = 0;
    }
    if (c == 'f' && v < 1e18) {
        fmt_fixed(body, sizeof body, v, prec);
    } else {   /* e, a, and f too big for the fixed path */
        e = 0;
        if (v != 0) {
            while (v >= 10) v /= 10, e++;
            while (v < 1) v *= 10, e--;
        }
        fmt_fixed(body, sizeof body, v, prec > 9 ? 9 : prec);
        if (body[0] == '1' && body[1] == '0') {   /* rounded up to 10.0 */
            v /= 10;
            e++;
            fmt_fixed(body, sizeof body, v, prec > 9 ? 9 : prec);
        }
        snprintf(body + strlen(body), sizeof body - strlen(body), "%c%c%02d", upper ? 'E' : 'e', e < 0 ? '-' : '+',
                 e < 0 ? -e : e);
    }
    if (conv == 'g' || conv == 'G') {   /* %g drops trailing zeros unless '#' */
        char* dot = strchr(body, '.');
        if (dot && !alt) {
            char* exp = strpbrk(body, "eE");
            char* end = exp ? exp : body + strlen(body);
            char* z = end;
            while (z > dot + 1 && z[-1] == '0') z--;
            if (z == dot + 1) z = dot;
            memmove(z, end, strlen(end) + 1);
        }
    }
    snprintf(d, cap - (size_t)(d - dst), "%s", body);
}

int xsdk_vsnprintf(char* out, size_t cap, const char* fmt, va_list ap) {
    size_t at = 0;
    while (*fmt) {
        char spec[32], piece[128];
        const char* start = fmt;
        int width = 0, prec = -1, minus = 0, plus = 0, space = 0, alt = 0, zero = 0, len = 0, k;
        char conv;
        if (*fmt != '%') {
            put_c(out, cap, &at, *fmt++);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') minus = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) minus = 1, width = -width;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        /* length: 1 h, 2 hh, 3 l, 4 ll/j, 5 z/t, 6 L */
        if (*fmt == 'h') len = fmt[1] == 'h' ? (fmt += 2, 2) : (fmt++, 1);
        else if (*fmt == 'l') len = fmt[1] == 'l' ? (fmt += 2, 4) : (fmt++, 3);
        else if (*fmt == 'j') len = 4, fmt++;
        else if (*fmt == 'z' || *fmt == 't') len = 5, fmt++;
        else if (*fmt == 'L') len = 6, fmt++;
        conv = *fmt;
        if (!conv) break;
        fmt++;
        piece[0] = '\0';
        /* the spec pdclib gets: flags, width and precision resolved */
        k = snprintf(spec, sizeof spec, "%%%s%s%s%s%s", minus ? "-" : "", plus ? "+" : "", space ? " " : "",
                     alt ? "#" : "", zero ? "0" : "");
        if (width) k += snprintf(spec + k, sizeof spec - (size_t)k, "%d", width);
        if (prec >= 0) k += snprintf(spec + k, sizeof spec - (size_t)k, ".%d", prec);
        switch (conv) {
            case '%': put_c(out, cap, &at, '%'); continue;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double v = len == 6 ? (double)va_arg(ap, long double) : va_arg(ap, double);
                char num[96];
                int n, pad;
                fmt_float(num, sizeof num, v, conv == 'a' ? 'e' : conv == 'A' ? 'E' : conv, prec, plus, space, alt);
                n = (int)strlen(num);
                pad = width > n ? width - n : 0;
                if (!minus && !zero) while (pad-- > 0) put_c(out, cap, &at, ' ');
                if (!minus && zero) {   /* zeros go after the sign */
                    const char* p = num;
                    if (*p == '-' || *p == '+' || *p == ' ') put_c(out, cap, &at, *p++);
                    while (pad-- > 0) put_c(out, cap, &at, '0');
                    put_s(out, cap, &at, p);
                } else {
                    put_s(out, cap, &at, num);
                }
                if (minus) while (pad-- > 0) put_c(out, cap, &at, ' ');
                continue;
            }
            case 'd': case 'i':
                snprintf(spec + k, sizeof spec - (size_t)k, len == 4 ? "lld" : "d");
                if (len == 4) snprintf(piece, sizeof piece, spec, va_arg(ap, long long));
                else {
                    int iv = len == 3 ? (int)va_arg(ap, long) : va_arg(ap, int);
                    snprintf(piece, sizeof piece, spec, len == 2 ? (signed char)iv : len == 1 ? (short)iv : iv);
                }
                break;
            case 'u': case 'o': case 'x': case 'X':
                snprintf(spec + k, sizeof spec - (size_t)k, len == 4 ? "ll%c" : "%c", conv);
                if (len == 4) snprintf(piece, sizeof piece, spec, va_arg(ap, unsigned long long));
                else {
                    unsigned uv = len == 3 ? (unsigned)va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                    snprintf(piece, sizeof piece, spec, len == 2 ? (unsigned char)uv : len == 1 ? (unsigned short)uv : uv);
                }
                break;
            case 'c':
                snprintf(spec + k, sizeof spec - (size_t)k, "c");
                snprintf(piece, sizeof piece, spec, va_arg(ap, int));
                break;
            case 's': {
                const char* sv = va_arg(ap, const char*);
                snprintf(spec + k, sizeof spec - (size_t)k, "s");
                if (!sv) sv = "(null)";
                if (width <= 0 && prec < 0) {   /* long strings: no piece-size limit */
                    put_s(out, cap, &at, sv);
                    continue;
                }
                snprintf(piece, sizeof piece, spec, sv);
                break;
            }
            case 'p':
                snprintf(spec + k, sizeof spec - (size_t)k, "p");
                snprintf(piece, sizeof piece, spec, va_arg(ap, void*));
                break;
            case 'n':
                *va_arg(ap, int*) = (int)at;
                continue;
            default:   /* unknown: copy it as written */
                while (start < fmt) put_c(out, cap, &at, *start++);
                continue;
        }
        put_s(out, cap, &at, piece);
    }
    if (cap) out[at < cap ? at : cap - 1] = '\0';
    return (int)at;
}

int xsdk_snprintf(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = xsdk_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

int xsdk_vprintf(const char* fmt, va_list ap) {
    char buf[1024];
    int n = xsdk_vsnprintf(buf, sizeof buf, fmt, ap);
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

/* Scene transitions (src/melee/gm/gm_1A3F.c, PORT:), with the memory
 * picture at that moment: these lines are flushed to disk at once, so a
 * console that freezes during a transition leaves where and how full it was. */
unsigned xsdk_frame_count(void);

void gx_tex_scene_leave(void);

void xsdk_scene_log(const char* what, int mode, int state, int scene) {
    if (!strcmp(what, "leave")) gx_tex_scene_leave();   /* the overflow texture pool goes back */
    else xhw_prof_scene_enter();                        /* the whole-match profile starts over */
    xhw_led_scene();   /* front LED back to the SMC, unless GAME!'s sweep is still running */
    xhw_logf("[SCENE] %s: mode %d state %d scene %d (retrace %u, presented %u)", what, mode, state, scene,
             xsdk_frame_count(), xgx_present_count());
    xhw_logf("[MEM] scene %s: free %u KB, MEM1+ARAM %u KB (ARAM on disc %u KB), tex pool %u of %u KB free, "
             "vertex cache %u of %u KB free",
             what, xhw_mem_free_kb(), xhw_lazy_committed_kb(), xsdk_aram_disc_kb(), xgx_tex_pool_free_kb(),
             xgx_tex_pool_kb(), xgx_vbuf_pool_free_kb(), xgx_vbuf_pool_kb());
    if (XHW_TEST_BUILD && !strcmp(what, "leave")) xhw_lazy_log_map();
}
