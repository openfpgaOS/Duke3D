/*
 * openfpgaOS Framebuffer Demo
 * Loads an indexed PNG image and displays it.
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#include "of.h"
#include "png.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define IMAGE_SLOT_ID 3
#define MAX_PNG_SIZE (128 * 1024)

static uint8_t png_buf[MAX_PNG_SIZE];
static uint8_t pixel_buf[SCREEN_W * SCREEN_H + SCREEN_H + MAX_PNG_SIZE];
static uint32_t raw_palette[256];

/* Determine actual PNG file size by walking chunk headers */
static int png_file_size(uint32_t *size_out) {
    int rc = of_file_read(IMAGE_SLOT_ID, 0, png_buf, 8);
    if (rc < 0) return rc;

    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    if (memcmp(png_buf, sig, 8) != 0) return -1;

    uint32_t pos = 8;
    while (pos < MAX_PNG_SIZE) {
        rc = of_file_read(IMAGE_SLOT_ID, 0, png_buf, pos + 8);
        if (rc < 0) return rc;

        uint32_t chunk_len = png__be32(png_buf + pos);
        if (memcmp(png_buf + pos + 4, "IEND", 4) == 0) {
            *size_out = pos + 12;
            return 0;
        }
        pos += 12 + chunk_len;
    }
    return -2;
}

int main(void) {
    printf("Loading image...\n");

    uint32_t file_size = 0;
    int rc = png_file_size(&file_size);
    if (rc < 0 || file_size == 0 || file_size > MAX_PNG_SIZE) {
        printf("Error reading PNG: rc=%d\n", rc);
        while (1) { of_delay_ms(100); }
    }
    printf("PNG size: %d bytes\n", (int)file_size);

    rc = of_file_read(IMAGE_SLOT_ID, 0, png_buf, file_size);
    if (rc < 0) {
        printf("Error loading PNG: rc=%d\n", rc);
        while (1) { of_delay_ms(100); }
    }

    int w, h;
    rc = png_decode(png_buf, file_size, raw_palette, pixel_buf, &w, &h);
    if (rc < 0) {
        printf("PNG decode error: %d\n", rc);
        while (1) { of_delay_ms(100); }
    }
    printf("Decoded %dx%d\n", w, h);

    /* Switch to framebuffer mode */
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("fbdemo",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    SDL_Surface *screen = SDL_GetWindowSurface(win);

    /* Set palette */
    SDL_Color colors[256];
    for (int i = 0; i < 256; i++) {
        colors[i].r = (raw_palette[i] >> 16) & 0xFF;
        colors[i].g = (raw_palette[i] >> 8) & 0xFF;
        colors[i].b = raw_palette[i] & 0xFF;
        colors[i].a = 0xFF;
    }
    SDL_SetPaletteColors(screen->format->palette, colors, 0, 256);

    /* Clear and blit image centered */
    SDL_FillRect(screen, NULL, 0);

    int ox = (SCREEN_W - w) / 2;
    int oy = (SCREEN_H - h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    int copy_w = w < SCREEN_W ? w : SCREEN_W;
    int copy_h = h < SCREEN_H ? h : SCREEN_H;
    uint8_t *fb = (uint8_t *)screen->pixels;
    for (int y = 0; y < copy_h; y++)
        memcpy(&fb[(oy + y) * screen->pitch + ox], &pixel_buf[y * w], copy_w);

    SDL_UpdateWindowSurface(win);

    /* Event loop */
    SDL_Event ev;
    while (1) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                goto done;
        }
        SDL_Delay(16);
    }

done:
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
