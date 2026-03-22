/* assert.h -- openfpgaOS libc wrapper (no-op on FPGA) */
#ifndef _OF_ASSERT_H
#define _OF_ASSERT_H

#ifdef OF_PC
#include_next <assert.h>
#else
#define assert(x) ((void)0)
#endif

#endif /* _OF_ASSERT_H */
