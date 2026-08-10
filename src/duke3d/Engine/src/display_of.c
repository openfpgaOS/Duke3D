/*
 * display_of.c -- openfpgaOS display/input/timer shim for Duke Nukem 3D
 *
 * Replaces the SDL2 display.c with calls to the openfpgaOS API (of_*).
 * Targets openfpgaOS: 320x200 indexed framebuffer, gamepad input.
 *
 * BUILD engine renders directly into the active 320x200 hardware surface.
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
#include "../../d3d_gpu.h"
#include "of_midi.h"
#include "of_timer.h"
#include "engine.h"
#include "draw.h"
#include "cache.h"
#include "duke3d.h"

#include "of.h"

/* ======================================================================
 * Internal state
 * ====================================================================== */

#define OF_DISPLAY_W  320
#define OF_DISPLAY_H  200
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
static int sync_next_palette_update = 0;

#define DOCK_MOUSE_DIVISOR  32  /* physical mouse deltas are high-DPI */
#define DOCK_MOUSE_FIRE_SCANCODE  0x1D  /* LCtrl */
#define DOCK_MOUSE_USE_SCANCODE   0x39  /* Space */
#define OPEN_BUTTON_MENU_SCANCODE  0x01  /* Escape = menu back */
#define B_TAP_USE_MS  500
#define TAP_USE_HOLD_MS  80
#define STICK_BUTTON_SUPPRESS_DEADZONE  8000

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

typedef struct {
    uint8_t active;
    uint8_t scancode;
    uint8_t extended;
} held_key_t;

typedef struct {
    uint8_t usage;
    uint8_t scancode;
    uint8_t extended;
} hid_key_map_t;

typedef struct {
    uint16_t mask;
    uint8_t  scancode;
    uint8_t  extended;
} keymod_map_t;

static const btn_map_t button_map[] = {
    /* Buttons with shoulder-modified alternatives are reconciled below. */
    { OF_BTN_L2,     0x39, 0x00 },  /* L2     -> Space  = Open        */
    { OF_BTN_R2,     0x1D, 0x00 },  /* R2     -> LCtrl  = Fire        */
};

#define NUM_BTN_MAPS  (sizeof(button_map) / sizeof(button_map[0]))

static const uint8_t hid_alpha_scancodes[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
    0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
    0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
};

static int duke_menu_active(void)
{
    if (myconnectindex < 0 || myconnectindex >= MAXPLAYERS)
        return 0;

    return (ps[myconnectindex].gm & MODE_MENU) != 0;
}

static int duke_perf_gameplay_active(void)
{
    if (myconnectindex < 0 || myconnectindex >= MAXPLAYERS)
        return 0;

    return (ps[myconnectindex].gm & MODE_GAME) &&
           !(ps[myconnectindex].gm & (MODE_MENU | MODE_TYPE |
                                      MODE_EOL | MODE_RESTART |
                                      MODE_END));
}

static const hid_key_map_t hid_key_map[] = {
    { 0x28, 0x1C, 0x00 },  /* Enter */
    { 0x29, 0x01, 0x00 },  /* Escape */
    { 0x2A, 0x0E, 0x00 },  /* Backspace */
    { 0x2B, 0x0F, 0x00 },  /* Tab */
    { 0x2C, 0x39, 0x00 },  /* Space */
    { 0x2D, 0x0C, 0x00 },  /* Minus */
    { 0x2E, 0x0D, 0x00 },  /* Equals */
    { 0x2F, 0x1A, 0x00 },  /* Left bracket */
    { 0x30, 0x1B, 0x00 },  /* Right bracket */
    { 0x31, 0x2B, 0x00 },  /* Backslash */
    { 0x33, 0x27, 0x00 },  /* Semicolon */
    { 0x34, 0x28, 0x00 },  /* Quote */
    { 0x35, 0x29, 0x00 },  /* Grave/Tilde */
    { 0x36, 0x33, 0x00 },  /* Comma */
    { 0x37, 0x34, 0x00 },  /* Period */
    { 0x38, 0x35, 0x00 },  /* Slash */

    { 0x49, 0x52, 0xE0 },  /* Insert */
    { 0x4A, 0x47, 0xE0 },  /* Home */
    { 0x4B, 0x49, 0xE0 },  /* PageUp */
    { 0x4C, 0x53, 0xE0 },  /* Delete */
    { 0x4D, 0x4F, 0xE0 },  /* End */
    { 0x4E, 0x51, 0xE0 },  /* PageDown */
    { 0x4F, 0x4D, 0xE0 },  /* Right arrow */
    { 0x50, 0x4B, 0xE0 },  /* Left arrow */
    { 0x51, 0x50, 0xE0 },  /* Down arrow */
    { 0x52, 0x48, 0xE0 },  /* Up arrow */
};

