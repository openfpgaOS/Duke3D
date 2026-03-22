/* dirent.h -- openfpgaOS stub (no directory operations) */
#ifndef _OF_DIRENT_H
#define _OF_DIRENT_H

#ifdef OF_PC
#include_next <dirent.h>
#else

struct dirent { char d_name[256]; int d_namlen; };
typedef struct { int __fd; } DIR;

static inline DIR *opendir(const char *name) { (void)name; return (DIR *)0; }
static inline struct dirent *readdir(DIR *d) { (void)d; return (struct dirent *)0; }
static inline int closedir(DIR *d) { (void)d; return 0; }

#endif /* OF_PC */
#endif /* _OF_DIRENT_H */
