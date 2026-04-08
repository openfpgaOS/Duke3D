// "Build Engine & Tools" Copyright (c) 1993-1997 Ken Silverman
// Ken Silverman's official web site: "http://www.advsys.net/ken"
// See the included license file "BUILDLIC.TXT" for license info.
// This file has been modified from Ken Silverman's original release

/* DDOI - This file is an attempt to reimplement a_nasm.asm in C */
/* FCS: However did that work: This is far from perfect but you have my eternal respect !!! */

#include "platform.h"
#include "build.h"
#include "draw.h"
#ifdef OPENFPGA
#include "of_fastram.h"
#else
#define OF_FASTTEXT
#define OF_FASTDATA
#endif

#if RENDER_LIMIT_PIXELS
int64_t pixelsAllowed = 10000000000;
#endif

uint8_t  *transluc = NULL;

static OF_FASTDATA int transrev = 0;

#define TRANSPARENT_COLOR 255

/* Shift-double macros: mask shift counts to avoid UB when c==0 or c>=32 */
#define shrd(a,b,c) (((b)<<((32-(c))&0x1f)) | ((a)>>((c)&0x1f)))
#define shld(a,b,c) (((b)>>((32-(c))&0x1f)) | ((a)<<((c)&0x1f)))

/* ---------------  WALLS RENDERING METHOD (USED TO BE HIGHLY OPTIMIZED ASSEMBLY) ----------------------------*/
extern int32_t asm1;
extern intptr_t asm2;
extern uint8_t *asm3;
extern int32_t asm4;

static OF_FASTDATA uint8_t machxbits_al;
static OF_FASTDATA uint8_t bitsSetup;
static OF_FASTDATA const uint8_t * textureSetup;
/* Setup-only: not in BRAM (cold path) */
void sethlinesizes(int32_t i1, int32_t _bits, const uint8_t * textureAddress)
{
    machxbits_al = i1;
    bitsSetup = _bits;
    textureSetup = textureAddress;
} 

//FCS:   Draw ceiling/floors
//Draw a line from destination in the framebuffer to framebuffer-numPixels
OF_FASTTEXT void hlineasm4(int32_t numPixels, int32_t shade, uint32_t i4, uint32_t i5, uint8_t * restrict dest){

    const int32_t shifter = ((256-machxbits_al) & 0x1f);
    const uint8_t * restrict texture = textureSetup;
    const uint8_t bits = bitsSetup;
    const int32_t local_asm1 = asm1;
    const intptr_t local_asm2 = asm2;
    uint32_t source;

    shade = shade & 0xffffff00;
    numPixels++;

	if (!RENDER_DRAW_CEILING_AND_FLOOR)
		return;

    while (numPixels) {

	    source = i5 >> shifter;
	    source = shld(source,i4,bits);
	    source = texture[source];

#if RENDER_LIMIT_PIXELS
		if (pixelsAllowed-- > 0)
#endif
			*dest = globalpalwritten[shade|source];

	    dest--;

	    i5 -= local_asm1;
	    i4 -= local_asm2;

	    numPixels--;

    }
}

static int32_t rmach_eax;
static int32_t rmach_ebx;
static int32_t rmach_ecx;
static const uint8_t* rmach_edx;
static int32_t rmach_esi;

void setuprhlineasm4(int32_t i1, int32_t i2, int32_t i3, const uint8_t* i4, int32_t i5, int32_t i6)
{
    rmach_eax = i1;
    rmach_ebx = i2;
    rmach_ecx = i3;
    rmach_edx = i4;
    rmach_esi = i5;
} 


OF_FASTTEXT void rhlineasm4(int32_t i1, const uint8_t* texture, int32_t i3, uint32_t i4, uint32_t i5, uint8_t* dest)
{
    uint32_t ebp = 0;
    int32_t numPixels;
	int32_t offset = i1 + 1;
	
    if (i1 <= 0) return;

    numPixels = i1;
    do {
	    i3 = ((i3&0xffffff00)|(*texture));
	    i4 -= rmach_eax;
	    ebp = (((i4+rmach_eax) < i4) ? -1 : 0);
	    i5 -= rmach_ebx;
        
	    if ((i5 + rmach_ebx) < i5)
            texture -= (rmach_ecx+1);
	    else
            texture -= rmach_ecx;
        
	    ebp &= rmach_esi;
	    i1 = ((i1&0xffffff00)|rmach_edx[i3]);

#if RENDER_LIMIT_PIXELS
		if (pixelsAllowed-- > 0)
#endif
			dest[numPixels - offset] = (i1&0xff);

	    texture -= ebp;
	    numPixels--;
    } while (numPixels);
}

