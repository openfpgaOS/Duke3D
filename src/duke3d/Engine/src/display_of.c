/*
 * display_of.c -- openfpgaOS display/input/timer shim for Duke Nukem 3D
 *
 * Replaces the SDL2 display.c with calls to the openfpgaOS API (of_*).
 * Targets openfpgaOS: 320x240 indexed framebuffer, gamepad input.
 *
 * BUILD engine renders into a 320x200 uint8_t buffer. We blit that into the
 * 320x240 hardware surface centered vertically (20-pixel black bars top/bottom).
 */

#ifdef OPENFPGA

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "build.h"
#include "display.h"
#include "fixedPoint_math.h"
#include "../../d3d_audio.h"
#include "of_midi.h"
#include "engine.h"
#include "draw.h"
#include "cache.h"

#include "of.h"

/* ======================================================================
 * Internal state
 * ====================================================================== */

#define OF_DISPLAY_W  320
#define OF_DISPLAY_H  200
#define OF_DISPLAY_HW_H 240
#define OF_DISPLAY_BAR_H ((OF_DISPLAY_HW_H - OF_DISPLAY_H) / 2)  /* 20px letterbox */
/* Variables required by the engine (declared extern in display.h / build.h) */
int32_t xres, yres, bytesperline, imageSize, maxpages;
uint8_t *frameplace;
uint8_t *frameoffset;
uint8_t *screen, vesachecked;
int32_t buffermode, origbuffermode, linearmode;
uint8_t permanentupdate = 0, vgacompatible;

/* Fallback framebuffer for pre-video-init and 2D editor drawing.
 * Once video is up, BUILD renders directly into the HW back buffer. */
static uint8_t of_framebuffer[OF_DISPLAY_W * OF_DISPLAY_H];

/* Triple-buffer bar-clearing tracker: counts down from 3 so each
 * of the 3 HW buffers gets its letterbox bars cleared exactly once. */
static int bars_remaining = 3;

/* Input state */
static int32_t mouse_relative_x = 0;
static int32_t mouse_relative_y = 0;
static short   mouse_buttons = 0;
static unsigned int lastkey = 0;

/* Saved previous button state for edge detection */
static uint32_t prev_buttons = 0;

/* Drawing helper */
static uint8_t drawpixel_color = 0;

/* Cached palette in 0x00RRGGBB format for of_video_palette_bulk */
static uint32_t of_palette[256];

/* Analog stick dead zone (out of +/-32767) */
#define STICK_DEADZONE  8000
#define STICK_MOUSE_SCALE  6   /* right stick -> mouse sensitivity */

/* ======================================================================
 * Interact menu variables (Pocket menu -> SDRAM)
 *
 * Index 0 = Run Mode:  0 = Always Run, 1 = Hold Y = Run, 2 = Walk
 * ====================================================================== */
#define INTERACT_RUN_MODE  0

#define RUN_MODE_ALWAYS    0
#define RUN_MODE_USE_RUN   1
#define RUN_MODE_WALK      2

int current_run_mode = RUN_MODE_ALWAYS;  /* read by game.c to sync ud.auto_run */

/* ======================================================================
 * Button-to-scancode mapping
 *
 * Each entry: { of button mask, DOS scancode, extended prefix (0 if none) }
 * ====================================================================== */

typedef struct {
    uint32_t btn;
    uint8_t  scancode;
    uint8_t  extended;  /* non-zero means send 0xE0 prefix first */
} btn_map_t;