static const keymod_map_t keymod_map[] = {
    { OF_KEYMOD_LCTRL,  0x1D, 0x00 },
    { OF_KEYMOD_LSHIFT, 0x2A, 0x00 },
    { OF_KEYMOD_LALT,   0x38, 0x00 },
    { OF_KEYMOD_RCTRL,  0x1D, 0xE0 },
    { OF_KEYMOD_RSHIFT, 0x36, 0x00 },
    { OF_KEYMOD_RALT,   0x38, 0xE0 },
};

/* Left-stick virtual keys (W/A/S/D mapped to arrow keys for movement) */
#define SC_UP     0x48
#define SC_DOWN   0x50
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_PGUP   0x49
#define SC_PGDN   0x51
#define SC_EXT    0xE0
#define SC_FIRE   0x1D  /* LCtrl */
#define SC_ESCAPE 0x01
#define SC_OPEN   0x39  /* Space */
#define SC_TAB    0x0F
#define SC_JUMP   0x1E  /* A */
#define SC_CROUCH 0x2C  /* Z */
#define SC_QUICK_KICK      0x2E  /* C */
#define SC_INVENTORY_USE   0x1C  /* Enter */
#define SC_INVENTORY_LEFT  0x1A  /* [ */
#define SC_INVENTORY_RIGHT 0x1B  /* ] */
#define SC_NEXT_WEAPON     0x28  /* ' */
#define SC_PREV_WEAPON     0x27  /* ; */
#define SC_STRAFE_LEFT     0x33  /* , */
#define SC_STRAFE_RIGHT    0x34  /* . */
#define SC_RUN             0x2A  /* LShift */

static held_key_t pocket_btn_a_key;
static held_key_t pocket_btn_x_key;
static held_key_t pocket_btn_y_key;
static held_key_t pocket_btn_select_key;
static held_key_t pocket_btn_start_key;
static held_key_t pocket_dpad_up_key;
static held_key_t pocket_dpad_down_key;
static held_key_t pocket_dpad_left_key;
static held_key_t pocket_dpad_right_key;
static uint8_t pocket_b_mode = 0;
static uint8_t pocket_b_tap_use_allowed = 0;
static unsigned int pocket_b_press_ms = 0;
static uint8_t tap_use_held = 0;
static unsigned int tap_use_release_ms = 0;
static int32_t dock_mouse_accum_x = 0;
static int32_t dock_mouse_accum_y = 0;
static uint16_t dock_mouse_held = 0;   /* fire/use buttons we have latched */
static of_input_state_t joystick_state;
static int joystick_state_valid = 0;

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

static void update_pocket_button_key(uint32_t buttons, uint32_t btn,
                                     uint8_t scancode, uint8_t extended,
                                     held_key_t *held)
{
    if (buttons & btn) {
        if (!held->active) {
            held->active = 1;
            held->scancode = scancode;
            held->extended = extended;
            send_key(scancode, extended, 1);
        } else if (held->scancode != scancode || held->extended != extended) {
            send_key(held->scancode, held->extended, 0);
            held->scancode = scancode;
            held->extended = extended;
            send_key(scancode, extended, 1);
        }
    } else if (held->active) {
        send_key(held->scancode, held->extended, 0);
        held->active = 0;
        held->scancode = 0;
        held->extended = 0;
    }
}

static int stick_axis_active(int16_t axis)
{
    return axis <= -STICK_BUTTON_SUPPRESS_DEADZONE ||
           axis >=  STICK_BUTTON_SUPPRESS_DEADZONE;
}

static uint32_t filter_stick_direction_buttons(uint32_t buttons,
                                               const of_input_state_t *state)
{
    if (stick_axis_active(state->joy_lx) ||
        stick_axis_active(state->joy_ly) ||
        stick_axis_active(state->joy_rx) ||
        stick_axis_active(state->joy_ry)) {
        buttons &= ~(OF_BTN_UP | OF_BTN_DOWN | OF_BTN_LEFT | OF_BTN_RIGHT);
    }

    return buttons;
}

static void update_tap_use_release(void)
{
    if (tap_use_held && (int)(of_time_ms() - tap_use_release_ms) >= 0) {
        send_key(SC_OPEN, 0x00, 0);
        tap_use_held = 0;
    }
}