static int32_t rmmach_eax;
static int32_t rmmach_ebx;
static int32_t rmmach_ecx;
static const uint8_t* rmmach_edx;
static int32_t setupTileHeight;
void setuprmhlineasm4(int32_t i1, int32_t i2, int32_t i3, const uint8_t* i4, int32_t tileHeight, int32_t i6)
{
    rmmach_eax = i1;
    rmmach_ebx = i2;
    rmmach_ecx = i3;
    rmmach_edx = i4;
    setupTileHeight = tileHeight;
} 


//FCS: ????
OF_FASTTEXT void rmhlineasm4(int32_t i1, const uint8_t* shade, int32_t colorIndex, int32_t i4, int32_t i5, uint8_t* dest)
{
    uint32_t ebp = 0;
    int32_t numPixels;
	int32_t offset = i1 + 1;
    
    if (i1 <= 0)
        return;

    numPixels = i1;
    do {
	    colorIndex = ((colorIndex&0xffffff00)|(*((uint8_t *)shade)));
	    i4 -= rmmach_eax;
	    ebp = (((i4+rmmach_eax) < i4) ? -1 : 0);
	    i5 -= rmmach_ebx;
        
	    if ((i5 + rmmach_ebx) < i5)
            shade -= (rmmach_ecx+1);
	    else
            shade -= rmmach_ecx;
        
	    ebp &= setupTileHeight;
        
        //Check if this colorIndex is the transparent color (255).
	    if ((colorIndex&0xff) != 255) {
#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
			{
				i1 = ((i1&0xffffff00)|rmmach_edx[colorIndex]);
				dest[numPixels - offset] = (i1 & 0xff);
			}
	    }
        
	    shade -= ebp;
	    numPixels--;
        
    } while (numPixels);
} 


//Variable used to draw column.
//This is how much you have to skip in the framebuffer in order to be one pixel below.
static OF_FASTDATA int32_t bytesperline;
void setBytesPerLine(int32_t _bytesperline)
{
    bytesperline = _bytesperline;
} 



static OF_FASTDATA uint8_t  mach3_al;

//FCS:  RENDER TOP AND BOTTOM COLUMN
OF_FASTTEXT int32_t prevlineasm1(int32_t i1, const uint8_t* palette, int32_t i3, int32_t i4, const uint8_t  *source, uint8_t  *dest)
{
    if (i3 == 0)
    {
		if (!RENDER_DRAW_TOP_AND_BOTTOM_COLUMN)
            return 0;

	    i1 += i4;
        i4 = ((uint32_t)i4) >> mach3_al;
	    i4 = (i4&0xffffff00) | source[i4];

#if RENDER_LIMIT_PIXELS
		if (pixelsAllowed-- > 0)
#endif
			*dest = palette[i4];

	    return i1;
    } else {
	    return vlineasm1(i1,palette,i3,i4,source,dest);
    }
}


//FCS: This is used to draw wall border vertical lines
OF_FASTTEXT int32_t vlineasm1(int32_t vince, const uint8_t * restrict palookupoffse, int32_t numPixels, int32_t vplce, const uint8_t * restrict texture, uint8_t * restrict dest)
{
    const uint8_t local_shift = mach3_al;
    const int32_t local_bpl = bytesperline;

    if (!RENDER_DRAW_WALL_BORDERS)
		return vplce;

    numPixels++;
    while (numPixels)
    {
	    uint32_t temp = ((uint32_t)vplce) >> local_shift;
	    temp = texture[temp];

#if RENDER_LIMIT_PIXELS
		if (pixelsAllowed-- > 0)
#endif
			*dest = palookupoffse[temp];

		vplce += vince;
	    dest += local_bpl;
	    numPixels--;
    }
    return vplce;
}


