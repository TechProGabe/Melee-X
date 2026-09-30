/* Force-included first into every game translation unit on the Xbox
 * (tools/xbox/compile_game.py). Keep it small: anything here changes every
 * game object. */
#ifndef XBOX_GAME_PRELUDE_H
#define XBOX_GAME_PRELUDE_H

/* nxdk's libc headers are written for its i386-pc-win32 (MSVC) triple, where
 * __int64 is a keyword. The game triple is i686-pc-windows-gnu. */
#define __int64 long long

/* aurora's dolphin/os.h uses va_list without including <stdarg.h>; glibc and
 * MinGW happened to provide it through <math.h>, pdclib does not. */
#include <stdarg.h>

/* pdclib's <math.h> has no POSIX constants (glibc and MinGW define them). */
#include <math.h>
/* The game defines its own atan2f, acosf, asinf, expf and powf (Melee's
 * MSL versions, part of the simulation). pdclib puts the float versions in
 * the same objects as the double ones, so linking pulls in both: the game's
 * are renamed instead. */
#define atan2f melee_atan2f
#define acosf melee_acosf
#define asinf melee_asinf
#define expf melee_expf
#define powf melee_powf
/* <math.h> is already in (its guard keeps later includes out), so without
 * these the renamed functions are implicitly declared: int return, double
 * argument, and the float result in st(0) is never read. */
float melee_atan2f(float, float);
float melee_acosf(float);
float melee_asinf(float);
float melee_expf(float);
float melee_powf(float, float);
/* melee-pc links its vendored musl sinf/cosf/tanf/atanf (src/pc/libm) in
 * place of the platform's, so every build rounds the simulation alike;
 * nxdk's are bare x87 fsin/fptan/fpatan. */
#define sinf pc_sinf
#define cosf pc_cosf
#define tanf pc_tanf
#define atanf pc_atanf
float pc_sinf(float);
float pc_cosf(float);
float pc_tanf(float);
float pc_atanf(float);

#ifndef M_PI
#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440
#endif

/* memcpy & co. as builtins despite -ffreestanding (xbox/include/xbuiltin.h) */
#include "../xbuiltin.h"

/* pdclib's stdout and stderr are dead handles on nxdk: printf-family output
 * goes to the Xbox log instead (xbox/src/sdk/log.c defines these). */
#include <stdio.h>
#ifndef XSDK_NO_STDIO_RENAME
int xsdk_printf(const char* fmt, ...);
int xsdk_vprintf(const char* fmt, va_list ap);
int xsdk_fprintf(FILE* f, const char* fmt, ...);
int xsdk_vfprintf(FILE* f, const char* fmt, va_list ap);
int xsdk_puts(const char* s);
int xsdk_fputs(const char* s, FILE* f);
#define printf xsdk_printf
#define vprintf xsdk_vprintf
#define fprintf xsdk_fprintf
#define vfprintf xsdk_vfprintf
#define puts xsdk_puts
#define fputs xsdk_fputs
#endif

#endif
