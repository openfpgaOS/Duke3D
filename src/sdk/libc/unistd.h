/* unistd.h -- openfpgaOS POSIX-like I/O stubs
 *
 * These wrap the openfpgaOS data slot API into POSIX-like file descriptors.
 * See of_posixio.h for implementation.
 */
#ifndef _OF_UNISTD_H
#define _OF_UNISTD_H

#ifdef OF_PC
#include_next <unistd.h>
#else

#include <stddef.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Forward declarations -- implemented in of_posixio.h */
int   open(const char *path, int flags, ...);
int   close(int fd);
int   read(int fd, void *buf, unsigned int count);
int   write(int fd, const void *buf, unsigned int count);
long  lseek(int fd, long offset, int whence);

/* Minimal stubs for functions Duke3D references */
static inline int   getpid(void)        { return 1; }
static inline int   isatty(int fd)      { (void)fd; return 0; }
static inline int   access(const char *path, int mode) { (void)path; (void)mode; return -1; }

#endif /* OF_PC */
#endif /* _OF_UNISTD_H */
