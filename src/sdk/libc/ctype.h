/* ctype.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_CTYPE_H
#define _OF_CTYPE_H

#ifdef OF_PC
#include_next <ctype.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include "of_libc.h"

/* Jump table functions (count >= 83) */
static inline int toupper(int c) { return __of_libc->toupper(c); }
static inline int tolower(int c) { return __of_libc->tolower(c); }
static inline int isalpha(int c) { return __of_libc->isalpha(c); }
static inline int isdigit(int c) { return __of_libc->isdigit(c); }
static inline int isalnum(int c) { return __of_libc->isalnum(c); }
static inline int isspace(int c) { return __of_libc->isspace(c); }
static inline int isupper(int c) { return __of_libc->isupper(c); }
static inline int islower(int c) { return __of_libc->islower(c); }
static inline int isprint(int c) { return __of_libc->isprint(c); }

/* These are simple enough to compute locally */
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
static inline int ispunct(int c)  { return isprint(c) && !isalnum(c) && !isspace(c); }
static inline int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7F; }
static inline int isgraph(int c)  { return isprint(c) && c != ' '; }

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_CTYPE_H */
