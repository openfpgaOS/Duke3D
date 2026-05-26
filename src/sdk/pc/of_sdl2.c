/*
 * of_sdl2.c -- SDL2 backend for openfpgaOS Application API
 *
 * Implements the full of.h API using SDL2 so games can be built
 * and tested on a PC (Linux/macOS/Windows).
 *
 * Build: cc -DOF_PC app.c of_sdl2.c $(sdl2-config --cflags --libs) -lm
 */

#ifndef OF_PC
#define OF_PC
#endif
#define OF_NO_COMPAT
#include "of.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ======================================================================
 * Internal state
 * ====================================================================== */

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;

/* Double-buffered 320x240 framebuffers, sized for the largest color mode. */
static uint8_t  g_fb[2][OF_FB_SIZE_16BPP];
static int      g_draw_buf;    /* index of current draw buffer */
static int      g_color_mode;
static uint32_t g_present_count;
static uint64_t g_last_present_us;

/* Palette: 256 entries, 0x00RRGGBB */
static uint32_t g_palette[256];

/* Composited ARGB output (uploaded to texture) */
static uint32_t g_pixels[OF_SCREEN_W * OF_SCREEN_H];

/* ---- Tile engine state ---- */
static int      g_tile_enabled;
static int      g_tile_priority;  /* 0=behind FB, 1=over FB */
static int      g_tile_scroll_x;
static int      g_tile_scroll_y;
static uint16_t g_tilemap[64 * 32];          /* 64 cols x 32 rows */
static uint32_t g_tile_chr[256 * 8];         /* 256 tiles x 8 rows x 32-bit */

/* ---- Sprite engine state ---- */
#define MAX_SPRITES 64

typedef struct {
    int16_t  x, y;
    uint8_t  tile_id;
    uint8_t  palette;
    uint8_t  hflip, vflip;
    uint8_t  enabled;
} sprite_t;

static int      g_sprite_enabled;
static sprite_t g_sprites[MAX_SPRITES];
static uint32_t g_sprite_chr[256 * 8];       /* same format as tile chr */

/* ---- Input state ---- */
static of_input_state_t g_input[2];
static uint32_t g_prev_buttons[2];

/* ---- Audio state ---- */
static SDL_AudioDeviceID g_audio_dev;
#define AUDIO_BUF_SIZE 8192
static int16_t  g_audio_ring[AUDIO_BUF_SIZE];
static int      g_audio_read_pos;
static int      g_audio_write_pos;
static SDL_mutex *g_audio_mutex;

/* ---- Timer ---- */
static uint64_t g_start_us;

static const struct of_capabilities g_caps = {
    .magic = OF_CAPS_MAGIC,
    .version = OF_CAPS_VERSION,
    .fb_width = OF_SCREEN_W,
    .fb_height = OF_SCREEN_H,
    .fb_stride = OF_SCREEN_W,
    .fb_size = OF_FB_SIZE_8BIT,
    .hw_features = OF_HW_MIXER | OF_HW_SAVE_SLOTS,
    .mixer_voices = OF_MIXER_MAX_VOICES,
    .mixer_rate = OF_AUDIO_RATE,
    .platform_id = OF_PLATFORM_SIM,
    .cpu_freq_hz = 100000000u,
};

static uint64_t get_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

const struct of_capabilities *of_get_caps(void) {
    return &g_caps;
}

int of_has_feature(uint32_t feature) {
    return (g_caps.hw_features & feature) != 0;
}

/* ======================================================================
 * Video
 * ====================================================================== */

