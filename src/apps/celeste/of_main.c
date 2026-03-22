/*
 * Celeste Classic -- openfpgaOS port
 *
 * Replaces the SDL frontend from ccleste with of.h API calls.
 * Game logic is in celeste.c/celeste.h (MIT license, lemon32767).
 *
 * Renders the 128x128 PICO-8 screen at 2x scale into the 320x240
 * framebuffer, centered horizontally and vertically.
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>

#include "of.h"
#include "celeste.h"

/* tilemap data (extracted from PICO-8 cartridge) */
#include "tilemap.h"

/* ======================================================================
 * PICO-8 palette (16 colors)
 * ====================================================================== */

static const uint32_t base_palette[16] = {
    0x000000, 0x1D2B53, 0x7E2553, 0x008751,
    0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
    0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436,
    0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA
};

static uint32_t palette[16];

static void reset_palette(void) {
    memcpy(palette, base_palette, sizeof(palette));
}

/* ======================================================================
 * Sprite / font data (embedded from BMP files)
 *
 * gfx.bmp  = 128x64, 4bpp indexed = PICO-8 spritesheet
 * font.bmp = 128x85, 1bpp = PICO-8 font (actually only 128x48 used: 6 rows of 16 chars)
 *
 * We convert these to flat 8-bit arrays at startup.
 * ====================================================================== */

/* Spritesheet: 128x64 pixels, each pixel is a 4-bit palette index */
static uint8_t gfx_data[128 * 64];

/* Font: 128x85 pixels, 1-bit (0 or 7) */
static uint8_t font_data[128 * 85];

/* BMP loader: reads uncompressed 4bpp or 1bpp BMP into 8-bit buffer */
static int load_bmp(const uint8_t *bmp, int bmp_len, uint8_t *out, int w, int h, int bpp) {
    if (bmp_len < 54) return -1;

    /* Read BMP header */
    int data_offset = bmp[10] | (bmp[11] << 8) | (bmp[12] << 16) | (bmp[13] << 24);
    int bmp_w = bmp[18] | (bmp[19] << 8) | (bmp[20] << 16) | (bmp[21] << 24);
    int bmp_h = bmp[22] | (bmp[23] << 8) | (bmp[24] << 16) | (bmp[25] << 24);
    int top_down = 0;
    if (bmp_h < 0) { bmp_h = -bmp_h; top_down = 1; }

    if (bmp_w != w || bmp_h != h) return -2;

    int row_bytes;
    if (bpp == 4)
        row_bytes = (w + 1) / 2;
    else /* bpp == 1 */
        row_bytes = (w + 7) / 8;

    /* Rows are padded to 4-byte boundaries */
    int row_stride = (row_bytes + 3) & ~3;

    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h - 1 - y);
        const uint8_t *row = bmp + data_offset + src_y * row_stride;

        for (int x = 0; x < w; x++) {
            uint8_t pixel;
            if (bpp == 4) {
                uint8_t byte = row[x / 2];
                pixel = (x & 1) ? (byte & 0x0F) : (byte >> 4);
            } else {
                uint8_t byte = row[x / 8];
                pixel = (byte >> (7 - (x & 7))) & 1;
            }
            out[y * w + x] = pixel;
        }
    }
    return 0;
}

/* ======================================================================
 * Intermediate 128x128 framebuffer (PICO-8 screen)
 *
 * Stores resolved palette indices: when pal(a,b) is active and
 * color 'a' is drawn, we find which base_palette entry 'palette[a]'
 * maps to, and store that index. This handles mid-frame PAL changes.
 * ====================================================================== */

#define P8_W 128
#define P8_H 128

static uint8_t p8_screen[P8_W * P8_H];

/* Resolve a PICO-8 color through the current palette remap.
 * Returns the base palette index that palette[col] corresponds to. */
static inline uint8_t resolve_color(int col) {
    col &= 0xF;
    uint32_t target = palette[col];
    for (int i = 0; i < 16; i++) {
        if (base_palette[i] == target) return (uint8_t)i;
    }
    return (uint8_t)col;  /* fallback */
}

static inline void p8_pixel(int x, int y, int col) {
    if ((unsigned)x < P8_W && (unsigned)y < P8_H)
        p8_screen[y * P8_W + x] = resolve_color(col);
}

