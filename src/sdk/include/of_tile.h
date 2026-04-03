/*
 * of_tile.h -- Tile layer & sprite engine API for openfpgaOS
 *
 * 64x32 tilemap of 8x8 tiles, 4bpp with 16 sub-palettes.
 * 64 hardware sprites, 8x8 pixels, 4bpp.
 */

#ifndef OF_TILE_H
#define OF_TILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_TILE_COLS    64
#define OF_TILE_ROWS    32
#define OF_TILE_SIZE    8
#define OF_MAX_TILES    256
#define OF_MAX_SPRITES  64

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Tile Layer */

static inline void of_tile_enable(int enable, int priority) {
    __of_syscall2(OF_SYS_TILE_ENABLE, enable, priority);
}

static inline void of_tile_scroll(int x, int y) {
    __of_syscall2(OF_SYS_TILE_SCROLL, x, y);
}

static inline void of_tile_set(int col, int row, uint16_t entry) {
    __of_syscall3(OF_SYS_TILE_SET, col, row, entry);
}

static inline void of_tile_load_map(const uint16_t *data,
                                     int x, int y, int w, int h) {
    __of_syscall5(OF_SYS_TILE_LOAD_MAP, (long)data, x, y, w, h);
}

static inline void of_tile_load_chr(int first_tile,
                                     const void *data, int num_tiles) {
    __of_syscall3(OF_SYS_TILE_LOAD_CHR, first_tile, (long)data, num_tiles);
}

/* Sprite Engine */

static inline void of_sprite_enable(int enable) {
    __of_syscall1(OF_SYS_SPRITE_ENABLE, enable);
}

static inline void of_sprite_set(int index, int x, int y,
                                  int tile_id, int palette,
                                  int hflip, int vflip, int enable) {
    long packed = (palette & 0xF) |
                  ((hflip & 1) << 4) |
                  ((vflip & 1) << 5) |
                  ((enable & 1) << 6);
    __of_syscall5(OF_SYS_SPRITE_SET, index, x, y, tile_id, packed);
}

static inline void of_sprite_move(int index, int x, int y) {
    __of_syscall3(OF_SYS_SPRITE_MOVE, index, x, y);
}

static inline void of_sprite_load_chr(int first_tile,
                                       const void *data, int num_tiles) {
    __of_syscall3(OF_SYS_SPRITE_LOAD_CHR, first_tile, (long)data, num_tiles);
}

static inline void of_sprite_hide(int index) {
    __of_syscall1(OF_SYS_SPRITE_HIDE, index);
}

static inline void of_sprite_hide_all(void) {
    __of_syscall0(OF_SYS_SPRITE_HIDE_ALL);
}

#else /* OF_PC */

/* Tile Layer */
void of_tile_enable(int enable, int priority);
void of_tile_scroll(int x, int y);
void of_tile_set(int col, int row, uint16_t entry);
void of_tile_load_map(const uint16_t *data, int x, int y, int w, int h);
void of_tile_load_chr(int first_tile, const void *data, int num_tiles);

/* Sprite Engine */
void of_sprite_enable(int enable);
void of_sprite_set(int index, int x, int y, int tile_id, int palette,
                   int hflip, int vflip, int enable);
void of_sprite_move(int index, int x, int y);
void of_sprite_load_chr(int first_tile, const void *data, int num_tiles);
void of_sprite_hide(int index);
void of_sprite_hide_all(void);

#endif /* OF_PC */

/* Tile entry helper (pure computation, available on both platforms) */
static inline uint16_t of_tile_entry(int tile_id, int palette,
                                      int hflip, int vflip) {
    return (uint16_t)(
        (tile_id & 0xFF) |
        ((palette & 0xF) << 10) |
        (hflip ? (1 << 14) : 0) |
        (vflip ? (1 << 15) : 0)
    );
}

#ifdef __cplusplus
}
#endif

#endif /* OF_TILE_H */