static const btn_map_t button_map[] = {
    /* Action buttons */
    { OF_BTN_A,      0x1D, 0x00 },  /* A      -> LCtrl  = Fire         */
    { OF_BTN_B,      0x39, 0x00 },  /* B      -> Space  = Open/Use     */
    { OF_BTN_X,      0x1E, 0x00 },  /* X      -> 'A'    = Jump         */
    { OF_BTN_Y,      0x2C, 0x00 },  /* Y      -> 'Z'    = Crouch       */

    /* Shoulders = weapon cycling */
    { OF_BTN_L1,     0x27, 0x00 },  /* L      -> ';'    = Prev Weapon  */
    { OF_BTN_R1,     0x28, 0x00 },  /* R      -> '''    = Next Weapon  */

    /* Menu */
    { OF_BTN_START,  0x01, 0x00 },  /* Start  -> Escape = Menu         */
    { OF_BTN_SELECT, 0x0F, 0x00 },  /* Select -> Tab    = Map          */

    /* D-pad = movement/turning */
    { OF_BTN_UP,     0x48, 0xE0 },  /* D-Up   -> Up     = Forward      */
    { OF_BTN_DOWN,   0x50, 0xE0 },  /* D-Down -> Down   = Backward     */
    { OF_BTN_LEFT,   0x4B, 0xE0 },  /* D-Left -> Left   = Turn Left    */
    { OF_BTN_RIGHT,  0x4D, 0xE0 },  /* D-Right-> Right  = Turn Right   */
};

#define NUM_BTN_MAPS  (sizeof(button_map) / sizeof(button_map[0]))

/* Left-stick virtual keys (W/A/S/D mapped to arrow keys for movement) */
#define SC_UP     0x48
#define SC_DOWN   0x50
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_EXT    0xE0

static uint8_t lstick_up_held    = 0;
static uint8_t lstick_down_held  = 0;
static uint8_t lstick_left_held  = 0;
static uint8_t lstick_right_held = 0;

/* ======================================================================
 * Helpers
 * ====================================================================== */

static void send_key(uint8_t scancode, uint8_t extended, int pressed)
{
    if (extended) {
        lastkey = extended;
        keyhandler();
    }

    lastkey = pressed ? scancode : (scancode + 128);
    keyhandler();
}


/* ======================================================================
 * VIDEO
 * ====================================================================== */

static uint8_t screenalloctype = 255;

static void init_new_res_vars(void)
{
    int i, j;

    setupmouse();

    xdim = xres = OF_DISPLAY_W;
    ydim = yres = OF_DISPLAY_H;

    numpages = 1;  /* perm overhead handled by periodic forced redraws instead */
    bytesperline = OF_DISPLAY_W;
    vesachecked = 1;
    vgacompatible = 1;
    linearmode = 1;
    qsetmode = OF_DISPLAY_H;
    activepage = visualpage = 0;

    /* Default to static buffer; retarget_frameplace() overrides once video is up */
    frameoffset = frameplace = of_framebuffer;

    if (screen != NULL) {
        if (screenalloctype == 0) free((void *)screen);
        if (screenalloctype == 1) suckcache((int32_t *)screen);
        screen = NULL;
    }

    /* horizlookup tables */
    j = ydim * 4 * sizeof(int32_t);
    if (horizlookup)  free(horizlookup);
    if (horizlookup2) free(horizlookup2);
    horizlookup  = (int32_t *)malloc(j);
    horizlookup2 = (int32_t *)malloc(j);

    /* Build ylookup table: screen-space Y -> framebuffer byte offset */
    j = 0;
    for (i = 0; i <= ydim; i++) {
        ylookup[i] = j;
        j += bytesperline;
    }

    horizycent = ((ydim * 4) >> 1);

    /* Force engine to recalculate aspect ratio */
    oxyaspect = oxdimen = oviewingrange = -1;

    setBytesPerLine(bytesperline);
    setview(0L, 0L, xdim - 1, ydim - 1);
    setbrightness(curbrightness, palette);

    if (searchx < 0) {
        searchx = halfxdimen;
        searchy = (ydimen >> 1);
    }
}

void *get_framebuffer(void)
{
    return of_framebuffer;
}

/* Point BUILD's rendering target at the given HW back buffer. */
static void retarget_frameplace_at(uint8_t *dst)
{
    if (dst) {
        uint8_t *render_base = dst + OF_DISPLAY_W * OF_DISPLAY_BAR_H;
        frameplace = render_base;
        extern int32_t viewoffset;
        frameoffset = render_base + viewoffset;
    }
}