static void start_tap_use(void)
{
    if (!tap_use_held) {
        send_key(SC_OPEN, 0x00, 1);
        tap_use_held = 1;
    }
    tap_use_release_ms = of_time_ms() + TAP_USE_HOLD_MS;
}

static void cancel_tap_use(void)
{
    if (tap_use_held) {
        send_key(SC_OPEN, 0x00, 0);
        tap_use_held = 0;
    }
}

static void stop_pocket_b_mode(uint8_t mode, int allow_tap_use)
{
    unsigned int held_ms;

    switch (mode) {
    case 1: /* hold-run/tap-use */
        send_key(SC_RUN, 0x00, 0);
        held_ms = of_time_ms() - pocket_b_press_ms;
        if (allow_tap_use && pocket_b_tap_use_allowed && held_ms < B_TAP_USE_MS)
            start_tap_use();
        break;
    case 2: /* menu back */
        send_key(OPEN_BUTTON_MENU_SCANCODE, 0x00, 0);
        break;
    case 3: /* R1+L1+B inventory use */
        send_key(SC_INVENTORY_USE, 0x00, 0);
        break;
    case 4: /* R1+B look down */
        send_key(SC_PGDN, SC_EXT, 0);
        break;
    default:
        break;
    }
}

static void start_pocket_b_mode(uint8_t mode)
{
    switch (mode) {
    case 1:
        cancel_tap_use();
        pocket_b_press_ms = of_time_ms();
        send_key(SC_RUN, 0x00, 1);
        break;
    case 2:
        cancel_tap_use();
        send_key(OPEN_BUTTON_MENU_SCANCODE, 0x00, 1);
        break;
    case 3:
        cancel_tap_use();
        send_key(SC_INVENTORY_USE, 0x00, 1);
        break;
    case 4:
        cancel_tap_use();
        send_key(SC_PGDN, SC_EXT, 1);
        break;
    default:
        break;
    }
}

static void update_pocket_b_button(uint32_t buttons, int menu_active,
                                   int right_modifier, int both_shoulder_modifier)
{
    uint8_t desired_mode = 0;
    uint8_t old_mode;

    if (buttons & OF_BTN_B) {
        if (menu_active)
            desired_mode = 2;
        else if (both_shoulder_modifier)
            desired_mode = 3;
        else if (right_modifier)
            desired_mode = 4;
        else
            desired_mode = 1;
    }

    if (desired_mode == pocket_b_mode)
        return;

    old_mode = pocket_b_mode;
    stop_pocket_b_mode(pocket_b_mode, desired_mode == 0);
    pocket_b_mode = desired_mode;
    pocket_b_tap_use_allowed = desired_mode == 1 && old_mode == 0;
    start_pocket_b_mode(pocket_b_mode);
}

static int hid_usage_to_scancode(uint8_t usage, uint8_t *scancode,
                                 uint8_t *extended)
{
    if (usage >= 0x04 && usage <= 0x1D) {
        *scancode = hid_alpha_scancodes[usage - 0x04];
        *extended = 0x00;
        return 1;
    }

    if (usage >= 0x1E && usage <= 0x27) {
        *scancode = usage - 0x1C;
        *extended = 0x00;
        return 1;
    }

    if (usage >= 0x3A && usage <= 0x43) {
        *scancode = usage + 1;
        *extended = 0x00;
        return 1;
    }

    if (usage == 0x44) {
        *scancode = 0x57;
        *extended = 0x00;
        return 1;
    }

    if (usage == 0x45) {
        *scancode = 0x58;
        *extended = 0x00;
        return 1;
    }

    for (int i = 0; i < (int)(sizeof(hid_key_map) / sizeof(hid_key_map[0])); i++) {
        if (hid_key_map[i].usage == usage) {
            *scancode = hid_key_map[i].scancode;
            *extended = hid_key_map[i].extended;
            return 1;
        }
    }

    return 0;
}

static void send_hid_key(uint8_t usage, int pressed)
{
    uint8_t scancode;
    uint8_t extended;

    if (hid_usage_to_scancode(usage, &scancode, &extended))
        send_key(scancode, extended, pressed);
}

static void send_hid_key_edges_word(int word, uint32_t bits, int pressed)
{
    while (bits) {
        int bit = __builtin_ctz(bits);
        send_hid_key((uint8_t)(word * 32 + bit), pressed);
        bits &= bits - 1;
    }
}

