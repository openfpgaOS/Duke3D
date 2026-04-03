/* stdlib.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_STDLIB_H
#define _OF_STDLIB_H

#ifdef OF_PC
#include_next <stdlib.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include "of_libc.h"

/* Memory allocation — dlmalloc in kernel, called via syscall */
#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline void *malloc(size_t s) {
    return (void *)__of_syscall1(OF_SYS_MALLOC, (long)s);
}
static inline void free(void *p) {
    __of_syscall1(OF_SYS_FREE, (long)p);
}
static inline void *realloc(void *p, size_t s) {
    return (void *)__of_syscall2(OF_SYS_REALLOC, (long)p, (long)s);
}
static inline void *calloc(size_t n, size_t s) {
    return (void *)__of_syscall2(OF_SYS_CALLOC, (long)n, (long)s);
}
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

/* Extended functions (jump table count >= 83) */
static inline double atof(const char *s)                              { return __of_libc->atof(s); }
static inline double strtod(const char *s, char **e)                 { return __of_libc->strtod(s, e); }
static inline long long strtoll(const char *s, char **e, int b)     { return __of_libc->strtoll(s, e, b); }
static inline unsigned long long strtoull(const char *s, char **e, int b) { return __of_libc->strtoull(s, e, b); }

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#ifndef NULL
#define NULL ((void *)0)
#endif
#define RAND_MAX 2147483647

/* exit/abort use SYS_exit (93) directly to avoid conflicts with of.h's
 * static inline of_exit(). */
static inline void exit(int status) {
    (void)status;
    register long a7 __asm__("a7") = 93;
    register long a0 __asm__("a0");
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    (void)a0;
    __builtin_unreachable();
}

/* No environment on Pocket */
static inline char *getenv(const char *name) {
    (void)name;
    return (char *)0;
}

static inline int atexit(void (*func)(void)) {
    (void)func;
    return 0;
}

static inline void abort(void) {
    register long a7 __asm__("a7") = 93;
    register long a0 __asm__("a0");
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    (void)a0;
    __builtin_unreachable();
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_STDLIB_H */
