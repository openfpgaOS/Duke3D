/* stdlib.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_STDLIB_H
#define _OF_STDLIB_H

#ifdef OF_PC
#include_next <stdlib.h>
#else

#include "of_libc.h"

static inline void *malloc(size_t s)                { return __of_libc->malloc(s); }
static inline void  free(void *p)                   { __of_libc->free(p); }
static inline void *realloc(void *p, size_t s)      { return __of_libc->realloc(p, s); }
static inline void *calloc(size_t n, size_t s)      { return __of_libc->calloc(n, s); }
static inline int   abs(int x)                      { return __of_libc->abs(x); }
static inline long  labs(long x)                    { return __of_libc->labs(x); }
static inline int   atoi(const char *s)             { return __of_libc->atoi(s); }
static inline long  strtol(const char *s, char **e, int b)          { return __of_libc->strtol(s, e, b); }
static inline unsigned long strtoul(const char *s, char **e, int b) { return __of_libc->strtoul(s, e, b); }

static inline void qsort(void *base, size_t n, size_t sz,
                          int(*cmp)(const void *, const void *)) {
    __of_libc->qsort(base, n, sz, cmp);
}

static inline void *bsearch(const void *key, const void *base,
                             size_t n, size_t sz,
                             int (*cmp)(const void *, const void *)) {
    return __of_libc->bsearch(key, base, n, sz, cmp);
}

static inline int   rand(void)          { return __of_libc->rand(); }
static inline void  srand(unsigned s)   { __of_libc->srand(s); }

static inline double atof(const char *s)                                     { return __of_libc->atof(s); }
static inline double strtod(const char *s, char **e)                         { return __of_libc->strtod(s, e); }
static inline long long strtoll(const char *s, char **e, int b)              { return __of_libc->strtoll(s, e, b); }
static inline unsigned long long strtoull(const char *s, char **e, int b)    { return __of_libc->strtoull(s, e, b); }

/* exit/abort: switch back to terminal display, then halt */
static inline void exit(int status) {
    (void)status;
    /* REG32 at 0x40000008 = display mode, 0 = terminal */
    *(volatile unsigned int *)0x40000008 = 0;
    while(1) {}
}
static inline void abort(void) { exit(1); }

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define NULL ((void *)0)
#define RAND_MAX 2147483647

#endif /* OF_PC */
#endif /* _OF_STDLIB_H */
