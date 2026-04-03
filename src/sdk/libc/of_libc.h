/*
 * of_libc.h -- openfpgaOS C Library Jump Table
 *
 * The OS populates a table of libc function pointers at a fixed address
 * before launching the application. Apps call standard C functions
 * through this table with zero syscall overhead (indirect call only).
 *
 * The table is append-only and versioned: new functions are added at
 * the end, old apps on new OS work unchanged, and apps can check
 * `count` for compatibility with newer functions.
 *
 * Apps should NOT include this header directly. Instead, use standard
 * headers (<math.h>, <string.h>, etc.) which resolve through the table
 * when compiled with -I<path-to-libc_include>.
 */

#ifndef OF_LIBC_H
#define OF_LIBC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define OF_LIBC_MAGIC   0x4F46434C  /* 'OFCL' */
#define OF_LIBC_VERSION 1
#define OF_LIBC_ADDR    0x103FF000

struct of_libc_table {
    uint32_t magic;
    uint32_t version;
    uint32_t count;         /* number of entries after this header */
    uint32_t _reserved;

    /* -- memory (4) -- */
    void *(*malloc)(size_t);
    void (*free)(void *);
    void *(*realloc)(void *, size_t);
    void *(*calloc)(size_t, size_t);

    /* -- string (12) -- */
    void *(*memcpy)(void *, const void *, size_t);
    void *(*memset)(void *, int, size_t);
    void *(*memmove)(void *, const void *, size_t);
    int (*memcmp)(const void *, const void *, size_t);
    size_t (*strlen)(const char *);
    int (*strcmp)(const char *, const char *);
    int (*strncmp)(const char *, const char *, size_t);
    char *(*strcpy)(char *, const char *);
    char *(*strncpy)(char *, const char *, size_t);
    char *(*strstr)(const char *, const char *);
    char *(*strchr)(const char *, int);
    char *(*strrchr)(const char *, int);

    /* -- stdio (6) -- */
    int (*snprintf)(char *, size_t, const char *, ...);
    int (*vsnprintf)(char *, size_t, const char *, va_list);
    int (*printf)(const char *, ...);
    int (*fprintf)(void *, const char *, ...);
    void *stdout_ptr;       /* musl FILE* for stdout */
    void *stderr_ptr;       /* musl FILE* for stderr */

    /* -- stdlib (9) -- */
    int (*abs)(int);
    long (*labs)(long);
    int (*atoi)(const char *);
    long (*strtol)(const char *, char **, int);
    unsigned long (*strtoul)(const char *, char **, int);
    void (*qsort)(void *, size_t, size_t, int(*)(const void *, const void *));
    void *(*bsearch)(const void *, const void *, size_t, size_t,
                     int (*)(const void *, const void *));
    int (*rand)(void);
    void (*srand)(unsigned int);

    /* -- math (21) -- */
    float (*sinf)(float);
    float (*cosf)(float);
    float (*tanf)(float);
    float (*asinf)(float);
    float (*acosf)(float);
    float (*atan2f)(float, float);
    float (*sqrtf)(float);
    float (*fmodf)(float, float);
    float (*floorf)(float);
    float (*ceilf)(float);
    float (*roundf)(float);
    float (*fabsf)(float);
    float (*fmaxf)(float, float);
    float (*fminf)(float, float);
    float (*powf)(float, float);
    float (*logf)(float);
    float (*log2f)(float);
    float (*expf)(float);
    double (*sin)(double);
    double (*cos)(double);
    double (*sqrt)(double);

    /* ================================================================
     * Extended entries (added in count=83 expansion)
     * Old apps (count<=52) ignore these; new apps check count >= 83.
     * ================================================================ */

    /* -- string extended (7) -- */
    char *(*strcat)(char *, const char *);
    char *(*strncat)(char *, const char *, size_t);
    char *(*strdup)(const char *);
    void *(*memchr)(const void *, int, size_t);
    char *(*strtok)(char *, const char *);
    size_t (*strspn)(const char *, const char *);
    size_t (*strcspn)(const char *, const char *);

    /* -- stdio extended (11) -- */
    int (*sprintf)(char *, const char *, ...);
    int (*vsprintf)(char *, const char *, va_list);
    int (*sscanf)(const char *, const char *, ...);
    void *(*fopen)(const char *, const char *);          /* FILE* */
    int (*fclose)(void *);                                /* FILE* */
    size_t (*fread)(void *, size_t, size_t, void *);     /* FILE* */
    size_t (*fwrite)(const void *, size_t, size_t, void *); /* FILE* */
    int (*fseek)(void *, long, int);                      /* FILE* */
    long (*ftell)(void *);                                /* FILE* */
    char *(*fgets)(char *, int, void *);                  /* FILE* */
    int (*fputs)(const char *, void *);                   /* FILE* */

    /* -- stdlib extended (4) -- */
    double (*atof)(const char *);
    double (*strtod)(const char *, char **);
    long long (*strtoll)(const char *, char **, int);
    unsigned long long (*strtoull)(const char *, char **, int);

    /* -- ctype (9) -- */
    int (*toupper)(int);
    int (*tolower)(int);
    int (*isalpha)(int);
    int (*isdigit)(int);
    int (*isalnum)(int);
    int (*isspace)(int);
    int (*isupper)(int);
    int (*islower)(int);
    int (*isprint)(int);

    /* ================================================================
     * POSIX I/O (5) — raw file descriptors for game engine ports
     * ================================================================ */
    int  (*open)(const char *, int, ...);
    int  (*close)(int);
    int  (*read)(int, void *, unsigned int);
    int  (*write)(int, const void *, unsigned int);
    long long (*lseek)(int, long long, int);
};

/* Total function pointers + data pointers in the table (excluding header) */
#define OF_LIBC_COUNT 88

