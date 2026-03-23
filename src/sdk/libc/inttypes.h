/* inttypes.h -- openfpgaOS integer format macros */
#ifndef _OF_INTTYPES_H
#define _OF_INTTYPES_H

#ifdef OF_PC
#include_next <inttypes.h>
#else

#include <stdint.h>

/* 32-bit platform: int32_t = int, int64_t = long long */
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"

#define SCNd32 "d"
#define SCNu32 "u"
#define SCNx32 "x"

#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"

#endif /* OF_PC */
#endif /* _OF_INTTYPES_H */
