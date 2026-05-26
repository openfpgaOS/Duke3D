/*
 * of_video.h -- Video subsystem API for openfpgaOS
 *
 * Triple-buffered framebuffer API. Boots as 320x240 8-bit indexed and can
 * switch to larger source framebuffer modes at runtime.
 */

#ifndef OF_VIDEO_H
#define OF_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Snapshot returned by of_video_get_timing().  The OS samples these
 * from the vblank IRQ and presented-swap state so apps can pace
 * interpolation against scanout instead of render-loop timing. */
#ifndef OF_VIDEO_TIMING_T_DEFINED
#define OF_VIDEO_TIMING_T_DEFINED
typedef struct of_video_timing {
    uint32_t vblank_count;
    uint32_t present_count;
    uint32_t last_presented_idx;
    uint32_t reserved;
    uint64_t last_vblank_us;
    uint64_t last_flip_presented_us;
} of_video_timing_t;
#endif

/* Screen constants */
#define OF_SCREEN_W     320
#define OF_SCREEN_H     240

/* Display mode constants */
#define OF_DISPLAY_TERMINAL    0  /* Terminal only */
#define OF_DISPLAY_FRAMEBUFFER 1  /* Framebuffer only */
#define OF_DISPLAY_OVERLAY     2  /* White terminal text over framebuffer */

#define OF_VIDEO_VTOTAL_AUTO 0u
#define OF_VIDEO_VTOTAL_60HZ 258u  /* experimental fast LCD mode, estimated ~60.14 Hz */
#define OF_VIDEO_VTOTAL_55HZ 285u
#define OF_VIDEO_VTOTAL_50HZ 310u
#define OF_VIDEO_VTOTAL_45HZ 340u
#define OF_VIDEO_VTOTAL_42HZ 375u

/* Color mode + framebuffer-size constants — referenced by both branches
 * (the PC SDL2 stub uses them too), so define here above the OF_PC fence. */
#define OF_VIDEO_MODE_8BIT     0  /* 8-bit indexed: 256 colors, 1 byte/pixel */
#define OF_VIDEO_MODE_4BIT     1  /* 4-bit indexed: 16 colors, 0.5 byte/pixel */
#define OF_VIDEO_MODE_2BIT     2  /* 2-bit indexed: 4 colors, 0.25 byte/pixel */
#define OF_VIDEO_MODE_RGB565   3  /* 16-bit direct: R5G6B5, 2 bytes/pixel */
#define OF_VIDEO_MODE_RGB555   4  /* 15-bit direct: X1R5G5B5, 2 bytes/pixel */
#define OF_VIDEO_MODE_RGBA5551 5  /* 15+1 bit: R5G5B5A1, 2 bytes/pixel */

#define OF_VIDEO_MAX_WIDTH     800
#define OF_VIDEO_MAX_HEIGHT    600

#ifndef OF_VIDEO_MODE_T_DEFINED
#define OF_VIDEO_MODE_T_DEFINED
typedef struct of_video_mode {
    uint16_t width;
    uint16_t height;
    uint16_t stride;      /* bytes per source framebuffer row, 0 = default */
    uint8_t color_mode;   /* OF_VIDEO_MODE_* */
    uint8_t reserved;
} of_video_mode_t;
#endif

/* Framebuffer size per mode (320x240) */
#define OF_FB_SIZE_8BIT     (320 * 240)         /* 76,800 bytes */
#define OF_FB_SIZE_4BIT     (320 * 240 / 2)     /* 38,400 bytes */
#define OF_FB_SIZE_2BIT     (320 * 240 / 4)     /* 19,200 bytes */
#define OF_FB_SIZE_16BPP    (320 * 240 * 2)     /* 153,600 bytes */

#ifndef OF_PC

#include "of_services.h"

#define OF_VIDEO_SVC_INDEX(field) \
    ((uint32_t)((offsetof(struct of_services_table, field) - \
                 offsetof(struct of_services_table, video_init)) / sizeof(void *)))

static inline void of_video_init(void) {
    OF_SVC->video_init();
}

static inline uint8_t *of_video_surface(void) {
    return OF_SVC->video_get_surface();
}

static inline void of_video_flip(void) {
    OF_SVC->video_flip();
}

/* Wait for the most recent of_video_flip() to be presented (vsync). */
static inline void of_video_wait_flip(void) {
    OF_SVC->video_wait_flip();
}