static void retarget_frameplace(void)
{
    retarget_frameplace_at(of_video_surface());
}

static int video_initialized = 0;

static void ensure_video_init(void) {
    if (!video_initialized) {
        video_initialized = 1;
        of_video_init();
        of_video_set_display_mode(OF_DISPLAY_OVERLAY);
        of_video_palette_bulk(of_palette, 256);
        /* Clear all 3 triple-buffer frames */
        of_video_clear(0); of_video_flip();
        of_video_clear(0); of_video_flip();
        of_video_clear(0);
        /* Point BUILD at the HW back buffer from now on */
        bars_remaining = 3;
        retarget_frameplace();
    }
}

void _platform_init(int argc, char **argv, const char *title, const char *iconName)
{
    (void)title;
    (void)iconName;

    _argc = argc;
    _argv = argv;

    /* Don't init video yet — keep terminal visible for debug.
     * Video will init on first _nextpage() call. */

    /* Seed random from timer */
    srand((unsigned int)of_time_ms());

    /* Zero out the internal framebuffer */
    memset(of_framebuffer, 0, sizeof(of_framebuffer));
}

uint32_t np_input_us, np_flip_us, np_audio_us;

void _nextpage(void)
{
    ensure_video_init();

    uint32_t _a = of_time_us();
    _handle_events();
    uint32_t _b = of_time_us();

    /* BUILD already rendered into the HW back buffer (via frameplace).
     * Clear letterbox bars only until all 3 triple-buffer slots are done. */
    if (bars_remaining > 0) {
        uint8_t *dst = of_video_surface();
        if (dst) {
            memset(dst, 0, OF_DISPLAY_W * OF_DISPLAY_BAR_H);
            memset(dst + OF_DISPLAY_W * (OF_DISPLAY_BAR_H + OF_DISPLAY_H), 0,
                   OF_DISPLAY_W * OF_DISPLAY_BAR_H);
        }
        bars_remaining--;
    }

    of_video_flip();
    retarget_frameplace();
    uint32_t _c = of_time_us();

    d3d_audio_pump();
    uint32_t _d = of_time_us();

    np_input_us = _b - _a;
    np_flip_us  = _c - _b;
    np_audio_us = _d - _c;
}

void VBE_setPalette(uint8_t *palettebuffer)
{
    /*
     * BUILD palette format (per entry, 4 bytes):
     *   byte 0: Blue  (0-63)
     *   byte 1: Green (0-63)
     *   byte 2: Red   (0-63)
     *   byte 3: Reserved
     *
     * Convert to 0x00RRGGBB for of_video_palette_bulk().
     */
    for (int i = 0; i < 256; i++) {
        uint8_t b8 = (uint8_t)((palettebuffer[i * 4 + 0] * 255 + 31) / 63);
        uint8_t g8 = (uint8_t)((palettebuffer[i * 4 + 1] * 255 + 31) / 63);
        uint8_t r8 = (uint8_t)((palettebuffer[i * 4 + 2] * 255 + 31) / 63);
        of_palette[i] = ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8;
    }

    if (video_initialized)
        of_video_palette_bulk(of_palette, 256);
}

void VBE_getPalette(int32_t start, int32_t num, uint8_t *palettebuffer)
{
    /* Reverse-convert from our cached 0x00RRGGBB back to 6-bit VGA format */
    uint8_t *p = palettebuffer + (start * 4);
    int i;

    for (i = start; i < start + num; i++) {
        uint32_t rgb = of_palette[i];
        uint8_t r8 = (rgb >> 16) & 0xFF;
        uint8_t g8 = (rgb >>  8) & 0xFF;
        uint8_t b8 = (rgb >>  0) & 0xFF;

        *p++ = (uint8_t)((b8 * 63 + 127) / 255);
        *p++ = (uint8_t)((g8 * 63 + 127) / 255);
        *p++ = (uint8_t)((r8 * 63 + 127) / 255);
        *p++ = 0;
    }
}

