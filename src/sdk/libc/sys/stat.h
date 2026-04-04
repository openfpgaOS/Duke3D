/* sys/stat.h -- openfpgaOS stat support */
#ifndef _OF_SYS_STAT_H
#define _OF_SYS_STAT_H

#ifdef OF_PC
#include_next <sys/stat.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "of_syscall.h"

typedef uint32_t mode_t;
typedef uint32_t off_t;

struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR

/* statx struct layout matching kernel's SYS_statx handler */
struct __of_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __pad;
    uint64_t stx_ino;
    uint64_t stx_size;
};

#define AT_FDCWD (-100)

static inline int stat(const char *path, struct stat *buf) {
    struct __of_statx stx;
    long ret = __of_syscall5(291 /* SYS_statx */, AT_FDCWD,
                             (long)path, 0, 0x7FF, (long)&stx);
    if (ret < 0) return -1;
    buf->st_size = (uint32_t)stx.stx_size;
    buf->st_mode = stx.stx_mode;
    return 0;
}

static inline int fstat(int fd, struct stat *buf) {
    struct __of_statx stx;
    long ret = __of_syscall5(291 /* SYS_statx */, fd,
                             (long)"", 0x1000 /* AT_EMPTY_PATH */,
                             0x7FF, (long)&stx);
    if (ret < 0) return -1;
    buf->st_size = (uint32_t)stx.stx_size;
    buf->st_mode = stx.stx_mode;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_SYS_STAT_H */