/* Render tile layer into a scanline buffer (palette indices) */
static void render_tile_scanline(uint8_t *line, int y) {
    int sy = (y + g_tile_scroll_y) & 0xFF;  /* 256 pixel wrap (32 tiles * 8) */
    int tile_row = sy >> 3;
    int fine_y   = sy & 7;

    for (int x = 0; x < OF_SCREEN_W; x++) {
        int sx = (x + g_tile_scroll_x) & 0x1FF;  /* 512 pixel wrap (64 * 8) */
        int tile_col = sx >> 3;
        int fine_x   = sx & 7;

        uint16_t entry = g_tilemap[tile_row * 64 + tile_col];
        int tile_id = entry & 0xFF;
        int pal     = (entry >> 10) & 0xF;
        int hflip   = (entry >> 14) & 1;
        int vflip   = (entry >> 15) & 1;

        int row = vflip ? (7 - fine_y) : fine_y;
        int col = hflip ? (7 - fine_x) : fine_x;

        uint32_t chr_word = g_tile_chr[tile_id * 8 + row];
        int nibble = (chr_word >> (col * 4)) & 0xF;

        line[x] = nibble ? ((pal << 4) | nibble) : 0;
    }
}

/* Render all sprites into a scanline buffer (palette indices) */
static void render_sprite_scanline(uint8_t *line, int y) {
    memset(line, 0, OF_SCREEN_W);

    /* Back to front for correct priority (sprite 0 = highest) */
    for (int i = MAX_SPRITES - 1; i >= 0; i--) {
        sprite_t *s = &g_sprites[i];
        if (!s->enabled) continue;

        int sy = y - s->y;
        if (sy < 0 || sy >= 8) continue;

        int row = s->vflip ? (7 - sy) : sy;
        uint32_t chr_word = g_sprite_chr[s->tile_id * 8 + row];

        for (int px = 0; px < 8; px++) {
            int col = s->hflip ? (7 - px) : px;
            int nibble = (chr_word >> (col * 4)) & 0xF;
            if (nibble == 0) continue;

            int screen_x = s->x + px;
            if (screen_x < 0 || screen_x >= OF_SCREEN_W) continue;

            line[screen_x] = (s->palette << 4) | nibble;
        }
    }
}

