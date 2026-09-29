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

#endif
