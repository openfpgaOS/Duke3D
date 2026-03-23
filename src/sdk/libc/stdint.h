/* stdint.h -- openfpgaOS rv32 type overrides */
#ifndef _OF_STDINT_H
#define _OF_STDINT_H

#ifdef OF_PC
#include_next <stdint.h>
#else

/* On rv32, int and long are both 32-bit.
 * Define int32_t as int (not long int) for compatibility with game code
 * that uses int and int32_t interchangeably in printf format strings. */

typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef short               int16_t;
typedef unsigned short      uint16_t;
typedef int                 int32_t;
typedef unsigned int        uint32_t;
typedef long long           int64_t;
typedef unsigned long long  uint64_t;

typedef int                 intptr_t;
typedef unsigned int        uintptr_t;

typedef long long           intmax_t;
typedef unsigned long long  uintmax_t;

typedef int32_t             int_least32_t;
typedef uint32_t            uint_least32_t;
typedef int16_t             int_least16_t;
typedef uint16_t            uint_least16_t;
typedef int8_t              int_least8_t;
typedef uint8_t             uint_least8_t;
typedef int64_t             int_least64_t;
typedef uint64_t            uint_least64_t;

typedef int32_t             int_fast32_t;
typedef uint32_t            uint_fast32_t;
typedef int16_t             int_fast16_t;
typedef uint16_t            uint_fast16_t;
typedef int8_t              int_fast8_t;
typedef uint8_t             uint_fast8_t;
typedef int64_t             int_fast64_t;
typedef uint64_t            uint_fast64_t;

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT64_MAX  18446744073709551615ULL

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#define SIZE_MAX    UINT32_MAX
#define PTRDIFF_MIN INT32_MIN
#define PTRDIFF_MAX INT32_MAX

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#endif /* OF_PC */
#endif /* _OF_STDINT_H */