static uint32_t rgb565_to_argb(uint16_t v) {
    uint32_t r = (uint32_t)((v >> 11) & 0x1F);
    uint32_t g = (uint32_t)((v >> 5) & 0x3F);
    uint32_t b = (uint32_t)(v & 0x1F);
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t rgb555_to_argb(uint16_t v) {
    uint32_t r = (uint32_t)((v >> 10) & 0x1F);
    uint32_t g = (uint32_t)((v >> 5) & 0x1F);
    uint32_t b = (uint32_t)(v & 0x1F);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t rgba5551_to_argb(uint16_t v) {
    uint32_t r = (uint32_t)((v >> 11) & 0x1F);
    uint32_t g = (uint32_t)((v >> 6) & 0x1F);
    uint32_t b = (uint32_t)((v >> 1) & 0x1F);
    uint32_t a = (v & 1) ? 0xFF000000u : 0x00000000u;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return a | (r << 16) | (g << 8) | b;
}

static uint8_t framebuffer_index_at(const uint8_t *fb, int x, int y) {
    switch (g_color_mode) {
    case OF_VIDEO_MODE_4BIT: {
        uint8_t packed = fb[y * (OF_SCREEN_W / 2) + (x >> 1)];
        return (x & 1) ? (packed >> 4) : (packed & 0x0F);
    }
    case OF_VIDEO_MODE_2BIT: {
        uint8_t packed = fb[y * (OF_SCREEN_W / 4) + (x >> 2)];
        return (packed >> ((x & 3) * 2)) & 0x03;
    }
    default:
        return fb[y * OF_SCREEN_W + x];
    }
}

/* Composite all layers and upload to texture */
static void composite_and_present(void) {
    int disp = g_draw_buf ^ 1;  /* display buffer is the one we just flipped from */
    const uint8_t *fb = g_fb[disp];
    const uint16_t *fb16 = (const uint16_t *)fb;
    uint8_t tile_line[OF_SCREEN_W];
    uint8_t sprite_line[OF_SCREEN_W];

    for (int y = 0; y < OF_SCREEN_H; y++) {
        if (g_tile_enabled)
            render_tile_scanline(tile_line, y);
        if (g_sprite_enabled)
            render_sprite_scanline(sprite_line, y);

        for (int x = 0; x < OF_SCREEN_W; x++) {
            uint32_t color = 0xFF000000;  /* opaque black */

            if (g_color_mode == OF_VIDEO_MODE_RGB565) {
                color = rgb565_to_argb(fb16[y * OF_SCREEN_W + x]);
            } else if (g_color_mode == OF_VIDEO_MODE_RGB555) {
                color = rgb555_to_argb(fb16[y * OF_SCREEN_W + x]);
            } else if (g_color_mode == OF_VIDEO_MODE_RGBA5551) {
                color = rgba5551_to_argb(fb16[y * OF_SCREEN_W + x]);
            } else {
                uint8_t fb_idx = framebuffer_index_at(fb, x, y);
                if (fb_idx && g_palette[fb_idx])
                    color = g_palette[fb_idx] | 0xFF000000;
            }

            /* Compositing: sprite > tile(hi) > FB > tile(lo) > black */
            if (g_sprite_enabled && sprite_line[x]) {
                color = g_palette[sprite_line[x]] | 0xFF000000;
            } else if (g_tile_enabled && g_tile_priority && tile_line[x]) {
                color = g_palette[tile_line[x]] | 0xFF000000;
            } else if (g_tile_enabled && !g_tile_priority && tile_line[x]) {
                color = g_palette[tile_line[x]] | 0xFF000000;
            }

            g_pixels[y * OF_SCREEN_W + x] = color;
        }
    }

    SDL_UpdateTexture(g_texture, NULL, g_pixels, OF_SCREEN_W * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    g_present_count++;
    g_last_present_us = get_us();
}

void of_video_init(void) {
    if (!g_window) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            exit(1);
        }
        g_start_us = get_us();

        g_window = SDL_CreateWindow("openfpgaOS",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            OF_SCREEN_W * 3, OF_SCREEN_H * 3,
            SDL_WINDOW_RESIZABLE);
        g_renderer = SDL_CreateRenderer(g_window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(g_renderer, OF_SCREEN_W, OF_SCREEN_H);
        g_texture = SDL_CreateTexture(g_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            OF_SCREEN_W, OF_SCREEN_H);
    }

    memset(g_fb, 0, sizeof(g_fb));
    g_draw_buf = 0;
    g_color_mode = OF_VIDEO_MODE_8BIT;
    memset(g_palette, 0, sizeof(g_palette));
}

uint8_t *of_video_surface(void) {
    return g_fb[g_draw_buf];
}

void of_video_flip(void) {
    composite_and_present();
    g_draw_buf ^= 1;
}

void of_video_wait_flip(void) {
    /* vsync is handled by SDL_RENDERER_PRESENTVSYNC */
}

int of_video_acquire_next(int just_flipped_idx, uint32_t fence_token) {
    (void)just_flipped_idx;
    (void)fence_token;
    return g_draw_buf;
}

uint8_t *of_video_buffer_addr(int idx) {
    if (idx < 0)
        idx = g_draw_buf;
    return g_fb[idx & 1];
}

void of_video_clear(uint8_t color) {
    size_t size = OF_FB_SIZE_8BIT;
    if (g_color_mode == OF_VIDEO_MODE_4BIT)
        size = OF_FB_SIZE_4BIT;
    else if (g_color_mode == OF_VIDEO_MODE_2BIT)
        size = OF_FB_SIZE_2BIT;
    else if (g_color_mode >= OF_VIDEO_MODE_RGB565)
        size = OF_FB_SIZE_16BPP;
    memset(g_fb[g_draw_buf], color, size);
}

void of_video_palette(uint8_t index, uint32_t rgb) {
    g_palette[index] = rgb;
}

void of_video_palette_bulk(const uint32_t *pal, int count) {
    if (count > 256) count = 256;
    memcpy(g_palette, pal, count * sizeof(uint32_t));
}

void of_video_flush(void) {
    /* no-op on PC */
}

void of_video_set_display_mode(int mode) {
    (void)mode;
}

void of_video_set_color_mode(int mode) {
    if (mode < OF_VIDEO_MODE_8BIT || mode > OF_VIDEO_MODE_RGBA5551)
        mode = OF_VIDEO_MODE_8BIT;
    g_color_mode = mode;
}

void of_video_get_timing(of_video_timing_t *out) {
    if (!out) return;
    out->vblank_count = g_present_count;
    out->present_count = g_present_count;
    out->last_presented_idx = (uint32_t)(g_draw_buf ^ 1);
    out->reserved = 0;
    out->last_vblank_us = g_last_present_us;
    out->last_flip_presented_us = g_last_present_us;
}

uint64_t of_video_last_vblank_us(void) {
    return g_last_present_us;
}

uint64_t of_video_last_flip_presented_us(void) {
    return g_last_present_us;
}

uint32_t of_video_vblank_count(void) {
    return g_present_count;
}

/* ======================================================================
 * Timer
 *
 * of_timer.h declares of_time_us / of_time_ms as plain externs in the
 * OF_PC build (no inline syscalls). The PC backend implements them
 * here on top of SDL_GetPerformanceCounter / SDL_GetTicks. Both are
 * monotonic and free-running, matching the on-target semantics.
 * ====================================================================== */

unsigned int of_time_us(void) {
    static Uint64 freq;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    Uint64 ticks = SDL_GetPerformanceCounter();
    /* Scale to microseconds. Wraps in ~71 minutes (uint32 us), same
     * cadence as the hardware free-running timer. */
    return (unsigned int)((ticks * 1000000ULL) / freq);
}

unsigned int of_time_ms(void) {
    return (unsigned int)SDL_GetTicks();
}

/* ======================================================================
 * Input
 * ====================================================================== */

/* SDL scancode -> button mask mapping */
static uint32_t key_to_btn(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP:     return OF_BTN_UP;
        case SDL_SCANCODE_DOWN:   return OF_BTN_DOWN;
        case SDL_SCANCODE_LEFT:   return OF_BTN_LEFT;
        case SDL_SCANCODE_RIGHT:  return OF_BTN_RIGHT;
        case SDL_SCANCODE_Z:      return OF_BTN_A;
        case SDL_SCANCODE_X:      return OF_BTN_B;
        case SDL_SCANCODE_A:      return OF_BTN_X;
        case SDL_SCANCODE_S:      return OF_BTN_Y;
        case SDL_SCANCODE_Q:      return OF_BTN_L1;
        case SDL_SCANCODE_W:      return OF_BTN_R1;
        case SDL_SCANCODE_1:      return OF_BTN_L2;
        case SDL_SCANCODE_2:      return OF_BTN_R2;
        case SDL_SCANCODE_RSHIFT: return OF_BTN_SELECT;
        case SDL_SCANCODE_RETURN: return OF_BTN_START;
        default: return 0;
    }
}

void of_input_poll(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            SDL_Quit();
            exit(0);
            break;
        case SDL_KEYDOWN:
            if (!ev.key.repeat)
                g_input[0].buttons |= key_to_btn(ev.key.keysym.scancode);
            break;
        case SDL_KEYUP:
            g_input[0].buttons &= ~key_to_btn(ev.key.keysym.scancode);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            /* PC backend folds all SDL controllers into player 0; the
             * P2 button query stubs above always read empty state. */
            int p = 0;
            uint32_t mask = 0;
            switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:    mask = OF_BTN_UP; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  mask = OF_BTN_DOWN; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  mask = OF_BTN_LEFT; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: mask = OF_BTN_RIGHT; break;
                case SDL_CONTROLLER_BUTTON_A:          mask = OF_BTN_A; break;
                case SDL_CONTROLLER_BUTTON_B:          mask = OF_BTN_B; break;
                case SDL_CONTROLLER_BUTTON_X:          mask = OF_BTN_X; break;
                case SDL_CONTROLLER_BUTTON_Y:          mask = OF_BTN_Y; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  mask = OF_BTN_L1; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: mask = OF_BTN_R1; break;
                case SDL_CONTROLLER_BUTTON_BACK:       mask = OF_BTN_SELECT; break;
                case SDL_CONTROLLER_BUTTON_START:      mask = OF_BTN_START; break;
                default: break;
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN)
                g_input[p].buttons |= mask;
            else
                g_input[p].buttons &= ~mask;
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(ev.cdevice.which);
            break;
        }
    }

    /* Compute pressed/released edges */
    for (int p = 0; p < 2; p++) {
        g_input[p].buttons_pressed  = g_input[p].buttons & ~g_prev_buttons[p];
        g_input[p].buttons_released = ~g_input[p].buttons & g_prev_buttons[p];
        g_prev_buttons[p] = g_input[p].buttons;
    }
}

