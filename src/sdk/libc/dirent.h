/* dirent.h -- Directory listing for openfpgaOS */
#ifndef _OF_DIRENT_H
#define _OF_DIRENT_H

#ifdef OF_PC
#include_next <dirent.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/* Linux dirent64 (matches kernel's getdents64 output) */
struct dirent {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

#define DT_REG  8

typedef struct {
    int  fd;
    int  buf_pos;
    int  buf_end;
    char buf[2048];
} DIR;

static inline DIR *opendir(const char *name) {
    int fd = open(name, O_RDONLY | 0200000 /* O_DIRECTORY */);
    if (fd < 0) return (DIR *)NULL;

    /* Allocate DIR from heap */
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { close(fd); return (DIR *)NULL; }
    d->fd = fd;
    d->buf_pos = 0;
    d->buf_end = 0;
    return d;
}

/* getdents64 syscall (61 on riscv32) */
static inline long __getdents64(int fd, void *buf, unsigned count) {
    register long a7 __asm__("a7") = 61;
    register long a0 __asm__("a0") = fd;
    register long a1 __asm__("a1") = (long)buf;
    register long a2 __asm__("a2") = count;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline struct dirent *readdir(DIR *d) {
    if (!d) return (struct dirent *)NULL;
    if (d->buf_pos >= d->buf_end) {
        long n = __getdents64(d->fd, d->buf, sizeof(d->buf));
        if (n <= 0) return (struct dirent *)NULL;
        d->buf_end = (int)n;
        d->buf_pos = 0;
    }
    struct dirent *de = (struct dirent *)(d->buf + d->buf_pos);
    d->buf_pos += de->d_reclen;
    return de;
}

static inline int closedir(DIR *d) {
    if (!d) return -1;
    int rc = close(d->fd);
    free(d);
    return rc;
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_DIRENT_H */
