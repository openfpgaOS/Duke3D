/* errno.h -- openfpgaOS minimal errno support */
#ifndef _OF_ERRNO_H
#define _OF_ERRNO_H

#ifdef OF_PC
#include_next <errno.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

static int __of_errno;
#define errno __of_errno

#define ENOENT  2
#define EIO     5
#define EBADF   9
#define ENOMEM  12
#define EACCES  13
#define EINVAL  22
#define EEXIST  17
#define ENOSYS  38
#define ERANGE  34

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_ERRNO_H */
