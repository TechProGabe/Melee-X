/* xbuiltin.h - memcpy, memmove, memset and memcmp as compiler builtins.
 *
 * Everything on the Xbox is built freestanding (nxdk-cc and the game's
 * flags pass -ffreestanding, which implies -fno-builtin), so clang may not
 * treat these as what they are: memcpy(v, out, 12) in the vertex decoder
 * was a real call to xhw_string.c's memcpy, a `rep movsb` of 12 bytes. The
 * console's profile had the four at ~18% of a match frame (memcpy alone
 * 12%), nearly all of it fixed-size copies and compares in the GX layer and
 * HSD. As builtins, constant sizes become inline moves and compares, and
 * the rest still calls xhw_string.c.
 *
 * sqrtf and sqrt likewise: pdclib's are an out-of-line `fsqrt`. Inline,
 * sqrtf is `sqrtss` (correctly rounded, as fsqrt then a store to float is)
 * and sqrt the same `fsqrt`, so results don't change. The elementwise
 * builtin because __builtin_sqrtf would still call sqrtf for errno.
 *
 * Force-included (after <string.h>, so its prototypes are declared first)
 * into the game units (xbox_game_prelude.h), the SDK and the hw units
 * (xbox/CMakeLists.txt). xhw_string.c defines the functions themselves and
 * is built with XBUILTIN_OFF. */
#ifndef XBUILTIN_H
#define XBUILTIN_H
#include <string.h>
#include <math.h>
#ifndef XBUILTIN_OFF
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define memset(d, c, n) __builtin_memset((d), (c), (n))
#define memcmp(a, b, n) __builtin_memcmp((a), (b), (n))
#define sqrtf(x) __builtin_elementwise_sqrt((float)(x))
#define sqrt(x) __builtin_elementwise_sqrt((double)(x))
#endif
#endif
