/* xhw_string.c - memcpy, memmove, memset and memcmp for the whole program.
 *
 * nxdk's pdclib implements these as byte loops. Everything goes through
 * them (the game, HSD, the GX layer, pbkit, the mixer), and on hardware they
 * took about 40% of a VS match's frame. These move and compare 32 bits at a
 * time; objects in the link come before libpdclib, so these definitions win.
 * Compiled with -fno-builtin (CMakeLists.txt) so clang can't turn the loops
 * back into calls to themselves. */
#include <stddef.h>
#include <stdint.h>

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    size_t head, words;
    if (n >= 16) {
        head = (size_t)(-(uintptr_t)dst & 3);   /* align the destination */
        n -= head;
        words = n >> 2;
        n &= 3;
        __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(head) : : "memory");
        __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
    }
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d <= s || d >= s + n) return memcpy(dst, src, n);
    /* overlapping, destination above the source: copy backwards */
    d += n;
    s += n;
    while (n >= 4 && ((uintptr_t)d & 3)) {
        *--d = *--s;
        n--;
    }
    while (n >= 4) {
        uint32_t w;
        d -= 4;
        s -= 4;
        __builtin_memcpy(&w, s, 4);
        __builtin_memcpy(d, &w, 4);
        n -= 4;
    }
    while (n--) *--d = *--s;
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    uint32_t v = (uint8_t)c * 0x01010101u;
    if (n >= 16) {
        size_t head = (size_t)(-(uintptr_t)dst & 3), words;
        n -= head;
        words = n >> 2;
        n &= 3;
        __asm__ volatile("rep stosb" : "+D"(dst), "+c"(head) : "a"(v) : "memory");
        __asm__ volatile("rep stosl" : "+D"(dst), "+c"(words) : "a"(v) : "memory");
    }
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(v) : "memory");
    return ret;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    while (n >= 4) {
        uint32_t x, y;
        __builtin_memcpy(&x, p, 4);
        __builtin_memcpy(&y, q, 4);
        if (x != y) break;   /* the byte loop below finds which byte differs */
        p += 4;
        q += 4;
        n -= 4;
    }
    while (n--) {
        if (*p != *q) return *p - *q;
        p++;
        q++;
    }
    return 0;
}
