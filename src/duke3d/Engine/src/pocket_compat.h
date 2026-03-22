//
//  pocket_compat.h
//  Duke3D - openfpgaOS (Pocket) platform compatibility
//
//  Replaces unix_compat.h for the POCKET target.
//  Does NOT include <sys/uio.h> or <dirent.h> (unavailable on this platform).
//

#ifndef Duke3D_pocket_compat_h
#define Duke3D_pocket_compat_h

#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// ── Byte order (RISC-V is little-endian) ──────────────────────────────
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

// ── min / max macros ────────────────────────────────────────────────────
#ifndef max
#define max(x, y)  (((x) > (y)) ? (x) : (y))
#endif

#ifndef min
#define min(x, y)  (((x) < (y)) ? (x) : (y))
#endif

// ── O_BINARY (not needed on UNIX-like targets) ─────────────────────────
#define O_BINARY 0

// ── POSIX permission alias ──────────────────────────────────────────────
#ifndef S_IREAD
#define S_IREAD S_IRUSR
#endif

// ── Case-insensitive string compare aliases ─────────────────────────────
#define stricmp  strcasecmp
#define strcmpi  strcasecmp

// ── Unix-like filesystem defines ────────────────────────────────────────
#define F_OK        0
#define PATH_SEP_CHAR '/'
#define PATH_SEP_STR  "/"
#define ROOTDIR       "/"
#define CURDIR        "."

// ── find_t (Duke3D file scanning) ────────────────────────────────────────
#include <dirent.h>

struct find_t {
    DIR *dir;
    char pattern[128];
    char name[256];
    unsigned long size;
    unsigned attrib;
};

#define _A_SUBDIR  0x10
#define _A_NORMAL  0x00

// ── dosdate_t ───────────────────────────────────────────────────────────
struct dosdate_t {
    unsigned char day, month;
    unsigned int year;
    unsigned char dayofweek;
};

/* _dos_getdate implemented in global.c */

// ── SDL type stubs (for code that references SDL types outside #ifdef) ──
typedef int SDL_bool;
#define SDL_TRUE  1
#define SDL_FALSE 0
typedef void SDL_Surface;
typedef void SDL_Rect;

// ── Missing stat permission bits ────────────────────────────────────────
#ifndef S_IWGRP
#define S_IWGRP 0020
#endif
#ifndef S_IWOTH
#define S_IWOTH 0002
#endif
#ifndef S_IXGRP
#define S_IXGRP 0010
#endif
#ifndef S_IXOTH
#define S_IXOTH 0001
#endif

// ── mkdir stub (Duke3D calls mkdir with 1 arg, POSIX needs 2) ───────────
static inline int _pocket_mkdir(const char *path, ...) {
    (void)path;
    return -1;
}
#define mkdir(...) _pocket_mkdir(__VA_ARGS__, 0)

#endif /* Duke3D_pocket_compat_h */