static void send_hid_key_edges(const uint32_t keys[OF_KEYBOARD_WORDS],
                               int pressed)
{
    for (int word = 0; word < (int)OF_KEYBOARD_WORDS; word++)
        send_hid_key_edges_word(word, keys[word], pressed);
}

static void send_modifier_edges(uint16_t modifiers, int pressed)
{
    for (int i = 0; i < (int)(sizeof(keymod_map) / sizeof(keymod_map[0])); i++) {
        if (modifiers & keymod_map[i].mask)
            send_key(keymod_map[i].scancode, keymod_map[i].extended, pressed);
    }
}

/* Mirror of the keyboard's level state, kept so a hot-unplug can undo it.
 * The HAL derives edges per poll, so once the device is gone there is
 * nothing left to derive a release from — this snapshot is the only
 * record of what BUILD still believes is held. */
static uint32_t kb_held_keys[OF_KEYBOARD_WORDS];
static uint16_t kb_held_modifiers;

static void remember_keyboard_held(const uint32_t keys[OF_KEYBOARD_WORDS],
                                   uint16_t modifiers)
{
    memcpy(kb_held_keys, keys, sizeof(kb_held_keys));
    kb_held_modifiers = modifiers;
}

static void release_keyboard_held(void)
{
    if (kb_held_modifiers) {
        send_modifier_edges(kb_held_modifiers, 0);
        kb_held_modifiers = 0;
    }

    for (int word = 0; word < (int)OF_KEYBOARD_WORDS; word++) {
        if (!kb_held_keys[word])
            continue;
        send_hid_key_edges_word(word, kb_held_keys[word], 0);
        kb_held_keys[word] = 0;
    }
}

/* From caps v4 the OS hands us decoded mouse counts.  Older firmware
 * passes the Pocket dock's packed sample pairs {a << 8 | b} through raw,
 * where the true delta is the sum of the two int8 halves.  Feeding a
 * packed pair to the scaler unchanged reads ~128x too fast and inverts
 * on sign, so this has to run before any scaling.
 *
 * Probed once and cached: of_get_caps() is a pointer chase, and this
 * sits in the per-frame input path. */
static int32_t mouse_counts(int32_t v)
{
    static int raw_pairs = -1;

    if (raw_pairs < 0) {
        const struct of_capabilities *caps = of_get_caps();

        raw_pairs = caps != NULL
                 && caps->platform_id == OF_PLATFORM_POCKET
                 && (caps->version < 4
                  || !(caps->os_features & OF_OS_FEAT_MOUSE_COUNTS));
    }

    if (!raw_pairs)
        return v;

    return (int32_t)((int8_t)(v >> 8) + (int8_t)v);
}

