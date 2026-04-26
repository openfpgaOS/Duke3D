/*
 * of_sdl_gpu.c — GPU bridge for the SDL2 shim (Phase 3).
 *
 * The SDL2 shim header (`SDL2/SDL.h`) cannot include `of_gpu.h` directly:
 * `of_gpu.h` carries static state (`_gpu_base`, `_gpu_wrptr`) and is
 * required to be included from exactly one translation unit per program.
 * Apps that use `of_gpu` directly (like `gpudemo/main.c`) already do that;
 * pulling it in via `SDL.h` would either duplicate state or block apps
 * from including both.
 *
 * Bridge: the shim calls these extern functions for any path that wants
 * GPU acceleration.  The SDK Makefile compiles this file with the same
 * status as `of_init.c`, so every app that links the SDK gets the
 * bridge automatically — but only this TU pulls in `of_gpu.h`.
 *
 * Lazy-init: `of_gpu_init()` runs on first use (idempotent in the SDK).
 * On targets without a GPU, the helpers degrade to CPU memset/memcpy.
 */

#include <stdint.h>
#include <string.h>

#include "of_gpu.h"

static int _of_sdl_gpu_inited = 0;

static void _of_sdl_gpu_lazy_init(void) {
    if (_of_sdl_gpu_inited) return;
    of_gpu_init();
    _of_sdl_gpu_inited = 1;
}

/*
 * GPU-accelerated solid fill into the framebuffer band.
 *
 * The caller passes the absolute SDRAM byte address of the rectangle's
 * top-left pixel, the rect dimensions, the stride of the surrounding FB
 * (in bytes/pixels), and the indexed-color value to splat.  The GPU's
 * CMD_CLEAR_RECT walks h rows × w bytes from `start_byte_addr`, advancing
 * each row by the active SET_FB stride.
 *
 * Returns 0 on success.  On a target without a GPU the caller should fall
 * back to CPU memset (we don't try to detect that here — the SDK init
 * panics on missing GPU caps).
 */
int of_sdl_gpu_fill_rect(uint32_t start_byte_addr,
                         uint16_t w, uint16_t h,
                         uint16_t stride,
                         uint8_t  color) {
    _of_sdl_gpu_lazy_init();
    /* Re-bind the FB stride each call.  The SDL2 destination surface's
     * pitch is the source of truth; if the caller is filling a sub-rect
     * of a 320-wide screen, stride=320 keeps each row's address derivation
     * inside the GPU correct.  This is cheap (single-word MMIO + ring
     * write) so doing it every call avoids subtle "last bind wins"
     * bugs across multiple surfaces. */
    of_gpu_set_framebuffer(start_byte_addr & 0xFFFFFE00, stride);
    of_gpu_clear_rect(start_byte_addr, w, h, color);
    of_gpu_finish();
    return 0;
}
