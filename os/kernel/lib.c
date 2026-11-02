#include "lib.h"

/*
 * The byte routines are the kernel's hottest leaf: COW faults, swap traffic
 * and the FAT bounce buffer all move 4 KiB at a time, and -mno-sse rules
 * out vector code. The wide bodies below use the integer string ops
 * (rep movsq/stosq) after aligning dst by hand; the byte tail keeps the
 * exact leftover semantics. rep form rather than word loops so the
 * compiler can never "optimize" a copy loop back into a memcpy call
 * (loop-idiom recognition would recurse into the very function it sits in).
 */

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n && ((uintptr_t)d & 7)) {           /* 8-byte align dst */
        *d++ = (uint8_t)c;
        n--;
    }
    size_t q = n >> 3;
    if (q) {
        uint64_t w = 0x0101010101010101ull * (uint8_t)c;
        __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(w) : "memory");
        n &= 7;
    }
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n && ((uintptr_t)d & 7)) {           /* align dst; src may stay unaligned */
        *d++ = *s++;
        n--;
    }
    size_t q = n >> 3;
    if (q) {
        __asm__ volatile("rep movsq"
                         : "+D"(d), "+S"(s), "+c"(q)
                         :
                         : "memory");
        n &= 7;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        size_t q = n >> 3;
        if (q) {
            __asm__ volatile("rep movsq"
                             : "+D"(d), "+S"(s), "+c"(q)
                             :
                             : "memory");
            n &= 7;
        }
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : NULL;
}

const char *strstr(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return NULL;
}
