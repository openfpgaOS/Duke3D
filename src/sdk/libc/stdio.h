/* stdio.h -- openfpgaOS libc wrapper
 *
 * File I/O and printf route through the OS kernel's musl libc jump table.
 * musl handles internal buffering, so fread/fwrite work correctly regardless
 * of the requested size.
 */
#ifndef _OF_STDIO_H
#define _OF_STDIO_H

#ifdef OF_PC
#include_next <stdio.h>
#else

#include "of_libc.h"

typedef void FILE;

#define __OF_JT ((const struct of_libc_table *)OF_LIBC_ADDR)

#define stdout (__OF_JT->stdout_ptr)
#define stderr (__OF_JT->stderr_ptr)
#define NULL   ((void *)0)
#define EOF    (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ======================================================================
 * File I/O — routed through musl via jump table
 * ====================================================================== */

static inline FILE *fopen(const char *path, const char *mode)              { return (FILE *)__OF_JT->fopen(path, mode); }
static inline int   fclose(FILE *f)                                         { return __OF_JT->fclose(f); }
static inline size_t fread(void *p, size_t sz, size_t n, FILE *f)          { return __OF_JT->fread(p, sz, n, f); }
static inline size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)   { return __OF_JT->fwrite(p, sz, n, f); }
static inline int   fseek(FILE *f, long off, int whence)                    { return __OF_JT->fseek(f, off, whence); }
static inline long  ftell(FILE *f)                                          { return __OF_JT->ftell(f); }
static inline char *fgets(char *s, int n, FILE *f)                          { return __OF_JT->fgets(s, n, f); }
static inline int   fputs(const char *s, FILE *f)                           { return __OF_JT->fputs(s, f); }

static inline int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    __OF_JT->fwrite(&ch, 1, 1, f);
    return c;
}

static inline int fgetc(FILE *f) {
    unsigned char c;
    if (__OF_JT->fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}

static inline int   feof(FILE *f)       { (void)f; return 0; }
static inline int   ferror(FILE *f)     { (void)f; return 0; }
static inline void  clearerr(FILE *f)   { (void)f; }
static inline int   fflush(FILE *f)     { (void)f; return 0; }
static inline void  rewind(FILE *f)     { __OF_JT->fseek(f, 0, 0); }
static inline int   remove(const char *p) { (void)p; return -1; }

/* ======================================================================
 * Console output
 * ====================================================================== */

static inline int putchar(int c) {
    char s[2] = { (char)c, '\0' };
    __OF_JT->printf("%s", s);
    return c;
}

static inline int puts(const char *s) {
    __OF_JT->printf("%s\n", s);
    return 0;
}

/* ======================================================================
 * Formatted output — jump table macros
 * ====================================================================== */

#define snprintf  __OF_JT->snprintf
#define vsnprintf __OF_JT->vsnprintf
#define printf    __OF_JT->printf
#define fprintf   __OF_JT->fprintf
#define sprintf   __OF_JT->sprintf
#define vsprintf  __OF_JT->vsprintf
#define sscanf    __OF_JT->sscanf

#endif /* OF_PC */
#endif /* _OF_STDIO_H */
