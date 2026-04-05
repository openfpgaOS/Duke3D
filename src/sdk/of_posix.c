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

/* Matches struct stat in libc_include/sys/stat.h */
struct _of_stat { uint32_t st_size; uint32_t st_mode; uint32_t st_mtime; };

/* ======================================================================
 * POSIX I/O -- routed through musl via jump table
 * musl handles _llseek on riscv32 correctly.
 * ====================================================================== */

int open(const char *path, int flags, ...) { return JT->open(path, flags); }
int close(int fd)                          { return JT->close(fd); }
int read(int fd, void *buf, unsigned int count)        { return JT->read(fd, buf, count); }
int write(int fd, const void *buf, unsigned int count)  { return JT->write(fd, buf, count); }
long lseek(int fd, long offset, int whence) { return JT->lseek(fd, offset, whence); }

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

/* access: try to open the file; if it succeeds, it exists */
int access(const char *path, int mode) {
    (void)mode;
    int fd = JT->open(path, 0);
    if (fd < 0) return -1;
    JT->close(fd);
    return 0;
}

/* exit/abort handled in stdlib.h via ecall(93) → kernel switches to terminal FB */
int mkdir(const char *p, int m){ (void)p; (void)m; return 0; }

/* stat/fstat: use lseek to determine file size.
 * No ecall — uses the JT functions which are known working. */
int stat(const char *path, struct _of_stat *buf) {
    int fd = JT->open(path, 0);
    if (fd < 0) return -1;
    /* Save pos, seek end, restore — all through musl's lseek */
    long long sz = JT->lseek(fd, 0, 2);  /* SEEK_END */
    JT->close(fd);
    if (buf) {
        buf->st_size = (sz > 0) ? (unsigned int)sz : 0;
        buf->st_mode = 0100644;
        buf->st_mtime = 0;
    }
    return 0;
}

int fstat(int fd, struct _of_stat *buf) {
    if (fd < 0) return -1;
    /* For fstat, save current position, seek to end, restore */
    long long pos = JT->lseek(fd, 0, 1);  /* SEEK_CUR */
    long long sz  = JT->lseek(fd, 0, 2);  /* SEEK_END */
    if (pos >= 0)
        JT->lseek(fd, pos, 0);            /* SEEK_SET restore */
    if (buf) {
        buf->st_size = (sz > 0) ? (unsigned int)sz : 0;
        buf->st_mode = 0100644;
        buf->st_mtime = 0;
    }
    return 0;
}
void *alloca(unsigned int sz)  { return __builtin_alloca(sz); }
int min(int a, int b)          { return a < b ? a : b; }
int max(int a, int b)          { return a > b ? a : b; }
int abs(int x)                 { return x < 0 ? -x : x; }

/* ======================================================================
 * POSIX directory operations — opendir/readdir/closedir via syscalls
 * ====================================================================== */

#include <dirent.h>

/* riscv32 Linux syscall numbers */
#define __NR_openat     56
#define __NR_close      57
#define __NR_getdents64 61
#define O_RDONLY        0
#define O_DIRECTORY     0200000

static long __syscall3(long n, long a, long b, long c) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static long __syscall4(long n, long a, long b, long c, long d) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static DIR __dir_storage;  /* single static DIR — no malloc needed for one opendir */

DIR *opendir(const char *name) {
    long fd = __syscall4(__NR_openat, -100 /* AT_FDCWD */, (long)name,
                         O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return NULL;
    __dir_storage.__fd = (int)fd;
    __dir_storage.__buf_pos = 0;
    __dir_storage.__buf_len = 0;
    return &__dir_storage;
}

/* Kernel getdents64 entry layout (matches riscv32 linux_dirent64) */
struct __kernel_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

struct dirent *readdir(DIR *dirp) {
    static struct dirent result;

    if (dirp->__buf_pos >= dirp->__buf_len) {
        /* Refill buffer */
        long n = __syscall3(__NR_getdents64, dirp->__fd,
                           (long)dirp->__buf, sizeof(dirp->__buf));
        if (n <= 0) return NULL;
        dirp->__buf_len = (int)n;
        dirp->__buf_pos = 0;
    }

    struct __kernel_dirent64 *kd =
        (struct __kernel_dirent64 *)(dirp->__buf + dirp->__buf_pos);
    dirp->__buf_pos += kd->d_reclen;

    result.d_ino = (unsigned long)kd->d_ino;
    /* Copy name */
    int i;
    for (i = 0; i < 255 && kd->d_name[i]; i++)
        result.d_name[i] = kd->d_name[i];
    result.d_name[i] = '\0';
    result.d_namlen = (unsigned short)i;

    return &result;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    long rc = __syscall3(__NR_close, dirp->__fd, 0, 0);
    dirp->__fd = -1;
    return (int)rc;
}

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

/* ======================================================================
 * POSIX time functions — ecall to kernel clock_gettime/nanosleep
 * ====================================================================== */

struct timespec { unsigned int tv_sec; long tv_nsec; };

int clock_gettime(int clk_id, struct timespec *tp) {
    unsigned int us = (unsigned int)__of_syscall0(OF_SYS_TIMER_GET_US);
    if (tp) {
        tp->tv_sec  = us / 1000000;
        tp->tv_nsec = (us % 1000000) * 1000;
    }
    (void)clk_id;
    return 0;
}

int clock_nanosleep(int clk_id, int flags, const struct timespec *req,
                    struct timespec *rem) {
    (void)clk_id; (void)flags; (void)rem;
    if (!req) return -1;
    unsigned int us = (unsigned int)(req->tv_sec * 1000000 + req->tv_nsec / 1000);
    __of_syscall1(OF_SYS_TIMER_DELAY_US, us);
    return 0;
}

unsigned int usleep(unsigned int us) {
    __of_syscall1(OF_SYS_TIMER_DELAY_US, us);
    return 0;
}

unsigned int sleep(unsigned int sec) {
    __of_syscall1(OF_SYS_TIMER_DELAY_US, sec * 1000000);
    return 0;
}
