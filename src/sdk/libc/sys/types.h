/* sys/types.h -- openfpgaOS type definitions */
#ifndef _OF_SYS_TYPES_H
#define _OF_SYS_TYPES_H

#ifdef OF_PC
#include_next <sys/types.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef int32_t  ssize_t;
typedef uint32_t off_t;
typedef uint32_t mode_t;
typedef int32_t  pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_SYS_TYPES_H */
