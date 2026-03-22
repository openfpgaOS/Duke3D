/* stdio.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_STDIO_H
#define _OF_STDIO_H

#ifdef OF_PC
#include_next <stdio.h>
#else

#include "of_libc.h"

typedef void FILE;
#define NULL   ((void *)0)
#define EOF    (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define stdout (__of_libc->stdout_ptr)
#define stderr (__of_libc->stderr_ptr)

/* Inline functions BEFORE macros (so struct member access works) */
static inline int puts(const char *s) {
    __of_libc->printf("%s\n", s);
    return 0;
}
static inline int putchar(int c) {
    char buf[2] = {(char)c, 0};
    __of_libc->printf("%s", buf);
    return c;
}

/* Variadic and file I/O as macros (call through jump table) */
#define printf    __of_libc->printf
#define fprintf   __of_libc->fprintf
#define sprintf   __of_libc->sprintf
#define snprintf  __of_libc->snprintf
#define vsnprintf __of_libc->vsnprintf
#define vsprintf  __of_libc->vsprintf
#define sscanf    __of_libc->sscanf
#define fopen     __of_libc->fopen
#define fclose    __of_libc->fclose
#define fread     __of_libc->fread
#define fwrite    __of_libc->fwrite
#define fseek     __of_libc->fseek
#define ftell     __of_libc->ftell
#define fgets     __of_libc->fgets
#define fputs     __of_libc->fputs

#endif /* OF_PC */
#endif /* _OF_STDIO_H */
