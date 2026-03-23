/*
 * of_video.h -- Video subsystem API for openfpgaOS
 *
 * 320x240 framebuffer, 8-bit indexed color, double-buffered.
 */

#ifndef OF_VIDEO_H
#define OF_VIDEO_H

#include <stdint.h>

/* Screen constants */
#define OF_SCREEN_W     320
#define OF_SCREEN_H     240

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline void of_video_init(void) {
    __of_syscall0(OF_SYS_VIDEO_INIT);
}

static inline uint8_t *of_video_surface(void) {
    return (uint8_t *)__of_syscall0(OF_SYS_VIDEO_GET_SURFACE);
}

static inline void of_video_flip(void) {
    __of_syscall0(OF_SYS_VIDEO_FLIP);
}

static inline void of_video_sync(void) {
    __of_syscall0(OF_SYS_VIDEO_WAIT_FLIP);
}

static inline void of_video_clear(uint8_t color) {
    __of_syscall1(OF_SYS_VIDEO_CLEAR, color);
}

static inline void of_video_palette(uint8_t index, uint32_t rgb) {
    __of_syscall2(OF_SYS_VIDEO_SET_PALETTE, index, rgb);
}

static inline void of_video_palette_bulk(const uint32_t *pal, int count) {
    __of_syscall2(OF_SYS_VIDEO_SET_PALETTE_BULK, (long)pal, count);
}

/* Convert and set a VGA 6-bit palette (768 bytes: R,G,B triplets, 0-63 range).
 * Converts to 8-bit 0x00RRGGBB and sets all 256 entries at once. */
static inline void of_video_palette_vga6(const uint8_t *vga_pal, int count) {
    uint32_t pal32[256];
    for (int i = 0; i < count && i < 256; i++) {
        uint8_t r = (vga_pal[i*3+0] * 255 + 31) / 63;
        uint8_t g = (vga_pal[i*3+1] * 255 + 31) / 63;
        uint8_t b = (vga_pal[i*3+2] * 255 + 31) / 63;
        pal32[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    of_video_palette_bulk(pal32, count);
}

static inline void of_video_flush(void) {
    __of_syscall0(OF_SYS_VIDEO_FLUSH_CACHE);
}

static inline void of_video_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x < OF_SCREEN_W && (unsigned)y < OF_SCREEN_H) {
        uint8_t *fb = of_video_surface();
        fb[y * OF_SCREEN_W + x] = color;
    }
}

/* Blit a source buffer centered vertically in the 320x240 surface.
 * Clears top/bottom bars. src must be src_w * src_h bytes (8-bit indexed). */
static inline void of_video_blit_letterbox(const uint8_t *src, int src_w, int src_h) {
    uint8_t *fb = of_video_surface();
    int y_offset = (OF_SCREEN_H - src_h) / 2;
    if (y_offset > 0)
        __builtin_memset(fb, 0, OF_SCREEN_W * y_offset);
    __builtin_memcpy(fb + OF_SCREEN_W * y_offset, src, src_w * src_h);
    int bottom = y_offset + src_h;
    if (bottom < OF_SCREEN_H)
        __builtin_memset(fb + OF_SCREEN_W * bottom, 0, OF_SCREEN_W * (OF_SCREEN_H - bottom));
}

/* Color mode constants */
#define OF_VIDEO_MODE_8BIT     0  /* 8-bit indexed: 256 colors, 1 byte/pixel */
#define OF_VIDEO_MODE_4BIT     1  /* 4-bit indexed: 16 colors, 0.5 byte/pixel */
#define OF_VIDEO_MODE_2BIT     2  /* 2-bit indexed: 4 colors, 0.25 byte/pixel */
#define OF_VIDEO_MODE_RGB565   3  /* 16-bit direct: R5G6B5, 2 bytes/pixel */
#define OF_VIDEO_MODE_RGB555   4  /* 15-bit direct: X1R5G5B5, 2 bytes/pixel */
#define OF_VIDEO_MODE_RGBA5551 5  /* 15+1 bit: R5G5B5A1, 2 bytes/pixel */

/* Framebuffer size per mode (320x240) */
#define OF_FB_SIZE_8BIT     (320 * 240)         /* 76,800 bytes */
#define OF_FB_SIZE_4BIT     (320 * 240 / 2)     /* 38,400 bytes */
#define OF_FB_SIZE_2BIT     (320 * 240 / 4)     /* 19,200 bytes */
#define OF_FB_SIZE_16BPP    (320 * 240 * 2)     /* 153,600 bytes */