OF_FASTTEXT int32_t tvlineasm1(int32_t i1, const uint8_t * restrict texture, int32_t numPixels, int32_t i4, const uint8_t * restrict source, uint8_t * restrict dest)
{
    const uint8_t shiftValue = (globalshiftval & 0x1f);
    const int32_t local_bpl = bytesperline;
    const int local_transrev = transrev;

	numPixels++;
	while (numPixels)
	{
		uint32_t temp = ((uint32_t)i4) >> shiftValue;
		temp = source[temp];

		if (temp != TRANSPARENT_COLOR)
		{
			uint16_t colorIndex = texture[temp];
			colorIndex |= ((*dest)<<8);

			if (local_transrev)
				colorIndex = ((colorIndex>>8)|(colorIndex<<8));

#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
				*dest = transluc[colorIndex];
		}

		i4 += i1;
		dest += local_bpl;
		numPixels--;
	}
	return i4;
}


static OF_FASTDATA uint8_t  tran2shr;
static OF_FASTDATA const uint8_t* tran2pal_ebx;
static OF_FASTDATA const uint8_t* tran2pal_ecx;
/* Setup-only: not in BRAM (cold path) */
void setuptvlineasm2(int32_t i1, const uint8_t* i2, const uint8_t* i3)
{
	tran2shr = (i1&0x1f);
	tran2pal_ebx = i2;
	tran2pal_ecx = i3;
} /* */


OF_FASTTEXT void tvlineasm2(uint32_t i1, uint32_t i2, uintptr_t i3, uintptr_t i4, uint32_t i5, uintptr_t i6)
{
	uint32_t ebp = i1;
	const uint32_t tran2inca = i2;
	const uint32_t tran2incb = asm1;
	const uintptr_t tran2bufa = i3;
	const uintptr_t tran2bufb = i4;
	const uintptr_t tran2edi = asm2;
	const uintptr_t tran2edi1 = asm2 + 1;
	const int32_t local_bpl = bytesperline;
	const int local_transrev = transrev;

	i6 -= asm2;

	uintptr_t prev_i6;
	do {
		prev_i6 = i6;

		i1 = i5 >> tran2shr;
		i2 = ebp >> tran2shr;
		i5 += tran2inca;
		ebp += tran2incb;
		i3 = ((uint8_t *)tran2bufa)[i1];
		i4 = ((uint8_t *)tran2bufb)[i2];
		if (i3 == TRANSPARENT_COLOR) {
			if (i4 != TRANSPARENT_COLOR) {
				uint16_t val = tran2pal_ecx[i4];
				val |= (((uint8_t *)i6)[tran2edi1]<<8);

				if (local_transrev)
					val = ((val>>8)|(val<<8));

#if RENDER_LIMIT_PIXELS
				if (pixelsAllowed-- > 0)
#endif
					((uint8_t *)i6)[tran2edi1] = transluc[val];
			}
		} else if (i4 == TRANSPARENT_COLOR) {
			uint16_t val = tran2pal_ebx[i3];
			val |= (((uint8_t *)i6)[tran2edi]<<8);

			if (local_transrev)
				val = ((val>>8)|(val<<8));

#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
				((uint8_t *)i6)[tran2edi] = transluc[val];
		} else {
			uint16_t l = ((uint8_t *)i6)[tran2edi]<<8;
			uint16_t r = ((uint8_t *)i6)[tran2edi1]<<8;
			l |= tran2pal_ebx[i3];
			r |= tran2pal_ecx[i4];
			if (local_transrev) {
				l = ((l>>8)|(l<<8));
				r = ((r>>8)|(r<<8));
			}
#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
			{
				((uint8_t *)i6)[tran2edi] = transluc[l];
				((uint8_t *)i6)[tran2edi1] = transluc[r];
#if RENDER_LIMIT_PIXELS
				pixelsAllowed--;
#endif
			}
		}
		i6 += local_bpl;
	} while (i6 > prev_i6);  /* original x86 used carry flag from add; detect unsigned wrap */
	asm1 = i5;
	asm2 = ebp;
} 



static OF_FASTDATA uint8_t  machmv;
OF_FASTTEXT int32_t mvlineasm1(int32_t vince, const uint8_t * restrict palookupoffse, int32_t i3, int32_t vplce, const uint8_t * restrict texture, uint8_t * restrict dest)
{
    const uint8_t local_shift = machmv;
    const int32_t local_bpl = bytesperline;

    for(;i3>=0;i3--)
    {
		uint32_t temp = ((uint32_t)vplce) >> local_shift;
	    temp = texture[temp];

	    if (temp != TRANSPARENT_COLOR)
		{
#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
				*dest = palookupoffse[temp];
		}

	    vplce += vince;
	    dest += local_bpl;
    }
    return vplce;
}