/* GPU-triggered flip path (cr-gpu-triggered-flip.md).
 *
 * Returns the idx of the next free draw buffer.  Pass `just_flipped_idx`
 * = the idx of the buffer the caller just emitted CMD_FLIP for via
 * of_gpu_flip_to(), and `fence_token` = the token returned by that
 * helper.  On the very first call (no previous flip), pass
 * just_flipped_idx=-1 and fence_token=0 — the kernel returns the
 * initial draw idx without any wait.
 *
 * The kernel waits only for fence_reached>=fence_token (proves
 * CMD_FLIP retired and the slave latched fb_swap_pending=1), then
 * returns the third buffer: not current scanout and not queued for
 * next vsync.  It does not wait for the vsync swap to complete, so
 * rendering can overlap scanout.  Pair with of_video_wait_flip()
 * before queuing another flip if the app wants one outstanding flip
 * at a time, and with of_video_buffer_addr(idx) to get the FB address. */
static inline int of_video_acquire_next(int just_flipped_idx,
                                         uint32_t fence_token) {
    return OF_SVC->video_acquire_next(just_flipped_idx, fence_token);
}

/* Address of buffer `idx` (0/1/2). */
static inline uint8_t *of_video_buffer_addr(int idx) {
    return OF_SVC->video_buffer_addr(idx);
}

static inline void of_video_clear(uint8_t color) {
    OF_SVC->video_clear(color);
}

static inline void of_video_palette(uint8_t index, uint32_t rgb) {
    OF_SVC->video_set_palette(index, rgb);
}