static inline int p8_get_pixel(int x, int y) {
    if ((unsigned)x < P8_W && (unsigned)y < P8_H)
        return p8_screen[y * P8_W + x];
    return 0;
}

/* ======================================================================
 * PICO-8 drawing primitives
 * ====================================================================== */

static void p8_rectfill(int x0, int y0, int x1, int y1, int col) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            p8_pixel(x, y, col);
}

static void p8_line(int x0, int y0, int x1, int y1, int col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        p8_pixel(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void p8_hline(int x0, int x1, int y, int col) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) p8_pixel(x, y, col);
}

static void p8_circfill(int cx, int cy, int r, int col) {
    if (r <= 0) {
        p8_pixel(cx, cy, col);
        return;
    }
    /* Midpoint circle fill (integer only) */
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        p8_hline(cx - x, cx + x, cy + y, col);
        p8_hline(cx - x, cx + x, cy - y, col);
        p8_hline(cx - y, cx + y, cy + x, col);
        p8_hline(cx - y, cx + y, cy - x, col);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void p8_spr_with_pal(int sprite, int x, int y, int flipx, int flipy) {
    int sx = (sprite % 16) * 8;
    int sy = (sprite / 16) * 8;

    for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
            int gx = sx + (flipx ? (7 - px) : px);
            int gy = sy + (flipy ? (7 - py) : py);
            if (gy >= 64) continue;
            uint8_t pixel = gfx_data[gy * 128 + gx];
            if (pixel != 0) {
                /* Palette remapping: find which base color this maps to */
                p8_pixel(x + px, y + py, pixel);
            }
        }
    }
}

static void p8_print(const char *str, int x, int y, int col) {
    for (const char *c = str; *c; c++) {
        int ch = (*c) & 0x7F;
        int fx = (ch % 16) * 8;
        int fy = (ch / 16) * 8;

        for (int py = 0; py < 8; py++) {
            for (int px = 0; px < 4; px++) {
                /* font is 1-bit: pixel is either 0 or 1 */
                if (fy + py < 85 && font_data[(fy + py) * 128 + fx + px])
                    p8_pixel(x + px, y + py, col);
            }
        }
        x += 4;
    }
}

static void p8_map(int mx, int my, int tx, int ty, int mw, int mh, int mask,
                   int camera_x, int camera_y) {
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < mw; x++) {
            int tile = tilemap_data[(x + mx) + (y + my) * 128];
            int draw = 0;
            if (mask == 0) {
                draw = 1;
            } else if (mask == 4 && tile_flags[tile] == 4) {
                draw = 1;
            } else if (mask != 4 && tile < (int)(sizeof(tile_flags)/sizeof(*tile_flags))
                       && (tile_flags[tile] & (1 << (mask - 1)))) {
                draw = 1;
            }
            if (draw) {
                p8_spr_with_pal(tile, tx + x * 8 - camera_x,
                                ty + y * 8 - camera_y, 0, 0);
            }
        }
    }
}

/* ======================================================================
 * Scale 128x128 → framebuffer and present
 * ====================================================================== */

/* Scale factor and offsets for centering */
#define SCREEN_W 320
#define SCREEN_H 240
#define SCALE 2
#define OFS_X ((SCREEN_W - P8_W * SCALE) / 2)   /* (320 - 256) / 2 = 32 */
#define OFS_Y ((SCREEN_H - P8_H * SCALE) / 2)   /* (240 - 256) / 2 = -8 */

static SDL_Window *win;
static SDL_Surface *screen;

static void present(void) {
    screen = SDL_GetWindowSurface(win);  /* refresh after flip */
    uint8_t *fb = (uint8_t *)screen->pixels;
    int pitch = screen->pitch;

    for (int py = 0; py < P8_H; py++) {
        int dy = OFS_Y + py * SCALE;
        for (int s = 0; s < SCALE; s++) {
            int fy = dy + s;
            if ((unsigned)fy >= SCREEN_H) continue;
            for (int px = 0; px < P8_W; px++) {
                uint8_t col = p8_screen[py * P8_W + px];
                int dx = OFS_X + px * SCALE;
                fb[fy * pitch + dx]     = col;
                fb[fy * pitch + dx + 1] = col;
            }
        }
    }
    SDL_UpdateWindowSurface(win);
}

/* ======================================================================
 * Input mapping
 * ====================================================================== */