/* Setup-only: not in BRAM (cold path) */
void setupvlineasm(int32_t i1)
{
    mach3_al = (i1&0x1f);
}

OF_FASTTEXT void vlineasm4(int32_t columnIndex, uint8_t * restrict framebuffer)
{
	if (!RENDER_DRAW_WALL_INSIDE)
		return;

	const uint8_t local_shift = mach3_al;
	const int32_t local_bpl = bytesperline;
	int i;
	uint32_t index = 0;
	const uint32_t length = ylookup[columnIndex];

	do {
		for (i = 0; i < 4; i++)
		{
			uint32_t temp = ((uint32_t)vplce[i]) >> local_shift;
			temp = (((uint8_t*)(bufplce[i]))[temp]);

#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
				framebuffer[index + i] = palookupoffse[i][temp];

			vplce[i] += vince[i];
		}
		index += local_bpl;
	} while (index < length);
}

/* Setup-only: not in BRAM (cold path) */
void setupmvlineasm(int32_t i1)
{
    //Only keep 5 first bits
    machmv = (i1&0x1f);
}

OF_FASTTEXT void mvlineasm4(int32_t columnIndex, uint8_t * restrict framebuffer)
{
    const uint8_t local_shift = machmv;
    const int32_t local_bpl = bytesperline;
    int i;
	uint32_t index = 0;
	const uint32_t length = ylookup[columnIndex];

    do {

#if RENDER_LIMIT_PIXELS
		if (pixelsAllowed <= 0)
			return;
#endif

        for (i = 0; i < 4; i++)
        {
	      uint32_t temp = ((uint32_t)vplce[i]) >> local_shift;
	      temp = (((uint8_t *)(bufplce[i]))[temp]);
	      if (temp != TRANSPARENT_COLOR)
		  {
#if RENDER_LIMIT_PIXELS
			  if (pixelsAllowed-- > 0)
#endif
				  framebuffer[index+i] = palookupoffse[i][temp];
		  }
	      vplce[i] += vince[i];
        }
        index += local_bpl;

    } while (index < length);
} 
/* END ---------------  WALLS RENDERING METHOD (USED TO BE HIGHLY OPTIMIZED ASSEMBLY) ----------------------------*/


/* ---------------  SPRITE RENDERING METHOD (USED TO BE HIGHLY OPTIMIZED ASSEMBLY) ----------------------------*/

OF_FASTDATA const uint8_t * tspal;
OF_FASTDATA uint32_t tsmach_eax1;
OF_FASTDATA uint32_t adder;
OF_FASTDATA uint32_t tsmach_eax3;
OF_FASTDATA uint32_t tsmach_ecx;
/* Setup-only: not in BRAM (cold path) */
void tsetupspritevline(const uint8_t * palette, int32_t i2, int32_t i3, int32_t i4, int32_t i5)
{
	tspal = palette;
	tsmach_eax1 = i5 << 16;
	adder = (i5 >> 16) + i2;
	tsmach_eax3 = adder + i4;
	tsmach_ecx = i3;
} 

/*
 FCS: Draw a sprite vertical line of pixels.
 */
OF_FASTTEXT void DrawSpriteVerticalLine(int32_t i2, int32_t numPixels, uint32_t i4, const uint8_t * restrict texture, uint8_t * restrict dest)
{
    const int32_t local_bpl = bytesperline;
    const int local_transrev = transrev;

	while (numPixels)
	{
		numPixels--;

		if (numPixels != 0)
		{
			uint8_t colorIndex;

			i4 += tsmach_ecx;

			if (i4 < (i4 - tsmach_ecx))
                adder = tsmach_eax3;

			colorIndex = *texture;

			i2 += tsmach_eax1;
			if (i2 < (i2 - tsmach_eax1))
                texture++;

			texture += adder;

			if (colorIndex != TRANSPARENT_COLOR)
			{
				uint16_t val = tspal[colorIndex];
				val |= (*dest)<<8;

				if (local_transrev)
					val = ((val>>8)|(val<<8));

				colorIndex = transluc[val];

#if RENDER_LIMIT_PIXELS
				if (pixelsAllowed-- > 0)
#endif
					*dest = colorIndex;
			}

			dest += local_bpl;
		}
	}
} 
/* END---------------  SPRITE RENDERING METHOD (USED TO BE HIGHLY OPTIMIZED ASSEMBLY) ----------------------------*/





















