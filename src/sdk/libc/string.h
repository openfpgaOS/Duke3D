/* string.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_STRING_H
#define _OF_STRING_H

#ifdef OF_PC
#include_next <string.h>
#else

#include "of_libc.h"

/* Original 12 functions (always available) */
static inline void *memcpy(void *d, const void *s, size_t n)  { return __of_libc->memcpy(d, s, n); }
static inline void *memset(void *s, int c, size_t n)          { return __of_libc->memset(s, c, n); }
static inline void *memmove(void *d, const void *s, size_t n) { return __of_libc->memmove(d, s, n); }
static inline int   memcmp(const void *a, const void *b, size_t n) { return __of_libc->memcmp(a, b, n); }
static inline size_t strlen(const char *s)                     { return __of_libc->strlen(s); }
static inline int   strcmp(const char *a, const char *b)       { return __of_libc->strcmp(a, b); }
static inline int   strncmp(const char *a, const char *b, size_t n) { return __of_libc->strncmp(a, b, n); }
static inline char *strcpy(char *d, const char *s)             { return __of_libc->strcpy(d, s); }
static inline char *strncpy(char *d, const char *s, size_t n) { return __of_libc->strncpy(d, s, n); }
static inline char *strstr(const char *h, const char *n)       { return __of_libc->strstr(h, n); }
static inline char *strchr(const char *s, int c)               { return __of_libc->strchr(s, c); }
static inline char *strrchr(const char *s, int c)              { return __of_libc->strrchr(s, c); }

/* Extended functions (jump table count >= 83) */
static inline char *strcat(char *d, const char *s)             { return __of_libc->strcat(d, s); }
static inline char *strncat(char *d, const char *s, size_t n)  { return __of_libc->strncat(d, s, n); }
static inline char *strdup(const char *s)                      { return __of_libc->strdup(s); }
static inline void *memchr(const void *s, int c, size_t n)    { return __of_libc->memchr(s, c, n); }
static inline char *strtok(char *s, const char *d)             { return __of_libc->strtok(s, d); }
static inline size_t strspn(const char *s, const char *a)      { return __of_libc->strspn(s, a); }
static inline size_t strcspn(const char *s, const char *r)     { return __of_libc->strcspn(s, r); }

/* Case-insensitive compare (pure computation, no jump table needed) */
static inline int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int strncasecmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    if (n == (size_t)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* Aliases used by old DOS/Windows code */
#define stricmp  strcasecmp
#define strnicmp strncasecmp
#define _stricmp strcasecmp

#endif /* OF_PC */
#endif /* _OF_STRING_H */
