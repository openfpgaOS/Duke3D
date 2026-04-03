/* math.h -- openfpgaOS libc jump table wrapper */
#ifndef _OF_MATH_H
#define _OF_MATH_H

#ifdef OF_PC
#include_next <math.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include "of_libc.h"

static inline float sinf(float x)              { return __of_libc->sinf(x); }
static inline float cosf(float x)              { return __of_libc->cosf(x); }
static inline float tanf(float x)              { return __of_libc->tanf(x); }
static inline float asinf(float x)             { return __of_libc->asinf(x); }
static inline float acosf(float x)             { return __of_libc->acosf(x); }
static inline float atan2f(float y, float x)   { return __of_libc->atan2f(y, x); }
static inline float sqrtf(float x)             { return __of_libc->sqrtf(x); }
static inline float fmodf(float x, float y)    { return __of_libc->fmodf(x, y); }
static inline float floorf(float x)            { return __of_libc->floorf(x); }
static inline float ceilf(float x)             { return __of_libc->ceilf(x); }
static inline float roundf(float x)            { return __of_libc->roundf(x); }
static inline float fabsf(float x)             { return __of_libc->fabsf(x); }
static inline float fmaxf(float a, float b)    { return __of_libc->fmaxf(a, b); }
static inline float fminf(float a, float b)    { return __of_libc->fminf(a, b); }
static inline float powf(float b, float e)     { return __of_libc->powf(b, e); }
static inline float logf(float x)              { return __of_libc->logf(x); }
static inline float log2f(float x)             { return __of_libc->log2f(x); }
static inline float expf(float x)              { return __of_libc->expf(x); }
static inline double sin(double x)             { return __of_libc->sin(x); }
static inline double cos(double x)             { return __of_libc->cos(x); }
static inline double sqrt(double x)            { return __of_libc->sqrt(x); }

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_MATH_H */
