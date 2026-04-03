/* sys/stat.h -- openfpgaOS minimal stat support */
#ifndef _OF_SYS_STAT_H
#define _OF_SYS_STAT_H

#ifdef OF_PC
#include_next <sys/stat.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef uint32_t mode_t;
typedef uint32_t off_t;

struct stat {
    uint32_t st_size;
    uint32_t st_mode;
    uint32_t st_mtime;
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

/* Legacy aliases */
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR

/* stat/fstat -- not supported, return -1 */
static inline int stat(const char *path, struct stat *buf) {
    (void)path; (void)buf;
    return -1;
}

static inline int fstat(int fd, struct stat *buf) {
    (void)fd; (void)buf;
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_SYS_STAT_H */
