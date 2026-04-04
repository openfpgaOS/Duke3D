/* unistd.h -- openfpgaOS POSIX I/O
 *
 * open/close/read/write/lseek route through the OS jump table (musl).
 * musl's lseek correctly uses _llseek on riscv32.
 */
#ifndef _OF_UNISTD_H
#define _OF_UNISTD_H

#ifdef OF_PC
#include_next <unistd.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "of_libc.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifndef __OF_JT
#define __OF_JT ((const struct of_libc_table *)OF_LIBC_ADDR)
#endif

static inline int open(const char *path, int flags, ...) {
    return __OF_JT->open(path, flags);
}

static inline int close(int fd) {
    return __OF_JT->close(fd);
}

static inline int read(int fd, void *buf, unsigned int count) {
    return __OF_JT->read(fd, buf, count);
}

static inline int write(int fd, const void *buf, unsigned int count) {
    return __OF_JT->write(fd, buf, count);
}

static inline long long lseek(int fd, long long offset, int whence) {
    return __OF_JT->lseek(fd, offset, whence);
}

/* usleep — delay in microseconds (via syscall) */
static inline int usleep(unsigned int us) {
    register long a7 __asm__("a7") = 115; /* SYS_clock_nanosleep */
    register long a0 __asm__("a0") = 0;   /* CLOCK_REALTIME */
    register long a1 __asm__("a1") = 0;   /* flags */
    struct { long tv_sec; long tv_nsec; } ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    register long a2 __asm__("a2") = (long)&ts;
    register long a3 __asm__("a3") = 0;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return 0;
}

/* Minimal stubs */
static inline int   getpid(void)        { return 1; }
static inline int   isatty(int fd)      { (void)fd; return 0; }
static inline int   access(const char *path, int mode) { (void)path; (void)mode; return -1; }

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_UNISTD_H */
