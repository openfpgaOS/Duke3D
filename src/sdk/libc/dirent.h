/* dirent.h -- openfpgaOS directory operations via POSIX syscalls */
#ifndef _OF_DIRENT_H
#define _OF_DIRENT_H

#ifdef OF_PC
#include_next <dirent.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

struct dirent {
    unsigned long  d_ino;
    unsigned short d_namlen;
    char           d_name[256];
};

typedef struct {
    int     __fd;
    char    __buf[512];     /* getdents64 buffer */
    int     __buf_pos;      /* current position in buffer */
    int     __buf_len;      /* bytes returned by last getdents64 */
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_DIRENT_H */
