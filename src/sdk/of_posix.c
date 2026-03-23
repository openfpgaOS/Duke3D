/*
 * of_posix.c -- Linkable libc + POSIX symbols for openfpgaOS game ports
 *
 * Game engines compiled with -nostdlib need real linkable symbols for
 * malloc, printf, open, read, etc. This file provides them by forwarding
 * to the OS jump table (musl libc) at OF_LIBC_ADDR.
 *
 * SDK demo apps don't need this — they use static inline wrappers from
 * the SDK libc headers. Game ports add this file to their build.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "of_libc.h"

#define JT ((const struct of_libc_table *)OF_LIBC_ADDR)

/* ======================================================================
 * POSIX I/O -- routed through musl via jump table
 * musl handles _llseek on riscv32 correctly.
 * ====================================================================== */

int open(const char *path, int flags, ...) { return JT->open(path, flags); }
int close(int fd)                          { return JT->close(fd); }
int read(int fd, void *buf, unsigned int count)        { return JT->read(fd, buf, count); }
int write(int fd, const void *buf, unsigned int count)  { return JT->write(fd, buf, count); }
long long lseek(int fd, long long offset, int whence) { return JT->lseek(fd, offset, whence); }

/* ======================================================================
 * Libc linkable symbols -- forwarding to jump table
 * ====================================================================== */

/* -- memory -- */
void *memset(void *s, int c, unsigned int n)           { return JT->memset(s, c, n); }
void *memcpy(void *d, const void *s, unsigned int n)   { return JT->memcpy(d, s, n); }
void *memmove(void *d, const void *s, unsigned int n)  { return JT->memmove(d, s, n); }
int   memcmp(const void *a, const void *b, unsigned int n) { return JT->memcmp(a, b, n); }

/* -- string -- */
unsigned int strlen(const char *s)                      { return JT->strlen(s); }
int   strcmp(const char *a, const char *b)              { return JT->strcmp(a, b); }
int   strncmp(const char *a, const char *b, unsigned int n) { return JT->strncmp(a, b, n); }
char *strcpy(char *d, const char *s)                    { return JT->strcpy(d, s); }
char *strncpy(char *d, const char *s, unsigned int n)   { return JT->strncpy(d, s, n); }
char *strcat(char *d, const char *s)                    { return JT->strcat(d, s); }
char *strchr(const char *s, int c)                      { return JT->strchr(s, c); }
char *strrchr(const char *s, int c)                     { return JT->strrchr(s, c); }
char *strstr(const char *h, const char *n)              { return JT->strstr(h, n); }

/* -- ctype -- */
int   toupper(int c) { return JT->toupper(c); }
int   tolower(int c) { return JT->tolower(c); }
int   isalpha(int c) { return JT->isalpha(c); }
int   isdigit(int c) { return JT->isdigit(c); }
int   isspace(int c) { return JT->isspace(c); }

/* -- memory allocation -- */
void *malloc(unsigned int s)                            { return JT->malloc(s); }
void  free(void *p)                                     { JT->free(p); }
void *realloc(void *p, unsigned int s)                  { return JT->realloc(p, s); }
void *calloc(unsigned int n, unsigned int s)            { return JT->calloc(n, s); }

/* -- stdlib -- */
int   atoi(const char *s)                               { return JT->atoi(s); }
long  atol(const char *s)                               { return JT->strtol(s, 0, 10); }
int   rand(void)                                        { return JT->rand(); }
void  srand(unsigned int s)                             { JT->srand(s); }
void  qsort(void *b, unsigned int n, unsigned int sz,
            int (*c)(const void *, const void *))       { JT->qsort(b, n, sz, c); }

/* ======================================================================
 * Printf family -- variadic, routed through vsnprintf from jump table
 * ====================================================================== */

static char __printf_buf[1024];

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = JT->vsnprintf(__printf_buf, sizeof(__printf_buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(1, __printf_buf, n);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    int n = JT->vsnprintf(__printf_buf, sizeof(__printf_buf), fmt, ap);
    if (n > 0) write(1, __printf_buf, n);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = JT->vsnprintf(buf, 1024, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, unsigned int sz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = JT->vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return n;
}

/* ======================================================================
 * Utility stubs
 * ====================================================================== */

int getchar(void)              { return -1; }
char *strerror(int n)          { (void)n; return "error"; }
int unlink(const char *p)      { (void)p; return -1; }
int mkdir(const char *p, int m){ (void)p; (void)m; return -1; }
void *alloca(unsigned int sz)  { return __builtin_alloca(sz); }
int min(int a, int b)          { return a < b ? a : b; }
int max(int a, int b)          { return a > b ? a : b; }
int abs(int x)                 { return x < 0 ? -x : x; }

/* ======================================================================
 * openfpgaOS convenience — linkable symbols for SDK inline functions
 * ====================================================================== */

#include "of_syscall.h"
#include "of_syscall_numbers.h"

void of_print(const char *s) {
    while (*s) __of_syscall1(OF_SYS_TERM_PUTCHAR, *s++);
}

unsigned int of_time_us(void) {
    return (unsigned int)__of_syscall0(OF_SYS_TIMER_GET_US);
}

unsigned int of_time_ms(void) {
    return (unsigned int)__of_syscall0(OF_SYS_TIMER_GET_MS);
}
