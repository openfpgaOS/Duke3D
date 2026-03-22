/*
 * of_posix.c -- POSIX/libc runtime for openfpgaOS applications
 *
 * Provides linkable symbols for POSIX I/O, standard C library functions,
 * and OS convenience wrappers. Game ports include this file in their build
 * instead of carrying their own posix_shim.c.
 *
 * POSIX I/O routes through Linux-compatible syscalls (ecall).
 * Libc functions forward to the OS jump table at OF_LIBC_ADDR.
 * OS utilities use openfpgaOS HAL syscalls.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "of_libc.h"
#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* ======================================================================
 * POSIX I/O -- Linux-compatible syscalls via ecall
 * ====================================================================== */

#define __NR_openat  56
#define __NR_close   57
#define __NR_lseek   62
#define __NR_read    63
#define __NR_write   64
#define AT_FDCWD     -100

static inline long __linux_syscall3(long n, long a, long b, long c) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

static inline long __linux_syscall4(long n, long a, long b, long c, long d) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3) : "memory");
    return a0;
}

int open(const char *path, int flags, ...) {
    int fd = (int)__linux_syscall4(__NR_openat, AT_FDCWD, (long)path, flags, 0);
    return fd < 0 ? -1 : fd;
}

int close(int fd) {
    return (int)__linux_syscall3(__NR_close, fd, 0, 0);
}

int read(int fd, void *buf, unsigned int count) {
    int n = (int)__linux_syscall3(__NR_read, fd, (long)buf, count);
    return n < 0 ? -1 : n;
}

int write(int fd, const void *buf, unsigned int count) {
    return (int)__linux_syscall3(__NR_write, fd, (long)buf, count);
}

long lseek(int fd, long offset, int whence) {
    long r = __linux_syscall3(__NR_lseek, fd, offset, whence);
    return r < 0 ? -1 : r;
}

/* ======================================================================
 * Libc linkable symbols -- forwarding to jump table
 * ====================================================================== */

#define JT ((const struct of_libc_table *)OF_LIBC_ADDR)

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

int getchar(void) {
    return -1;  /* no stdin on Pocket */
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

int unlink(const char *path) {
    (void)path;
    return -1;
}

int mkdir(const char *path, int mode) {
    (void)path;
    (void)mode;
    return -1;
}

void *alloca(unsigned int size) {
    return __builtin_alloca(size);
}

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return a > b ? a : b; }
int abs(int x) { return x < 0 ? -x : x; }

/* ======================================================================
 * OS convenience -- linkable symbols for SDK inline functions
 * ====================================================================== */

void of_print(const char *s) {
    while (*s) __of_syscall1(OF_SYS_TERM_PUTCHAR, *s++);
}

unsigned int of_time_ms(void) {
    return (unsigned int)__of_syscall0(OF_SYS_TIMER_GET_MS);
}