static uint16_t buttons_state;
static uint8_t key_held[256]; /* indexed by SDL_Scancode */

static void poll_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
            key_held[ev.key.keysym.scancode] = 1;
        else if (ev.type == SDL_KEYUP)
            key_held[ev.key.keysym.scancode] = 0;
    }
}

/* PICO-8 buttons: 0=left, 1=right, 2=up, 3=down, 4=jump(z), 5=dash(x) */
static void read_input(void) {
    poll_events();

    buttons_state = 0;
    if (key_held[SDL_SCANCODE_LEFT])   buttons_state |= (1 << 0);
    if (key_held[SDL_SCANCODE_RIGHT])  buttons_state |= (1 << 1);
    if (key_held[SDL_SCANCODE_UP])     buttons_state |= (1 << 2);
    if (key_held[SDL_SCANCODE_DOWN])   buttons_state |= (1 << 3);
    if (key_held[SDL_SCANCODE_Z])      buttons_state |= (1 << 4);  /* jump */
    if (key_held[SDL_SCANCODE_X])      buttons_state |= (1 << 5);  /* dash */
}

/* ======================================================================
 * PICO-8 callback implementation
 * ====================================================================== */

static int camera_x, camera_y;
static int enable_screenshake = 1;

static int pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...) {
    va_list args;
    int ret = 0;
    va_start(args, call);

    #define   INT_ARG() va_arg(args, int)
    #define  BOOL_ARG() (Celeste_P8_bool_t)va_arg(args, int)

    switch (call) {
    case CELESTE_P8_MUSIC: {
        int index = INT_ARG();
        int fade = INT_ARG();
        int mask = INT_ARG();
        (void)index; (void)fade; (void)mask;
        /* Audio not implemented in this port */
    } break;

    case CELESTE_P8_SPR: {
        int sprite = INT_ARG();
        int x = INT_ARG() - camera_x;
        int y = INT_ARG() - camera_y;
        int cols = INT_ARG();
        int rows = INT_ARG();
        int flipx = BOOL_ARG();
        int flipy = BOOL_ARG();
        (void)cols; (void)rows;
        if (sprite >= 0)
            p8_spr_with_pal(sprite, x, y, flipx, flipy);
    } break;

    case CELESTE_P8_BTN: {
        int b = INT_ARG();
        ret = (buttons_state & (1 << b)) != 0;
    } break;

    case CELESTE_P8_SFX: {
        int id = INT_ARG();
        (void)id;
        /* Audio not implemented */
    } break;

    case CELESTE_P8_PAL: {
        int a = INT_ARG();
        int b = INT_ARG();
        if (a >= 0 && a < 16 && b >= 0 && b < 16)
            palette[a] = base_palette[b];
    } break;

    case CELESTE_P8_PAL_RESET: {
        reset_palette();
    } break;

    case CELESTE_P8_CIRCFILL: {
        int cx = INT_ARG() - camera_x;
        int cy = INT_ARG() - camera_y;
        int r = INT_ARG();
        int col = INT_ARG();
        p8_circfill(cx, cy, r, col);
    } break;

    case CELESTE_P8_PRINT: {
        const char *str = va_arg(args, const char *);
        int x = INT_ARG() - camera_x;
        int y = INT_ARG() - camera_y;
        int col = INT_ARG() % 16;
        p8_print(str, x, y, col);
    } break;

    case CELESTE_P8_RECTFILL: {
        int x0 = INT_ARG() - camera_x;
        int y0 = INT_ARG() - camera_y;
        int x1 = INT_ARG() - camera_x;
        int y1 = INT_ARG() - camera_y;
        int col = INT_ARG();
        p8_rectfill(x0, y0, x1, y1, col);
    } break;

    case CELESTE_P8_LINE: {
        int x0 = INT_ARG() - camera_x;
        int y0 = INT_ARG() - camera_y;
        int x1 = INT_ARG() - camera_x;
        int y1 = INT_ARG() - camera_y;
        int col = INT_ARG();
        p8_line(x0, y0, x1, y1, col);
    } break;

    case CELESTE_P8_MGET: {
        int tx = INT_ARG();
        int ty = INT_ARG();
        ret = tilemap_data[tx + ty * 128];
    } break;

    case CELESTE_P8_CAMERA: {
        if (enable_screenshake) {
            camera_x = INT_ARG();
            camera_y = INT_ARG();
        }
    } break;

    case CELESTE_P8_FGET: {
        int tile = INT_ARG();
        int flag = INT_ARG();
        ret = tile < (int)(sizeof(tile_flags)/sizeof(*tile_flags))
              && (tile_flags[tile] & (1 << flag)) != 0;
    } break;

    case CELESTE_P8_MAP: {
        int mx = INT_ARG(), my = INT_ARG();
        int tx = INT_ARG(), ty = INT_ARG();
        int mw = INT_ARG(), mh = INT_ARG();
        int mask = INT_ARG();
        p8_map(mx, my, tx, ty, mw, mh, mask, camera_x, camera_y);
    } break;
    }

    va_end(args);
    return ret;
}

