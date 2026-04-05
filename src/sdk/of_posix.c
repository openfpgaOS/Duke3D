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

/* stat/fstat: route through musl → ecall → kernel SYS_statx.
 * The kernel writes a 256-byte statx struct, so we use a stack buffer
 * and extract st_size from offset 40 (sx[10]). */
int fstat(int fd, struct _of_stat *buf) {
    uint8_t tmp[256] __attribute__((aligned(8)));
    int rc = JT->fstat(fd, tmp);
    if (rc == 0 && buf) {
        uint32_t *sx = (uint32_t *)tmp;
        buf->st_size = sx[10];    /* stx_size low 32 (offset 40) */
        buf->st_mode = 0100644;
        buf->st_mtime = 0;
    }
    return rc;
}

int stat(const char *path, struct _of_stat *buf) {
    int fd = JT->open(path, 0);
    if (fd < 0) return -1;
    int rc = fstat(fd, buf);
    JT->close(fd);
    return rc;
}
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

/* ======================================================================
 * Directory operations — opendir/readdir/closedir via Linux syscalls
 * ====================================================================== */

#include <dirent.h>

/* Linux RISC-V syscall numbers */
#define _SYS_openat      56
#define _SYS_close        57
#define _SYS_getdents64   61

/* O_RDONLY | O_DIRECTORY on riscv-linux */
#define _O_DIRECTORY 0200000

/* Linux getdents64 on-disk record layout */
struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

static DIR __dir_buf;  /* single static DIR (no malloc needed) */

DIR *opendir(const char *name) {
    /* SYS_openat(AT_FDCWD, path, O_RDONLY|O_DIRECTORY, 0) */
    int fd = (int)__of_syscall4(_SYS_openat, -100, (long)name, _O_DIRECTORY, 0);
    if (fd < 0) return 0;
    __dir_buf.__fd = fd;
    __dir_buf.__buf_pos = 0;
    __dir_buf.__buf_len = 0;
    return &__dir_buf;
}

static struct dirent __de_buf;

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return 0;

    /* Refill buffer if exhausted */
    if (dirp->__buf_pos >= dirp->__buf_len) {
        long n = __of_syscall3(_SYS_getdents64, dirp->__fd,
                               (long)dirp->__buf, sizeof(dirp->__buf));
        if (n <= 0) return 0;
        dirp->__buf_len = (int)n;
        dirp->__buf_pos = 0;
    }

    /* Parse linux_dirent64 from buffer */
    struct linux_dirent64 *lde =
        (struct linux_dirent64 *)(dirp->__buf + dirp->__buf_pos);
    dirp->__buf_pos += lde->d_reclen;

    /* Copy to our dirent */
    __de_buf.d_ino = (unsigned long)lde->d_ino;
    int i = 0;
    while (lde->d_name[i] && i < 255) {
        __de_buf.d_name[i] = lde->d_name[i];
        i++;
    }
    __de_buf.d_name[i] = '\0';
    __de_buf.d_namlen = (unsigned short)i;
    return &__de_buf;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    return (int)__of_syscall1(_SYS_close, dirp->__fd);
}
