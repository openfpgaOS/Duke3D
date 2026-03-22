/* ctype.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_CTYPE_H
#define _OF_CTYPE_H

#ifdef OF_PC
#include_next <ctype.h>
#else

#include "of_libc.h"

static inline int toupper(int c) { return __of_libc->toupper(c); }
static inline int tolower(int c) { return __of_libc->tolower(c); }
static inline int isalpha(int c) { return __of_libc->isalpha(c); }
static inline int isdigit(int c) { return __of_libc->isdigit(c); }
static inline int isalnum(int c) { return __of_libc->isalnum(c); }
static inline int isspace(int c) { return __of_libc->isspace(c); }
static inline int isupper(int c) { return __of_libc->isupper(c); }
static inline int islower(int c) { return __of_libc->islower(c); }
static inline int isprint(int c) { return __of_libc->isprint(c); }

#endif /* OF_PC */
#endif /* _OF_CTYPE_H */
