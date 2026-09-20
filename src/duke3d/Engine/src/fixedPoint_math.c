// converted from asm to c by Jonof

#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "fixedPoint_math.h"

/* Doom's fixed-point division strategy, specialized for BUILD's wall setup.
 * The common small numerator fits in one native divide.  Otherwise split
 * into whole/remainder and generate the twelve fractional bits.  Unsigned
 * magnitudes handle INT_MIN and retain the original low 32 quotient bits,
 * including overflow, with truncation toward zero for either sign.
 * denominator must be nonzero (the divscale wrapper checks it).
 * Kept out of line so prepwall's four call sites do not grow APP_BRAM. */
int32_t build_divscale12(int32_t numerator, int32_t denominator)
{
    uint32_t n = numerator < 0 ? 0u - (uint32_t)numerator : (uint32_t)numerator;
    uint32_t d = denominator < 0 ? 0u - (uint32_t)denominator : (uint32_t)denominator;
    uint32_t result;

    if (n <= (UINT32_MAX >> 12)) {
        result = (n << 12) / d;
    } else {
        uint32_t whole = n / d;
        uint32_t rem = n - whole * d;
        result = whole << 12;
        /* d <= 2^31 and rem < d, so doubling rem cannot overflow. */
        for (uint32_t bit = 1u << 11; bit; bit >>= 1) {
            rem <<= 1;
            if (rem >= d) {
                rem -= d;
                result |= bit;
            }
        }
    }
    if ((numerator < 0) != (denominator < 0))
        result = 0u - result;
    return (int32_t)result;
}

void clearbuf(void *d, int32_t c, int32_t a)
{
	union
    {
	    struct { uint8_t a,b,c,d; };
	    int32_t value;
    } src;

	src.value = a;

	uint8_t* dst = (uint8_t*) d;

	for (int32_t i = 0; i < c; ++i)
    {
	    dst[i*4 + 0] = src.a;
        dst[i*4 + 1] = src.b;
        dst[i*4 + 2] = src.c;
        dst[i*4 + 3] = src.d;
    }
}

void clearbufbyte(void *D, int32_t c, int32_t a)
{
	uint8_t  *p = (uint8_t *)D;
	int32_t m[4] = { 0xffl,0xff00l,0xff0000l,0xff000000l };
	int32_t n[4] = { 0,8,16,24 };
	int32_t z=0;
	while ((c--) > 0) {
		*(p++) = (uint8_t )((a & m[z])>>n[z]);
		z=(z+1)&3;
	}
}

void copybuf(void *s, void *d, int32_t c)
{
	memcpy(d, s, c * sizeof(int32_t));
}

void copybufbyte(void *S, void *D, int32_t c)
{
	uint8_t  *p = (uint8_t *)S, *q = (uint8_t *)D;
	while((c--) > 0) *(q++) = *(p++);
}

void copybufreverse(void *S, void *D, int32_t c)
{
	uint8_t  *p = (uint8_t *)S, *q = (uint8_t *)D;
	while((c--) > 0) *(q++) = *(p--);
}

void qinterpolatedown16(int32_t* bufptr, int32_t num, int32_t val, int32_t add)
{ // gee, I wonder who could have provided this...
    int32_t i, *lptr = bufptr;
    for(i=0;i<num;i++) { lptr[i] = (val>>16); val += add; }
}

void qinterpolatedown16short(int32_t* bufptr, int32_t num, int32_t val, int32_t add)
{ // ...maybe the same person who provided this too?
    int32_t i; short *sptr = (short *)bufptr;
    for(i=0;i<num;i++) { sptr[i] = (short)(val>>16); val += add; }
}