void VBE_presentPalette(void)
{
    if (!video_initialized) return;
    /* Palette is already applied to hardware by VBE_setPalette →
     * of_video_palette_bulk.  Do NOT flip the triple buffer here —
     * the splash image lives in only one buffer, so flipping would
     * show a stale/empty frame and rapid-fire flips can crash the OS.
     *
     * One-frame delay so each fade step is visible (~60 Hz pacing). */
    usleep(16000);
}

void getvalidvesamodes(void)
{
    static int already_checked = 0;

    if (already_checked)
        return;

    already_checked = 1;
    validmodecnt = 0;
    vidoption = 1;

    /* Only mode: 320x200 */
    validmode[0]     = 0;
    validmodexdim[0] = OF_DISPLAY_W;
    validmodeydim[0] = OF_DISPLAY_H;
    validmodecnt     = 1;
}

int32_t _setgamemode(int32_t daxdim, int32_t daydim)
{
    (void)daxdim;
    (void)daydim;

    getvalidvesamodes();

    /* Always use 320x200 regardless of what was requested */
    if (video_initialized) {
        /* Clear the HW back buffer that BUILD is rendering into */
        uint8_t *dst = of_video_surface();
        if (dst) memset(dst, 0, OF_DISPLAY_W * OF_DISPLAY_HW_H);
        bars_remaining = 3;
    } else {
        memset(of_framebuffer, 0, sizeof(of_framebuffer));
    }
    init_new_res_vars();

    /* Retarget BUILD at HW surface if video is initialized */
    if (video_initialized)
        retarget_frameplace();

    qsetmode = 200;

    return 0;
}

void setvmode(int mode)
{
    if (mode == 0x3) {
        /* text mode -- nothing to tear down on openfpgaOS */
        return;
    }
}

void *_getVideoBase(void)
{
    return (void *)of_framebuffer;
}

/* ======================================================================
 * INPUT
 * ====================================================================== */

void initkeys(void)
{
    /* Nothing to initialize -- openfpgaOS handles input hardware */
}

void uninitkeys(void)
{
    /* Nothing to tear down */
}

int setupmouse(void)
{
    mouse_relative_x = mouse_relative_y = 0;
    mouse_buttons = 0;
    moustat = 1;
    return 1;
}

void readmousexy(short *x, short *y)
{
    if (x) *x = (short)(mouse_relative_x << 2);
    if (y) *y = (short)(mouse_relative_y << 2);

    mouse_relative_x = mouse_relative_y = 0;
}

void readmousebstatus(short *bstatus)
{
    if (bstatus)
        *bstatus = mouse_buttons;

    /* Clear wheel-like bits if we ever set them */
    if (mouse_buttons & 8)  mouse_buttons ^= 8;
    if (mouse_buttons & 16) mouse_buttons ^= 16;
}

uint8_t _readlastkeyhit(void)
{
    return (uint8_t)lastkey;
}