int of_btn(uint32_t mask)          { return (g_input[0].buttons & mask) != 0; }
int of_btn_pressed(uint32_t mask)  { return (g_input[0].buttons_pressed & mask) != 0; }
int of_btn_released(uint32_t mask) { return (g_input[0].buttons_released & mask) != 0; }
int of_btn_p2(uint32_t mask)           { return (g_input[1].buttons & mask) != 0; }
int of_btn_pressed_p2(uint32_t mask)   { return (g_input[1].buttons_pressed & mask) != 0; }
int of_btn_released_p2(uint32_t mask)  { return (g_input[1].buttons_released & mask) != 0; }

uint32_t of_input_state(int player, of_input_state_t *state) {
    if (player >= 0 && player < 2 && state)
        *state = g_input[player];
    return 0;
}

/* Keyboard/mouse/deadzone stubs — declared as plain externs in
 * of_input.h's OF_PC branch.  The PC backend doesn't expose dock
 * peripherals through SDL, so return empty state and accept the
 * deadzone for API compatibility. */
void of_input_keyboard_state(of_keyboard_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void of_input_mouse_state(of_mouse_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void of_input_set_deadzone(int16_t deadzone) {
    (void)deadzone;
}

/* ======================================================================
 * Audio
 * ====================================================================== */

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int samples = len / 2;

    SDL_LockMutex(g_audio_mutex);
    for (int i = 0; i < samples; i++) {
        if (g_audio_read_pos != g_audio_write_pos) {
            out[i] = g_audio_ring[g_audio_read_pos];
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUF_SIZE;
        } else {
            out[i] = 0;
        }
    }
    SDL_UnlockMutex(g_audio_mutex);
}

void of_audio_init(void) {
    if (g_audio_dev) return;

    g_audio_mutex = SDL_CreateMutex();

    SDL_AudioSpec want = {0}, have;
    want.freq = OF_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev)
        SDL_PauseAudioDevice(g_audio_dev, 0);
}

