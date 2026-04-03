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

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define __OF_JT ((const struct of_libc_table *)OF_LIBC_ADDR)

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

/* Minimal stubs */
static inline int   getpid(void)        { return 1; }
static inline int   isatty(int fd)      { (void)fd; return 0; }
static inline int   access(const char *path, int mode) { (void)path; (void)mode; return -1; }

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_UNISTD_H */