/* ======================================================================
 * Embedded BMP data (gfx.bmp + font.bmp baked into the binary)
 * ====================================================================== */

#include "gfx_data.h"

static int load_gfx(void) {
    if (load_bmp(gfx_bmp, gfx_bmp_len, gfx_data, 128, 64, 4) < 0)
        return -1;
    if (load_bmp(font_bmp, font_bmp_len, font_data, 128, 85, 1) < 0)
        return -2;
    return 0;
}

/* ======================================================================
 * OSD (on-screen display)
 * ====================================================================== */

static char osd_text[200] = "";
static int osd_timer = 0;

static void osd_draw(void) {
    if (osd_timer > 0) {
        osd_timer--;
        int x = 4;
        int y = 120 + (osd_timer < 10 ? 10 - osd_timer : 0);
        int len = 0;
        for (const char *s = osd_text; *s; s++) len++;
        p8_rectfill(x - 2, y - 2, x + 4 * len, y + 6, 6);
        p8_rectfill(x - 1, y - 1, x + 4 * len - 1, y + 5, 0);
        p8_print(osd_text, x, y, 7);
    }
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    SDL_Init(SDL_INIT_VIDEO);
    win = SDL_CreateWindow("Celeste",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    screen = SDL_GetWindowSurface(win);

    /* Set up PICO-8 palette */
    SDL_Color colors[16];
    for (int i = 0; i < 16; i++) {
        colors[i].r = (base_palette[i] >> 16) & 0xFF;
        colors[i].g = (base_palette[i] >> 8) & 0xFF;
        colors[i].b = base_palette[i] & 0xFF;
        colors[i].a = 0xFF;
    }
    SDL_SetPaletteColors(screen->format->palette, colors, 0, 16);

    /* Load spritesheet and font */
    printf("Loading Celeste...\n");

    if (load_gfx() < 0) {
        printf("Error loading graphics!\n");
        printf("Place gfx.bmp and font.bmp\n");
        printf("in data/ directory.\n");
        while (1) { SDL_Delay(100); }
    }

    printf("OK!\n");

    /* Initialize game */
    reset_palette();
    Celeste_P8_set_call_func(pico8emu);
    Celeste_P8_set_rndseed(SDL_GetTicks());
    Celeste_P8_init();

    /* Save initial state for reset */
    void *initial_state = malloc(Celeste_P8_get_state_size());
    if (initial_state)
        Celeste_P8_save_state(initial_state);

    /* Game loop (30fps) */
    while (1) {
        uint32_t frame_start = SDL_GetTicks();

        read_input();

        /* Check for reset (hold SELECT+START for 1 second) */
        {
            static int reset_timer = 0;
            if (key_held[SDL_SCANCODE_RSHIFT] && key_held[SDL_SCANCODE_RETURN]) {
                reset_timer++;
                if (reset_timer >= 30 && initial_state) {
                    reset_timer = 0;
                    Celeste_P8_load_state(initial_state);
                    Celeste_P8_set_rndseed(SDL_GetTicks());
                    Celeste_P8_init();
                    memcpy(osd_text, "reset", 6);
                    osd_timer = 30;
                }
            } else {
                reset_timer = 0;
            }
        }

        /* Update and draw */
        Celeste_P8_update();
        Celeste_P8_draw();
        osd_draw();

        /* Clear framebuffer and blit scaled PICO-8 screen */
        SDL_FillRect(screen, NULL, 0);
        present();

        /* Frame timing: ~33ms per frame for 30fps */
        uint32_t elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < 33)
            SDL_Delay(33 - elapsed);
    }

    return 0;
}
