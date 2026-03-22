/* fcntl.h -- openfpgaOS file control options */
#ifndef _OF_FCNTL_H
#define _OF_FCNTL_H

#ifdef OF_PC
#include_next <fcntl.h>
#else

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_BINARY    0       /* no-op on this platform */

#endif /* OF_PC */
#endif /* _OF_FCNTL_H */