/* App-side accessor */
#define __of_libc ((const struct of_libc_table *)OF_LIBC_ADDR)

/* ======================================================================
 * Named slot indices for each function in the table
 * ====================================================================== */

/* Memory (slots 0-3) */
#define OF_LIBC_SLOT_MALLOC         0
#define OF_LIBC_SLOT_FREE           1
#define OF_LIBC_SLOT_REALLOC        2
#define OF_LIBC_SLOT_CALLOC         3

/* String (slots 4-15) */
#define OF_LIBC_SLOT_MEMCPY         4
#define OF_LIBC_SLOT_MEMSET         5
#define OF_LIBC_SLOT_MEMMOVE        6
#define OF_LIBC_SLOT_MEMCMP         7
#define OF_LIBC_SLOT_STRLEN         8
#define OF_LIBC_SLOT_STRCMP          9
#define OF_LIBC_SLOT_STRNCMP        10
#define OF_LIBC_SLOT_STRCPY         11
#define OF_LIBC_SLOT_STRNCPY        12
#define OF_LIBC_SLOT_STRSTR         13
#define OF_LIBC_SLOT_STRCHR         14
#define OF_LIBC_SLOT_STRRCHR        15

/* Stdio (slots 16-21) */
#define OF_LIBC_SLOT_SNPRINTF       16
#define OF_LIBC_SLOT_VSNPRINTF      17
#define OF_LIBC_SLOT_PRINTF         18
#define OF_LIBC_SLOT_FPRINTF        19
#define OF_LIBC_SLOT_STDOUT_PTR     20
#define OF_LIBC_SLOT_STDERR_PTR     21

/* Stdlib (slots 22-30) */
#define OF_LIBC_SLOT_ABS            22
#define OF_LIBC_SLOT_LABS           23
#define OF_LIBC_SLOT_ATOI           24
#define OF_LIBC_SLOT_STRTOL         25
#define OF_LIBC_SLOT_STRTOUL        26
#define OF_LIBC_SLOT_QSORT          27
#define OF_LIBC_SLOT_BSEARCH        28
#define OF_LIBC_SLOT_RAND           29
#define OF_LIBC_SLOT_SRAND          30

/* Math (slots 31-51) */
#define OF_LIBC_SLOT_SINF           31
#define OF_LIBC_SLOT_COSF           32
#define OF_LIBC_SLOT_TANF           33
#define OF_LIBC_SLOT_ASINF          34
#define OF_LIBC_SLOT_ACOSF          35
#define OF_LIBC_SLOT_ATAN2F         36
#define OF_LIBC_SLOT_SQRTF          37
#define OF_LIBC_SLOT_FMODF          38
#define OF_LIBC_SLOT_FLOORF         39
#define OF_LIBC_SLOT_CEILF          40
#define OF_LIBC_SLOT_ROUNDF         41
#define OF_LIBC_SLOT_FABSF          42
#define OF_LIBC_SLOT_FMAXF          43
#define OF_LIBC_SLOT_FMINF          44
#define OF_LIBC_SLOT_POWF           45
#define OF_LIBC_SLOT_LOGF           46
#define OF_LIBC_SLOT_LOG2F          47
#define OF_LIBC_SLOT_EXPF           48
#define OF_LIBC_SLOT_SIN            49
#define OF_LIBC_SLOT_COS            50
#define OF_LIBC_SLOT_SQRT           51

/* String extended (slots 52-58) */
#define OF_LIBC_SLOT_STRCAT         52
#define OF_LIBC_SLOT_STRNCAT        53
#define OF_LIBC_SLOT_STRDUP         54
#define OF_LIBC_SLOT_MEMCHR         55
#define OF_LIBC_SLOT_STRTOK         56
#define OF_LIBC_SLOT_STRSPN         57
#define OF_LIBC_SLOT_STRCSPN        58

/* Stdio extended (slots 59-69) */
#define OF_LIBC_SLOT_SPRINTF        59
#define OF_LIBC_SLOT_VSPRINTF       60
#define OF_LIBC_SLOT_SSCANF         61
#define OF_LIBC_SLOT_FOPEN          62
#define OF_LIBC_SLOT_FCLOSE         63
#define OF_LIBC_SLOT_FREAD          64
#define OF_LIBC_SLOT_FWRITE         65
#define OF_LIBC_SLOT_FSEEK          66
#define OF_LIBC_SLOT_FTELL          67
#define OF_LIBC_SLOT_FGETS          68
#define OF_LIBC_SLOT_FPUTS          69

/* Stdlib extended (slots 70-73) */
#define OF_LIBC_SLOT_ATOF           70
#define OF_LIBC_SLOT_STRTOD         71
#define OF_LIBC_SLOT_STRTOLL        72
#define OF_LIBC_SLOT_STRTOULL       73

/* Ctype (slots 74-82) */
#define OF_LIBC_SLOT_TOUPPER        74
#define OF_LIBC_SLOT_TOLOWER        75
#define OF_LIBC_SLOT_ISALPHA        76
#define OF_LIBC_SLOT_ISDIGIT        77
#define OF_LIBC_SLOT_ISALNUM        78
#define OF_LIBC_SLOT_ISSPACE        79
#define OF_LIBC_SLOT_ISUPPER        80
#define OF_LIBC_SLOT_ISLOWER        81
#define OF_LIBC_SLOT_ISPRINT        82

/* POSIX I/O (slots 83-87) */
#define OF_LIBC_SLOT_OPEN           83
#define OF_LIBC_SLOT_CLOSE          84
#define OF_LIBC_SLOT_READ           85
#define OF_LIBC_SLOT_WRITE          86
#define OF_LIBC_SLOT_LSEEK          87

#ifdef __cplusplus
}
#endif

#endif /* OF_LIBC_H */