static void handle_events(void)
{
    of_input_state_t state;
    uint32_t buttons;
    uint32_t pressed, released;
    int i;

    of_input_poll();
    of_input_state(0, &state);

    buttons  = state.buttons;
    pressed  = buttons & ~prev_buttons;   /* just went down */
    released = ~buttons & prev_buttons;   /* just went up   */
    prev_buttons = buttons;

    /* --- Poll interact menu for Run Mode changes --- */
    current_run_mode = (int)of_interact_get(INTERACT_RUN_MODE);

    /* --- Digital buttons -> scancodes --- */
    for (i = 0; i < (int)NUM_BTN_MAPS; i++) {
        const btn_map_t *m = &button_map[i];

        if (pressed & m->btn) {
            send_key(m->scancode, m->extended, 1);
        }
        if (released & m->btn) {
            send_key(m->scancode, m->extended, 0);
        }
    }

    /* --- "Use = Use + Run" mode: B also sends LShift while held --- */
    if (current_run_mode == RUN_MODE_USE_RUN) {
        if (pressed & OF_BTN_B)
            send_key(0x2A, 0x00, 1);   /* LShift down = Run */
        if (released & OF_BTN_B)
            send_key(0x2A, 0x00, 0);   /* LShift up */
    }

    /* --- Left analog stick -> movement keys (Up/Down/Left/Right arrows) --- */
    {
        int16_t lx = state.joy_lx;
        int16_t ly = state.joy_ly;

        /* Up (negative Y) */
        int up_active    = (ly < -STICK_DEADZONE);
        int down_active  = (ly >  STICK_DEADZONE);
        int left_active  = (lx < -STICK_DEADZONE);
        int right_active = (lx >  STICK_DEADZONE);

        if (up_active && !lstick_up_held) {
            lstick_up_held = 1;
            send_key(SC_UP, SC_EXT, 1);
        } else if (!up_active && lstick_up_held) {
            lstick_up_held = 0;
            send_key(SC_UP, SC_EXT, 0);
        }

        if (down_active && !lstick_down_held) {
            lstick_down_held = 1;
            send_key(SC_DOWN, SC_EXT, 1);
        } else if (!down_active && lstick_down_held) {
            lstick_down_held = 0;
            send_key(SC_DOWN, SC_EXT, 0);
        }

        if (left_active && !lstick_left_held) {
            lstick_left_held = 1;
            send_key(SC_LEFT, SC_EXT, 1);
        } else if (!left_active && lstick_left_held) {
            lstick_left_held = 0;
            send_key(SC_LEFT, SC_EXT, 0);
        }

        if (right_active && !lstick_right_held) {
            lstick_right_held = 1;
            send_key(SC_RIGHT, SC_EXT, 1);
        } else if (!right_active && lstick_right_held) {
            lstick_right_held = 0;
            send_key(SC_RIGHT, SC_EXT, 0);
        }
    }

    /* --- Right analog stick -> mouse look --- */
    {
        int16_t rx = state.joy_rx;
        int16_t ry = state.joy_ry;

        if (rx > STICK_DEADZONE || rx < -STICK_DEADZONE)
            mouse_relative_x += rx / (32768 / STICK_MOUSE_SCALE);

        if (ry > STICK_DEADZONE || ry < -STICK_DEADZONE)
            mouse_relative_y += ry / (32768 / STICK_MOUSE_SCALE);
    }

    /* --- R2 as mouse left-button (alt fire / useful in menus) --- */
    mouse_buttons = 0;
    if (buttons & OF_BTN_R2)
        mouse_buttons |= 1;  /* left mouse button */
    if (buttons & OF_BTN_L2)
        mouse_buttons |= 2;  /* right mouse button */
}

void _handle_events(void)
{
    handle_events();
}

/* ======================================================================
 * TIMER
 * ====================================================================== */

static int64_t  timerfreq = 0;
static int32_t  timerlastsample = 0;
static int      timerticspersec = 0;
static void     (*usertimercallback)(void) = NULL;

void (*installusertimercallback(void (*callback)(void)))(void)
{
    void (*oldcb)(void);
    oldcb = usertimercallback;
    usertimercallback = callback;
    return oldcb;
}

int inittimer(int tickspersecond)
{
    int64_t t;

    if (timerfreq) return 0;  /* already installed */

    timerfreq = 1000;
    timerticspersec = tickspersecond;
    t = (int64_t)of_time_ms();
    timerlastsample = (int32_t)(t * timerticspersec / timerfreq);

    usertimercallback = NULL;

    return 0;
}

void uninittimer(void)
{
    if (!timerfreq) return;
    timerfreq = 0;
    timerticspersec = 0;
}

void resettimer(void)
{
    int64_t t = (int64_t)of_time_ms();
    timerlastsample = (int32_t)(t * timerticspersec / timerfreq);
}