int of_audio_write(const int16_t *samples, int count) {
    int written = 0;
    SDL_LockMutex(g_audio_mutex);
    for (int i = 0; i < count * 2; i++) {
        int next = (g_audio_write_pos + 1) % AUDIO_BUF_SIZE;
        if (next == g_audio_read_pos) break;
        g_audio_ring[g_audio_write_pos] = samples[i];
        g_audio_write_pos = next;
        if (i & 1) written++;
    }
    SDL_UnlockMutex(g_audio_mutex);
    return written;
}

int of_audio_free(void) {
    SDL_LockMutex(g_audio_mutex);
    int used = (g_audio_write_pos - g_audio_read_pos + AUDIO_BUF_SIZE) % AUDIO_BUF_SIZE;
    SDL_UnlockMutex(g_audio_mutex);
    return (AUDIO_BUF_SIZE - 1 - used) / 2;
}

/* ======================================================================
 * Timer
 * ====================================================================== */


/* ======================================================================
 * File I/O
 *
 * On PC, data slot files are read from ./data/<slot_id>.bin
 * Set OF_DATA_DIR env var to override.
 * ====================================================================== */

int of_file_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length) {
    char path[512];
    const char *dir = getenv("OF_DATA_DIR");
    if (!dir) dir = "data";
    snprintf(path, sizeof(path), "%s/%u.bin", dir, slot_id);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, offset, SEEK_SET);
    int n = fread(dest, 1, length, f);
    fclose(f);
    return n == (int)length ? 0 : -1;
}

