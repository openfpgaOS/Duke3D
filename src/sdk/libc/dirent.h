/* dirent.h -- openfpgaOS stub for directory operations */
#ifndef _OF_DIRENT_H
#define _OF_DIRENT_H

#ifdef OF_PC
#include_next <dirent.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct dirent {
    unsigned long  d_ino;
    unsigned short d_namlen;
    char           d_name[256];
};

typedef struct { int __fd; } DIR;

static inline DIR *opendir(const char *name) {
    (void)name;
    return (DIR *)NULL;
}

static inline struct dirent *readdir(DIR *dirp) {
    (void)dirp;
    return (struct dirent *)NULL;
}

static inline int closedir(DIR *dirp) {
    (void)dirp;
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_DIRENT_H */
