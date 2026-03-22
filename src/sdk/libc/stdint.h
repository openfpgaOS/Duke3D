/* stdint.h -- openfpgaOS integer types
 *
 * On rv32, GCC defines int32_t as 'long int' which conflicts with code
 * that uses 'int' and 'int32_t' interchangeably (very common in game code).
 * We define int32_t as 'int' to avoid conflicting-type errors.
 */
#ifndef _OF_STDINT_H
#define _OF_STDINT_H

#ifdef OF_PC
#include_next <stdint.h>
#else

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

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535
#define INT32_MIN   (-2147483647-1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295U
#define INT64_MIN   (-9223372036854775807LL-1)
#define INT64_MAX   9223372036854775807LL
#define UINT64_MAX  18446744073709551615ULL

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#define SIZE_MAX    UINT32_MAX
#define PTRDIFF_MIN INT32_MIN
#define PTRDIFF_MAX INT32_MAX

#endif /* OF_PC */
#endif /* _OF_STDINT_H */