static int32_t consume_scaled_mouse_delta(int32_t *accum, int32_t delta)
{
    int32_t scaled;

    *accum += mouse_counts(delta);
    scaled = *accum / DOCK_MOUSE_DIVISOR;
    *accum -= scaled * DOCK_MOUSE_DIVISOR;
    return scaled;
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

    /* Match the actual hardware triple-buffer count.  BUILD's
     * rotatesprite uses `numpages` to know how many times to redraw
     * each `dastat & 128` (perm) HUD element so it lands on every
     * buffer in the rotation.  With numpages=1 (the previous setting)
     * each HUD perm draw was queued NOT-AT-ALL — engine.c::rotatesprite
     * line 8119 gates the permfifo enqueue on `numpages >= 2`, so HUD
     * elements only appeared on whichever single buffer the game
     * happened to be rendering when the state changed.  As the
     * triple-buffer rotation cycled, the other two buffers showed
     * stale (or zeroed) content where the HUD should be → visible
     * flashing for the first 3 frames after every HUD update.
     * numpages=3 enables the standard BUILD perm-sprite handling:
     * each HUD draw is queued in permfifo, and nextpage() walks the
     * queue once per frame redrawing every queued sprite onto the
     * current buffer until pagesleft reaches 0 (= drawn 3×, once on
     * each of the 3 hardware buffers). */
    numpages = 3;
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

static void set_duke_video_mode(void)
{
    of_video_mode_t mode = {
        OF_DISPLAY_W,
        OF_DISPLAY_H,
        OF_DISPLAY_W,
        OF_VIDEO_MODE_8BIT,
        0
    };
    of_video_mode_t actual;

    if (of_video_set_mode(&mode) < 0) {
        printf("Duke3D: failed to set openfpgaOS 320x200 video mode\n");
        exit(1);
    }

    of_video_get_mode(&actual);
    if (actual.width != OF_DISPLAY_W ||
        actual.height != OF_DISPLAY_H ||
        actual.stride != OF_DISPLAY_W ||
        actual.color_mode != OF_VIDEO_MODE_8BIT) {
        printf("Duke3D: unexpected video mode %ux%u stride %u color %u\n",
               (unsigned)actual.width,
               (unsigned)actual.height,
               (unsigned)actual.stride,
               (unsigned)actual.color_mode);
        exit(1);
    }
}

/* Point BUILD's rendering target at the given HW back buffer.
 * Also retargets the GPU framebuffer to the same back buffer so any
 * GPU-driven span draws this frame land in the right surface. */
static void retarget_frameplace_at(uint8_t *dst)
{
    if (dst) {
        /* Keep BUILD CPU fallback/2D writes on the uncached alias.  The
         * cached framebuffer experiment is compiled off in the fixed GPU
         * policy because the uncached path has the simpler coherency model. */
        frameplace = D3D_GPU_USE_CACHED_FRAMEPLACE ?
            dst : (uint8_t *)of_uncached(dst);
        extern int32_t viewoffset;
        frameoffset = frameplace + viewoffset;
        d3d_gpu_set_fb(dst, OF_DISPLAY_W);
    }
}

static void retarget_frameplace(void)
{
    retarget_frameplace_at(of_video_surface());
}

static int video_initialized = 0;

/* GPU-triggered flip pipeline state.  draw_idx tracks the slot the
 * GPU's CMD_FLIP will target this frame; initialized in
 * ensure_video_init() to the kernel's initial buf_draw via
 * of_video_acquire_next(-1), then advanced each _nextpage() to the
 * new draw slot returned by of_video_acquire_next(draw_idx). */
static int draw_idx = -1;

static void ensure_video_init(void) {
    if (!video_initialized) {
        video_initialized = 1;
        of_video_init();
        set_duke_video_mode();
        of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
        of_video_palette_bulk(of_palette, 256);
        /* Clear all 3 triple-buffer frames using the kernel-driven flip
         * path.  Wait each startup flip out before handing the stream to
         * GPU-triggered CMD_FLIP; otherwise two back-to-back nonblocking
         * flips can leave the kernel's software roles one vsync ahead of
         * the hardware queue. */
        of_video_clear(0); of_video_flip(); of_video_wait_flip();
        of_video_clear(0); of_video_flip(); of_video_wait_flip();
        of_video_clear(0);
        /* Bring up the GPU before the first frame.  Safe no-op on
         * targets without a GPU window (caps->gpu_base == 0). */
        d3d_gpu_init();
        /* Capture the kernel's current draw idx so the first
         * _nextpage()'s of_gpu_flip_to() targets the right slot.
         * Passing -1 means "no previous flip — just tell me which
         * slot is currently drawable". */
        draw_idx = of_video_acquire_next(-1, 0);
        /* Point BUILD at the HW back buffer from now on */
        retarget_frameplace_at(of_video_buffer_addr(draw_idx));
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

    /* Zero the static fallback framebuffer.  CPU memset is correct
     * here — _platform_init runs before any GPU init, and the
     * of_framebuffer array lives in BSS, not in GPU-accessible SDRAM. */
    memset(of_framebuffer, 0, sizeof(of_framebuffer));
}

/* GPU-triggered flip pipeline (cr-gpu-triggered-flip.md):
 *   1. d3d_gpu_drain_batch — ship pending span buffer to GPU ring
 *   2. d3d_gpu_flip_to(idx) — emit CMD_FLIP via of_gpu_flip_to + kick;
 *      the GPU drains its m_wr_* writes, pulses the swap side-port,
 *      and publishes the fence token.
 *   3. of_video_acquire_next(idx) — waits for the CMD_FLIP fence and
 *      returns the next free draw idx.
 * No CPU spin on STATUS_BUSY: CMD_FLIP's drain primitive replaces it.
 * Do not wait for the previous swap before submitting the drain/flip.
 * GPU CMD_FLIP is back-pressured by fb_swap_pending, so queuing it early
 * lets GPU write-drain overlap the tail of the current scanout. */
void _nextpage(void)
{
    ensure_video_init();

    static uint32_t last_page_begin_us = 0;
    static uint32_t last_page_end_us = 0;
    uint32_t page_begin_us = d3d_gpu_perf_enable ? of_time_us() : 0;
    uint32_t frame_period_us = 0;
    uint32_t render_us = 0;
    int perf_gameplay = d3d_gpu_perf_enable && duke_perf_gameplay_active();
    if (d3d_gpu_perf_enable) {
        frame_period_us = last_page_begin_us ?
            (page_begin_us - last_page_begin_us) : 0;
        render_us = last_page_end_us ?
            (page_begin_us - last_page_end_us) : 0;
        last_page_begin_us = page_begin_us;
    }

    _handle_events();

    /* BUILD already rendered into the HW back buffer via frameplace. */
    d3d_gpu_prepare_framebuffer_for_present();

    uint32_t wait_flip_us = 0;

    uint32_t drain_batch_us = 0;
    uint32_t flip_emit_us = 0;
    uint32_t acquire_us = 0;

    if (d3d_gpu_present) {
        /* Ship pending spans + emit CMD_FLIP for the current draw idx.
         * Both go through d3d_gpu wrappers because of_gpu.h's static ring
         * state (_gpu_wrptr, _gpu_fence_next) MUST live in exactly one TU
         * — d3d_gpu.c. */
        uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        d3d_gpu_drain_batch();
        drain_batch_us = t0 ? (of_time_us() - t0) : 0;

        t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        uint32_t flip_token = d3d_gpu_flip_to(draw_idx);
        flip_emit_us = t0 ? (of_time_us() - t0) : 0;

        t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        draw_idx = of_video_acquire_next(draw_idx, flip_token);
        acquire_us = t0 ? (of_time_us() - t0) : 0;

        retarget_frameplace_at(of_video_buffer_addr(draw_idx));
    } else {
        uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
        of_video_flip();
        flip_emit_us = t0 ? (of_time_us() - t0) : 0;
        draw_idx = -1;
        retarget_frameplace();
    }

    uint32_t t0 = d3d_gpu_perf_enable ? of_time_us() : 0;
    d3d_audio_pump();
    uint32_t audio_us = t0 ? (of_time_us() - t0) : 0;

    perf_gameplay = d3d_gpu_perf_enable && duke_perf_gameplay_active();
    if (d3d_gpu_perf_enable) {
        uint32_t page_end_us = of_time_us();
        uint32_t page_us = page_end_us - page_begin_us;
        if (perf_gameplay)
            d3d_gpu_perf_report_frame(frame_period_us, render_us, page_us,
                                      wait_flip_us, drain_batch_us,
                                      flip_emit_us, acquire_us, audio_us);
        else {
            d3d_gpu_perf_discard_interval();
            last_page_begin_us = 0;
            last_page_end_us = 0;
            return;
        }
        last_page_end_us = page_end_us;
    }
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

    if (video_initialized) {
        if (sync_next_palette_update) {
            sync_next_palette_update = 0;
            of_video_wait_flip();
            if (OF_SVC && OF_SVC->video_vsync)
                OF_SVC->video_vsync();
        }
        of_video_palette_bulk(of_palette, 256);
    }
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

void VBE_syncNextPaletteUpdate(void)
{
    /* Palette RAM is live during scanout. Startup fades rewrite all 256
     * entries, so align the actual hardware bulk write to vblank. This arms
     * VBE_setPalette() to wait only after BUILD has finished constructing and
     * converting the palette, keeping the write as close to vblank as possible.
     */
    sync_next_palette_update = 1;
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
        /* Clear the active 320x200 back buffer via GPU. */
        uint8_t *dst = of_video_surface();
        if (dst) d3d_gpu_clear_rect_fb(dst, OF_DISPLAY_W,
                                       OF_DISPLAY_H, 0);
    } else {
        /* CPU fallback only — of_framebuffer is a static C array used
         * pre-video-init; the GPU isn't initialized yet and can't
         * address this region anyway. */
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
    of_keyboard_state_t kb;
    of_mouse_state_t ms;
    uint32_t buttons;
    uint32_t pressed, released;
    int menu_active;
    int left_modifier;
    int right_modifier;
    int both_shoulder_modifier;
    int i;

    memset(&kb, 0, sizeof(kb));
    memset(&ms, 0, sizeof(ms));

    of_input_poll();
    of_input_keyboard_state(&kb);
    of_input_mouse_state(&ms);
    of_input_state(0, &state);
    joystick_state = state;
    joystick_state_valid = 1;

    buttons  = filter_stick_direction_buttons(state.buttons, &state);
    pressed  = buttons & ~prev_buttons;   /* just went down */
    released = ~buttons & prev_buttons;   /* just went up   */
    prev_buttons = buttons;
    menu_active = duke_menu_active();
    left_modifier = !menu_active && (buttons & OF_BTN_L1);
    right_modifier = !menu_active && (buttons & OF_BTN_R1);
    both_shoulder_modifier = left_modifier && right_modifier;

    update_tap_use_release();

    /* --- Physical dock keyboard -> DOS scancodes --- */
    if (kb.present) {
        send_modifier_edges(kb.modifiers_pressed, 1);
        send_hid_key_edges(kb.keys_pressed, 1);
        send_hid_key_edges(kb.keys_released, 0);
        send_modifier_edges(kb.modifiers_released, 0);
        remember_keyboard_held(kb.keys, kb.modifiers);
    } else {
        /* Unplugged: the release edges for whatever was down never
         * arrive, and BUILD latches key state — a keyboard pulled
         * mid-strafe leaves the player walking into a wall forever. */
        release_keyboard_held();
    }

    /* --- Digital buttons with no shoulder-modified alternate -> scancodes --- */
    for (i = 0; i < (int)NUM_BTN_MAPS; i++) {
        const btn_map_t *m = &button_map[i];
        uint8_t scancode = m->scancode;
        uint8_t extended = m->extended;

        if (pressed & m->btn) {
            send_key(scancode, extended, 1);
        }
        if (released & m->btn) {
            send_key(scancode, extended, 0);
        }
    }

    /* --- Pocket shoulder modifiers --- */
    update_pocket_button_key(buttons, OF_BTN_UP,
                             SC_UP, SC_EXT, &pocket_dpad_up_key);
    update_pocket_button_key(buttons, OF_BTN_DOWN,
                             SC_DOWN, SC_EXT, &pocket_dpad_down_key);
    update_pocket_button_key(buttons, OF_BTN_LEFT,
                             left_modifier ? SC_STRAFE_LEFT : SC_LEFT,
                             left_modifier ? 0x00 : SC_EXT,
                             &pocket_dpad_left_key);
    update_pocket_button_key(buttons, OF_BTN_RIGHT,
                             left_modifier ? SC_STRAFE_RIGHT : SC_RIGHT,
                             left_modifier ? 0x00 : SC_EXT,
                             &pocket_dpad_right_key);
    update_pocket_button_key(buttons, OF_BTN_A,
                             both_shoulder_modifier ? SC_QUICK_KICK :
                                 (right_modifier ? SC_PGUP : SC_FIRE),
                             (right_modifier && !both_shoulder_modifier) ? SC_EXT : 0x00,
                             &pocket_btn_a_key);
    update_pocket_b_button(buttons, menu_active, right_modifier,
                           both_shoulder_modifier);
    update_pocket_button_key(buttons, OF_BTN_X,
                             right_modifier ? SC_NEXT_WEAPON : SC_JUMP,
                             0x00, &pocket_btn_x_key);
    update_pocket_button_key(buttons, OF_BTN_Y,
                             right_modifier ? SC_PREV_WEAPON : SC_CROUCH,
                             0x00, &pocket_btn_y_key);
    update_pocket_button_key(buttons, OF_BTN_SELECT,
                             right_modifier ? SC_INVENTORY_LEFT : SC_TAB,
                             0x00, &pocket_btn_select_key);
    update_pocket_button_key(buttons, OF_BTN_START,
                             right_modifier ? SC_INVENTORY_RIGHT : SC_ESCAPE,
                             0x00, &pocket_btn_start_key);

    /* --- Physical dock mouse --- */
    if (ms.present) {
        mouse_relative_x += consume_scaled_mouse_delta(&dock_mouse_accum_x, ms.dx);
        mouse_relative_y += consume_scaled_mouse_delta(&dock_mouse_accum_y, ms.dy);

        if (ms.buttons_pressed & 1)
            send_key(DOCK_MOUSE_FIRE_SCANCODE, 0x00, 1);
        if (ms.buttons_released & 1)
            send_key(DOCK_MOUSE_FIRE_SCANCODE, 0x00, 0);
        if (ms.buttons_pressed & 2)
            send_key(DOCK_MOUSE_USE_SCANCODE, 0x00, 1);
        if (ms.buttons_released & 2)
            send_key(DOCK_MOUSE_USE_SCANCODE, 0x00, 0);

        dock_mouse_held = ms.buttons & 3;
    } else if (dock_mouse_held) {
        /* Unplugged mid-click: the firmware's release edge is gone with
         * the device, so LCtrl/Space would stay latched down — the
         * player fires forever.  Same failure mode as the keyboard. */
        if (dock_mouse_held & 1)
            send_key(DOCK_MOUSE_FIRE_SCANCODE, 0x00, 0);
        if (dock_mouse_held & 2)
            send_key(DOCK_MOUSE_USE_SCANCODE, 0x00, 0);
        dock_mouse_held = 0;
    }

    mouse_buttons = 0;
    if (ms.present) {
        if (ms.buttons & 1)
            mouse_buttons |= 1;
        if (ms.buttons & 2)
            mouse_buttons |= 2;
        if (ms.buttons & 4)
            mouse_buttons |= 4;
    }
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

void _joystick_init(void)
{
    memset(&joystick_state, 0, sizeof(joystick_state));
    joystick_state_valid = 0;
}

void _joystick_deinit(void)
{
    joystick_state_valid = 0;
}

int _joystick_update(void)
{
    return joystick_state_valid;
}

int _joystick_axis(int axis)
{
    if (!joystick_state_valid)
        return 0;

    switch (axis) {
    case 0: return joystick_state.joy_lx;
    case 1: return joystick_state.joy_ly;
    case 2: return joystick_state.joy_rx;
    case 3: return joystick_state.joy_ry;
    case 4: return 0;
    case 5: return 0;
    default: return 0;
    }
}

int _joystick_hat(int hat)
{
    (void)hat;
    return 0;
}

int _joystick_button(int button)
{
    (void)button;
    return 0;
}

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
    /* GPU-routed via 1×1 clear_rect — every CPU FB write should
     * vanish per project_gpu_owns_framebuffer.md.  This is the
     * single drawpixel hot path used by automap line draws and
     * BUILD's 2D drawline2d, so converting it sweeps three call
     * sites at once. */
    d3d_gpu_clear_rect_fb(location, 1, 1, pixel);
}

void setcolor16(uint8_t col)
{
    drawpixel_color = col;
}

void drawpixel16(int32_t offset)
{
    if (offset < 0 || offset >= OF_DISPLAY_W * OF_DISPLAY_H) return;
    /* GPU-routed single pixel — 1×1 clear_rect.  Editor / 2D path,
     * called rarely; the per-pixel ring submission cost is fine. */
    d3d_gpu_clear_rect_fb((uint8_t *)frameplace + offset, 1, 1,
                          drawpixel_color);
}

void fillscreen16(int32_t offset, int32_t color, int32_t blocksize)
{
    int32_t fb_size = OF_DISPLAY_W * OF_DISPLAY_H;

    if (!pageoffset) {
        offset = offset << 3;
        offset += 640 * 336;  /* original code compat -- will likely be clipped */
    }

    if (offset < 0) offset = 0;
    if (offset + blocksize > fb_size)
        blocksize = fb_size - offset;
    if (blocksize <= 0) return;

    /* Single-row rect — full width × N rows would need (offset, w, h)
     * decomposition.  fillscreen16 here is treated as a 1-row clear of
     * `blocksize` bytes since BUILD's editor uses it for stripe fills.
     * Matches the prior memset semantics byte-for-byte. */
    d3d_gpu_clear_rect_fb((uint8_t *)frameplace + offset,
                          (uint16_t)blocksize, 1, (uint8_t)color);
    _nextpage();
}

void drawline16(int32_t XStart, int32_t YStart, int32_t XEnd, int32_t YEnd, uint8_t Color)
{
    /* Bresenham line — 2D map overlay only.  Each lit pixel becomes a
     * 1×1 of_gpu_clear_rect; the automap is opt-in and low-rate so the
     * per-pixel ring cost is acceptable.  GPU-only keeps the FB clean
     * of CPU writes (project_gpu_owns_framebuffer.md). */
    int dx = abs((int)(XEnd - XStart));
    int dy = abs((int)(YEnd - YStart));
    int sx = (XStart < XEnd) ? 1 : -1;
    int sy = (YStart < YEnd) ? 1 : -1;
    int err = dx - dy;
    int x = (int)XStart;
    int y = (int)YStart;

    while (1) {
        if (x >= 0 && x < OF_DISPLAY_W && y >= 0 && y < OF_DISPLAY_H)
            d3d_gpu_clear_rect_fb((uint8_t *)frameplace + y * OF_DISPLAY_W + x,
                                  1, 1, Color);

        if (x == (int)XEnd && y == (int)YEnd) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

void clear2dscreen(void)
{
    /* BUILD 2D-mode full-screen clear.  GPU-routed via clear_rect on the
     * 320x200 frameplace region — caller's setviewtotalarea / qsetmode
     * sequencing fences before any reads. */
    d3d_gpu_clear_rect_fb((uint8_t *)frameplace,
                          OF_DISPLAY_W, OF_DISPLAY_H, 0);
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