void sampletimer(void)
{
    int64_t i;
    int32_t n;

    if (!timerfreq) return;

    /* Poll input so button presses are detected even in non-rendering loops */
    _handle_events();

    /* Audio pump: MIDI is timer-driven (60Hz), voice completion polled here */
    d3d_audio_pump();

    i = (int64_t)of_time_ms();
    n = (int32_t)(i * timerticspersec / timerfreq) - timerlastsample;

    if (n > 0) {
        /* Always advance timerlastsample by the real elapsed amount
         * so we don't accumulate permanent drift */
        totalclock += n;
        timerlastsample += n;

        /* But clamp callback iterations to prevent hang after loading pauses */
        int callbacks = n;
        if (callbacks > 4) callbacks = 4;

        if (usertimercallback) {
            for (; callbacks > 0; callbacks--)
                usertimercallback();
        }
    }
}

uint32_t getticks(void)
{
    if (!timerfreq) return of_time_ms();
    return (uint32_t)((int64_t)of_time_ms() * 1000 / timerfreq);
}

int gettimerfreq(void)
{
    return timerticspersec;
}

/* ======================================================================
 * STUBS & MISC
 * ====================================================================== */

void screencapture(char *filename)
{
    (void)filename;
    /* No-op on openfpgaOS -- no filesystem to save screenshots to */
}

void _joystick_init(void)   { /* no-op */ }
void _joystick_deinit(void) { /* no-op */ }
int  _joystick_update(void) { return 0; }
int  _joystick_axis(int axis)     { (void)axis;   return 0; }
int  _joystick_hat(int hat)       { (void)hat;    return -1; }
int  _joystick_button(int button) { (void)button; return 0; }

void _uninitengine(void)
{
    /* Nothing to tear down -- the FPGA OS manages video hardware */
}

void _idle(void)
{
    _handle_events();
    usleep(1000);
}

/* --- 2D editor drawing primitives (minimal stubs for game mode) --- */

uint8_t readpixel(uint8_t *offset)
{
    return *offset;
}

void drawpixel(uint8_t *location, uint8_t pixel)
{
    *location = pixel;
}

void setcolor16(uint8_t col)
{
    drawpixel_color = col;
}

void drawpixel16(int32_t offset)
{
    if (offset >= 0 && offset < OF_DISPLAY_W * OF_DISPLAY_H)
        ((uint8_t *)frameplace)[offset] = drawpixel_color;
}

void fillscreen16(int32_t offset, int32_t color, int32_t blocksize)
{
    uint8_t *pixels = (uint8_t *)frameplace;
    int32_t fb_size = OF_DISPLAY_W * OF_DISPLAY_H;

    if (!pageoffset) {
        offset = offset << 3;
        offset += 640 * 336;  /* original code compat -- will likely be clipped */
    }

    if (offset < 0) offset = 0;
    if (offset + blocksize > fb_size)
        blocksize = fb_size - offset;
    if (blocksize <= 0) return;

    memset(pixels + offset, (int)color, blocksize);
    _nextpage();
}

void drawline16(int32_t XStart, int32_t YStart, int32_t XEnd, int32_t YEnd, uint8_t Color)
{
    /* Bresenham line into the of framebuffer.
       Simplified version -- only needed for 2D map overlay. */
    int dx = abs((int)(XEnd - XStart));
    int dy = abs((int)(YEnd - YStart));
    int sx = (XStart < XEnd) ? 1 : -1;
    int sy = (YStart < YEnd) ? 1 : -1;
    int err = dx - dy;
    int x = (int)XStart;
    int y = (int)YStart;
    uint8_t *fb = (uint8_t *)frameplace;

    while (1) {
        if (x >= 0 && x < OF_DISPLAY_W && y >= 0 && y < OF_DISPLAY_H)
            fb[y * OF_DISPLAY_W + x] = Color;

        if (x == (int)XEnd && y == (int)YEnd) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

void clear2dscreen(void)
{
    memset((void *)frameplace, 0, OF_DISPLAY_W * OF_DISPLAY_H);
}

void _updateScreenRect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)x; (void)y; (void)w; (void)h;
    /* Just do a full flip -- partial updates aren't meaningful on openfpgaOS */
    _nextpage();
}

void fullscreen_toggle_and_change_driver(void)
{
    /* No-op on openfpgaOS -- always fullscreen */
}

#endif /* OPENFPGA */