static inline void of_video_set_color_mode(int mode) {
    __of_syscall1(OF_SYS_VIDEO_SET_COLOR_MODE, mode);
}

/* Get surface as 16-bit for direct color modes */
static inline uint16_t *of_video_surface16(void) {
    return (uint16_t *)of_video_surface();
}

#else /* OF_PC */

void     of_video_init(void);
uint8_t *of_video_surface(void);
void     of_video_flip(void);
void     of_video_sync(void);
void     of_video_clear(uint8_t color);
void     of_video_palette(uint8_t index, uint32_t rgb);
void     of_video_palette_bulk(const uint32_t *pal, int count);
void     of_video_flush(void);

/* Convert and set a VGA 6-bit palette (768 bytes: R,G,B triplets, 0-63 range).
 * Converts to 8-bit 0x00RRGGBB and sets all 256 entries at once. */
static inline void of_video_palette_vga6(const uint8_t *vga_pal, int count) {
    uint32_t pal32[256];
    for (int i = 0; i < count && i < 256; i++) {
        uint8_t r = (vga_pal[i*3+0] * 255 + 31) / 63;
        uint8_t g = (vga_pal[i*3+1] * 255 + 31) / 63;
        uint8_t b = (vga_pal[i*3+2] * 255 + 31) / 63;
        pal32[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    of_video_palette_bulk(pal32, count);
}

static inline void of_video_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x < OF_SCREEN_W && (unsigned)y < OF_SCREEN_H) {
        uint8_t *fb = of_video_surface();
        fb[y * OF_SCREEN_W + x] = color;
    }
}

/* Blit a source buffer centered vertically in the 320x240 surface.
 * Clears top/bottom bars. src must be src_w * src_h bytes (8-bit indexed). */
static inline void of_video_blit_letterbox(const uint8_t *src, int src_w, int src_h) {
    uint8_t *fb = of_video_surface();
    int y_offset = (OF_SCREEN_H - src_h) / 2;
    if (y_offset > 0)
        __builtin_memset(fb, 0, OF_SCREEN_W * y_offset);
    __builtin_memcpy(fb + OF_SCREEN_W * y_offset, src, src_w * src_h);
    int bottom = y_offset + src_h;
    if (bottom < OF_SCREEN_H)
        __builtin_memset(fb + OF_SCREEN_W * bottom, 0, OF_SCREEN_W * (OF_SCREEN_H - bottom));
}

#endif /* OF_PC */

/* Blit a rectangular region from src buffer to the framebuffer. */
static inline void of_blit(int dx, int dy, int w, int h,
                            const uint8_t *src, int src_stride) {
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < h; y++) {
        int fy = dy + y;
        if ((unsigned)fy >= OF_SCREEN_H) continue;
        for (int x = 0; x < w; x++) {
            int fx = dx + x;
            if ((unsigned)fx >= OF_SCREEN_W) continue;
            uint8_t px = src[y * src_stride + x];
            if (px) fb[fy * OF_SCREEN_W + fx] = px;
        }
    }
}

/* Blit with a fixed palette offset. */
static inline void of_blit_pal(int dx, int dy, int w, int h,
                                const uint8_t *src, int src_stride,
                                uint8_t pal_offset) {
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < h; y++) {
        int fy = dy + y;
        if ((unsigned)fy >= OF_SCREEN_H) continue;
        for (int x = 0; x < w; x++) {
            int fx = dx + x;
            if ((unsigned)fx >= OF_SCREEN_W) continue;
            uint8_t px = src[y * src_stride + x];
            if (px) fb[fy * OF_SCREEN_W + fx] = px + pal_offset;
        }
    }
}

/* Fill a rectangle with a solid palette index. */
static inline void of_fill_rect(int x, int y, int w, int h, uint8_t color) {
    uint8_t *fb = of_video_surface();
    for (int ry = 0; ry < h; ry++) {
        int fy = y + ry;
        if ((unsigned)fy >= OF_SCREEN_H) continue;
        for (int rx = 0; rx < w; rx++) {
            int fx = x + rx;
            if ((unsigned)fx >= OF_SCREEN_W) continue;
            fb[fy * OF_SCREEN_W + fx] = color;
        }
    }
}

#endif /* OF_VIDEO_H */