long of_file_size(uint32_t slot_id) {
    char path[512];
    const char *dir = getenv("OF_DATA_DIR");
    if (!dir) dir = "data";
    snprintf(path, sizeof(path), "%s/%u.bin", dir, slot_id);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* ======================================================================
 * Link Cable (stubs)
 * ====================================================================== */

int      of_link_send(uint32_t data)   { (void)data; return -1; }
int      of_link_recv(uint32_t *data)  { (void)data; return -1; }
uint32_t of_link_status(void)          { return 0; }


/* ======================================================================
 * Analogizer (stubs)
 * ====================================================================== */

int of_analogizer_enabled(void)                     { return 0; }
int of_analogizer_state(of_analogizer_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
    return 0;
}

/* ======================================================================
 * Tile Layer
 * ====================================================================== */

void of_tile_enable(int enable, int priority) {
    g_tile_enabled  = enable;
    g_tile_priority = priority;
}

void of_tile_scroll(int x, int y) {
    g_tile_scroll_x = x;
    g_tile_scroll_y = y;
}

void of_tile_set(int col, int row, uint16_t entry) {
    if ((unsigned)col < 64 && (unsigned)row < 32)
        g_tilemap[row * 64 + col] = entry;
}

void of_tile_load_map(const uint16_t *data, int x, int y, int w, int h) {
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            of_tile_set(x + c, y + r, data[r * w + c]);
}

void of_tile_load_chr(int first_tile, const void *data, int num_tiles) {
    int words = num_tiles * 8;
    int offset = first_tile * 8;
    if (offset + words > 256 * 8) words = 256 * 8 - offset;
    memcpy(&g_tile_chr[offset], data, words * sizeof(uint32_t));
}

/* ======================================================================
 * Sprite Engine
 * ====================================================================== */

void of_sprite_enable(int enable) {
    g_sprite_enabled = enable;
}

void of_sprite_set(int index, int x, int y, int tile_id, int palette,
                   int hflip, int vflip, int enable) {
    if ((unsigned)index >= MAX_SPRITES) return;
    sprite_t *s = &g_sprites[index];
    s->x       = (int16_t)x;
    s->y       = (int16_t)y;
    s->tile_id = (uint8_t)tile_id;
    s->palette = (uint8_t)palette;
    s->hflip   = (uint8_t)hflip;
    s->vflip   = (uint8_t)vflip;
    s->enabled = (uint8_t)enable;
}

void of_sprite_move(int index, int x, int y) {
    if ((unsigned)index >= MAX_SPRITES) return;
    g_sprites[index].x = (int16_t)x;
    g_sprites[index].y = (int16_t)y;
}

void of_sprite_load_chr(int first_tile, const void *data, int num_tiles) {
    int words = num_tiles * 8;
    int offset = first_tile * 8;
    if (offset + words > 256 * 8) words = 256 * 8 - offset;
    memcpy(&g_sprite_chr[offset], data, words * sizeof(uint32_t));
}

void of_sprite_hide(int index) {
    if ((unsigned)index < MAX_SPRITES)
        g_sprites[index].enabled = 0;
}

void of_sprite_hide_all(void) {
    for (int i = 0; i < MAX_SPRITES; i++)
        g_sprites[i].enabled = 0;
}

/* ======================================================================
 * System
 * ====================================================================== */

void of_exit(void) {
    if (g_audio_dev) SDL_CloseAudioDevice(g_audio_dev);
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
    exit(0);
}