/* ---------------  FLOOR/CEILING RENDERING METHOD (USED TO BE HIGHLY OPTIMIZED ASSEMBLY) ----------------------------*/

void settrans(int32_t type){
	transrev = type;
}

static OF_FASTDATA uint8_t  * textureData;
static OF_FASTDATA uint8_t  * mmach_asm3;
static OF_FASTDATA int32_t mmach_asm1;
static OF_FASTDATA int32_t mmach_asm2;

void mhline(uint8_t  * texture, int32_t i2, int32_t numPixels, int32_t i4, int32_t i5, uint8_t* dest)
{
    textureData = texture;
    mmach_asm3 = asm3;
    mmach_asm1 = asm1;
    mmach_asm2 = asm2;
    mhlineskipmodify(i2,numPixels>>16,i5,dest);
}


static OF_FASTDATA uint8_t  mshift_al = 26;
static OF_FASTDATA uint8_t  mshift_bl = 6;
OF_FASTTEXT void mhlineskipmodify( uint32_t i2, int32_t numPixels, int32_t i5, uint8_t * restrict dest)
{
    const uint8_t local_shift_al = mshift_al;
    const uint8_t local_shift_bl = mshift_bl;
    const uint8_t * restrict local_texture = textureData;
    const uint8_t * restrict local_pal = mmach_asm3;
    const int32_t local_asm1 = mmach_asm1;
    const int32_t local_asm2 = mmach_asm2;

    while (numPixels >= 0)
    {
	    uint32_t ebx = i2 >> local_shift_al;
	    ebx = shld(ebx, (uint32_t)i5, local_shift_bl);
	    int32_t colorIndex = local_texture[ebx];

		if ((colorIndex&0xff) != TRANSPARENT_COLOR){
#if RENDER_LIMIT_PIXELS
            if (pixelsAllowed-- > 0)
#endif
				*dest = local_pal[colorIndex];
        }
	    i2 += local_asm1;
	    i5 += local_asm2;
	    dest++;
	    numPixels--;
    }
}


/* Setup-only: not in BRAM (cold path) */
void msethlineshift(int32_t i1, int32_t i2)
{
    i1 = 256-i1;
    mshift_al = (i1&0x1f);
    mshift_bl = (i2&0x1f);
}


static OF_FASTDATA uint8_t * tmach_eax;
static OF_FASTDATA uint8_t * tmach_asm3;
static OF_FASTDATA int32_t tmach_asm1;
static OF_FASTDATA int32_t tmach_asm2;

void thline(uint8_t  * i1, int32_t i2, int32_t i3, int32_t i4, int32_t i5, uint8_t * i6)
{
    tmach_eax = i1;
    tmach_asm3 = asm3;
    tmach_asm1 = asm1;
    tmach_asm2 = asm2;
    thlineskipmodify(asm2,i2,i3,i4,i5,i6);
}

static OF_FASTDATA uint8_t  tshift_al = 26;
static OF_FASTDATA uint8_t  tshift_bl = 6;
OF_FASTTEXT void thlineskipmodify(int32_t i1, uint32_t i2, uint32_t i3, int32_t i4, int32_t i5, uint8_t * restrict i6)
{
    const uint8_t local_shift_al = tshift_al;
    const uint8_t local_shift_bl = tshift_bl;
    const uint8_t * restrict local_tex = tmach_eax;
    const uint8_t * restrict local_pal = tmach_asm3;
    const int32_t local_asm1 = tmach_asm1;
    const int32_t local_asm2 = tmach_asm2;
    const int local_transrev = transrev;
    int counter = (i3>>16);

    while (counter >= 0)
    {
	    uint32_t ebx = i2 >> local_shift_al;
	    ebx = shld(ebx, (uint32_t)i5, local_shift_bl);
	    i1 = local_tex[ebx];
	    if ((i1&0xff) != TRANSPARENT_COLOR)
	    {
		    uint16_t val = local_pal[i1];
		    val |= (*i6)<<8;

		    if (local_transrev)
				val = ((val>>8)|(val<<8));

#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
			 *i6 = transluc[val];
	    }

	    i2 += local_asm1;
	    i5 += local_asm2;
	    i6++;
	    counter--;
    }
} 


/* Setup-only: not in BRAM (cold path) */
void tsethlineshift(int32_t i1, int32_t i2)
{
    i1 = 256-i1;
    tshift_al = (i1&0x1f);
    tshift_bl = (i2&0x1f);
}