static inline void of_video_palette_bulk(const uint32_t *pal, int count) {
    OF_SVC->video_set_palette_bulk(pal, count);
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

/* Set a VGA 4-byte palette (6-bit BGR format: B6 G6 R6 pad per entry).
 * Kernel converts 6-bit→8-bit directly — no userspace math needed. */
static inline void of_video_palette_vga4(const uint8_t *bgra6, int count) {
    OF_SVC->video_set_palette_vga4(bgra6, count);
}

static inline void of_video_flush(void) {
    OF_SVC->video_flush_cache();
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

static inline void of_video_set_display_mode(int mode) {
    OF_SVC->video_set_display_mode(mode);
}

/* Color mode + FB size constants are defined above the OF_PC fence. */

static inline void of_video_set_color_mode(int mode) {
    OF_SVC->video_set_color_mode(mode);
}

static inline int of_video_set_mode(const of_video_mode_t *mode) {
    if (OF_SVC->count > OF_VIDEO_SVC_INDEX(video_set_mode) &&
        OF_SVC->video_set_mode) {
        return OF_SVC->video_set_mode(mode);
    }
    return -1;
}

static inline void of_video_get_mode(of_video_mode_t *out) {
    if (!out)
        return;
    if (OF_SVC->count > OF_VIDEO_SVC_INDEX(video_get_mode) &&
        OF_SVC->video_get_mode) {
        OF_SVC->video_get_mode(out);
    } else {
        out->width = OF_SCREEN_W;
        out->height = OF_SCREEN_H;
        out->stride = OF_SCREEN_W;
        out->color_mode = OF_VIDEO_MODE_8BIT;
        out->reserved = 0;
    }
}

static inline int of_video_get_mode_count(void) {
    if (OF_SVC->count > OF_VIDEO_SVC_INDEX(video_get_mode_count) &&
        OF_SVC->video_get_mode_count) {
        return OF_SVC->video_get_mode_count();
    }
    return 1;
}

static inline int of_video_get_mode_info(int index, of_video_mode_t *out) {
    if (OF_SVC->count > OF_VIDEO_SVC_INDEX(video_get_mode_info) &&
        OF_SVC->video_get_mode_info) {
        return OF_SVC->video_get_mode_info(index, out);
    }
    if (!out || index != 0)
        return -1;
    out->width = OF_SCREEN_W;
    out->height = OF_SCREEN_H;
    out->stride = OF_SCREEN_W;
    out->color_mode = OF_VIDEO_MODE_8BIT;
    out->reserved = 0;
    return 0;
}

/* Register a callback invoked on every vsync (vblank) IRQ.
 * Pass NULL to disable. Callback runs in kernel context — keep it short. */
static inline void of_video_set_vsync_callback(void (*cb)(void)) {
    OF_SVC->video_set_vsync_callback(cb);
}

static inline void of_video_get_timing(of_video_timing_t *out) {
    if (!out)
        return;
    if (OF_SVC->count > OF_VIDEO_SVC_INDEX(video_get_timing) &&
        OF_SVC->video_get_timing) {
        OF_SVC->video_get_timing(out);
    } else {
        out->vblank_count = 0;
        out->present_count = 0;
        out->last_presented_idx = 0;
        out->reserved = 0;
        out->last_vblank_us = 0;
        out->last_flip_presented_us = 0;
    }
}

static inline uint64_t of_video_last_vblank_us(void) {
    of_video_timing_t timing;
    of_video_get_timing(&timing);
    return timing.last_vblank_us;
}

static inline uint64_t of_video_last_flip_presented_us(void) {
    of_video_timing_t timing;
    of_video_get_timing(&timing);
    return timing.last_flip_presented_us;
}

static inline uint32_t of_video_vblank_count(void) {
    of_video_timing_t timing;
    of_video_get_timing(&timing);
    return timing.vblank_count;
}

/* Request a fixed scanout V_TOTAL, or pass OF_VIDEO_VTOTAL_AUTO to restore
 * the OS automatic cadence/recovery policy. Hardware clamps again, and
 * Analogizer/SNAC fixed-rate output overrides this request. */
static inline void of_video_set_refresh_vtotal(uint32_t v_total) {
    if (OF_SVC->count > OF_VIDEO_SVC_INDEX(video_set_refresh_vtotal) &&
        OF_SVC->video_set_refresh_vtotal) {
        OF_SVC->video_set_refresh_vtotal(v_total);
    }
}

/* Get surface as 16-bit for direct color modes */
static inline uint16_t *of_video_surface16(void) {
    return (uint16_t *)of_video_surface();
}

#else /* OF_PC */

void     of_video_init(void);
uint8_t *of_video_surface(void);
void     of_video_flip(void);
void     of_video_wait_flip(void);
void     of_video_clear(uint8_t color);
void     of_video_palette(uint8_t index, uint32_t rgb);
void     of_video_palette_bulk(const uint32_t *pal, int count);
void     of_video_flush(void);
void     of_video_set_display_mode(int mode);
static inline int of_video_set_mode(const of_video_mode_t *mode) {
    (void)mode;
    return 0;
}
static inline void of_video_get_mode(of_video_mode_t *out) {
    if (!out)
        return;
    out->width = OF_SCREEN_W;
    out->height = OF_SCREEN_H;
    out->stride = OF_SCREEN_W;
    out->color_mode = OF_VIDEO_MODE_8BIT;
    out->reserved = 0;
}
static inline int of_video_get_mode_count(void) {
    return 1;
}
static inline int of_video_get_mode_info(int index, of_video_mode_t *out) {
    if (!out || index != 0)
        return -1;
    of_video_get_mode(out);
    return 0;
}
void     of_video_get_timing(of_video_timing_t *out);
uint64_t of_video_last_vblank_us(void);
uint64_t of_video_last_flip_presented_us(void);
uint32_t of_video_vblank_count(void);
static inline void of_video_set_refresh_vtotal(uint32_t v_total) {
    (void)v_total;
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

/* Blit a rectangular region from src buffer to the framebuffer.
 * Transparent: pixel value 0 is skipped. For opaque blits, use of_blit_opaque. */
static inline void of_blit(int dx, int dy, int w, int h,
                            const uint8_t *src, int src_stride) {
    uint8_t *fb = of_video_surface();
    /* Clip to screen bounds */
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > OF_SCREEN_W) w = OF_SCREEN_W - dx;
    if (dy + h > OF_SCREEN_H) h = OF_SCREEN_H - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src + (sy + y) * src_stride + sx;
        uint8_t *dp = fb + (dy + y) * OF_SCREEN_W + dx;
        for (int x = 0; x < w; x++)
            if (sp[x]) dp[x] = sp[x];
    }
}

/* Opaque blit: copies all pixels (no transparency check). Uses memcpy per row. */
static inline void of_blit_opaque(int dx, int dy, int w, int h,
                                   const uint8_t *src, int src_stride) {
    uint8_t *fb = of_video_surface();
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > OF_SCREEN_W) w = OF_SCREEN_W - dx;
    if (dy + h > OF_SCREEN_H) h = OF_SCREEN_H - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++)
        __builtin_memcpy(fb + (dy + y) * OF_SCREEN_W + dx,
                         src + (sy + y) * src_stride + sx, w);
}

/* Blit with a fixed palette offset (transparent: pixel 0 skipped). */
static inline void of_blit_pal(int dx, int dy, int w, int h,
                                const uint8_t *src, int src_stride,
                                uint8_t pal_offset) {
    uint8_t *fb = of_video_surface();
    int sx = 0, sy = 0;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > OF_SCREEN_W) w = OF_SCREEN_W - dx;
    if (dy + h > OF_SCREEN_H) h = OF_SCREEN_H - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src + (sy + y) * src_stride + sx;
        uint8_t *dp = fb + (dy + y) * OF_SCREEN_W + dx;
        for (int x = 0; x < w; x++)
            if (sp[x]) dp[x] = sp[x] + pal_offset;
    }
}

/* Fill a rectangle with a solid palette index. Uses memset per row. */
static inline void of_fill_rect(int x, int y, int w, int h, uint8_t color) {
    uint8_t *fb = of_video_surface();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > OF_SCREEN_W) w = OF_SCREEN_W - x;
    if (y + h > OF_SCREEN_H) h = OF_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int ry = 0; ry < h; ry++)
        __builtin_memset(fb + (y + ry) * OF_SCREEN_W + x, color, w);
}

#ifdef __cplusplus
}
#endif

#endif /* OF_VIDEO_H */
