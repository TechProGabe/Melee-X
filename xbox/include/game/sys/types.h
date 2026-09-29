/* pdclib (nxdk) has no <sys/types.h>; src/pc/compat.h includes it. */
#ifndef XBOX_SYS_TYPES_H
#define XBOX_SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef int32_t ssize_t;
typedef int32_t off_t;
#endif