static OF_FASTDATA intptr_t slopemach_ebx;
static OF_FASTDATA int32_t slopemach_ecx;
static OF_FASTDATA int32_t slopemach_edx;
static OF_FASTDATA uint8_t  slopemach_ah1;
static OF_FASTDATA uint8_t  slopemach_ah2;
static OF_FASTDATA float asm2_f;
typedef union { unsigned int i; float f; } bitwisef2i;
/* Setup-only: not in BRAM (cold path) */
void setupslopevlin(int32_t i1, intptr_t i2, int32_t i3)
{
    bitwisef2i c;
    slopemach_ebx = i2;
    slopemach_ecx = i3;
    slopemach_edx = (1<<(i1&0x1f)) - 1;
    slopemach_edx <<= ((i1&0x1f00)>>8);
    slopemach_ah1 = 32-((i1&0x1f00)>>8);
    slopemach_ah2 = (slopemach_ah1 - (i1&0x1f)) & 0x1f;
    c.f = asm2_f = (float)asm1;
    asm2 = c.i;
}

extern int32_t reciptable[2048];
extern int32_t globalx3, globaly3;
extern int32_t fpuasm;
#define low32(a) (((a)&0xffffffff))
#define high32(a) ((int)(((int64_t)(a)&(int64_t)0xffffffff00000000)>>32))

//FCS: Render RENDER_SLOPPED_CEILING_AND_FLOOR
OF_FASTTEXT void slopevlin(intptr_t i1, uint32_t i2, intptr_t* i3, uint32_t index, int32_t i4, int32_t i5, int32_t i6)
{
    bitwisef2i c;
	uintptr_t ecx, eax, ebx, edx, esi;
	uint32_t edi;
//This is so bad to cast asm3 to int then float :( !!!
    float a = (float)(int32_t) asm3 + asm2_f;
    i1 -= slopemach_ecx;
    esi = i5 + low32((int64_t)globalx3 * (int64_t)(i2<<3));
    edi = i6 + low32((int64_t)globaly3 * (int64_t)(i2<<3));
    ebx = i4;

	if (!RENDER_SLOPPED_CEILING_AND_FLOOR)
		return;

    do {
	    // -------------
	    // All this is calculating a fixed point approx. of 1/a
	    c.f = a;
	    fpuasm = eax = c.i;
	    edx = (((int32_t)eax) < 0) ? 0xffffffff : 0;
	    eax = eax << 1;
	    ecx = (eax>>24);	//  exponent
	    eax = ((eax&0xffe000)>>11);
	    ecx = ((ecx&0xffffff00)|((ecx-2)&0xff));
	    eax = reciptable[eax/4];
	    eax >>= (ecx&0x1f);
	    eax ^= edx;
	    // -------------
	    edx = i2;
	    i2 = eax;
	    eax -= edx;
	    ecx = low32((int64_t)globalx3 * (int64_t)eax);
	    eax = low32((int64_t)globaly3 * (int64_t)eax);
	    a += asm2_f;

	    asm4 = ebx;
	    ecx = ((ecx&0xffffff00)|(ebx&0xff));
	    if (ebx >= 8) ecx = ((ecx&0xffffff00)|8);

	    ebx = esi;
	    edx = edi;
	    while ((ecx&0xff))
	    {
		    ebx >>= slopemach_ah2;
		    esi += ecx;
		    edx >>= slopemach_ah1;
		    ebx &= slopemach_edx;
		    edi += eax;
		    i1 += slopemach_ecx;
		    edx = ((edx&0xffffff00)|((((uint8_t *)(ebx+edx))[slopemach_ebx])));
			ebx = i3[index];
			index--;
			eax = ((eax & 0xffffff00) | (*((uint8_t*)(ebx + edx))));
			ebx = esi;

#if RENDER_LIMIT_PIXELS
			if (pixelsAllowed-- > 0)
#endif
				*((uint8_t  *)i1) = (eax&0xff);

		    edx = edi;
		    ecx = ((ecx&0xffffff00)|((ecx-1)&0xff));

			
	    }
	    ebx = asm4;
	    ebx -= 8;	// BITSOFPRECISIONPOW

		

    } while ((int32_t)ebx > 0);
}


/* END ---------------  FLOOR/CEILING RENDERING METHOD (USED TO BE HIGHLY OPTIMIZED ASSEMBLY) ----------------------------*/
