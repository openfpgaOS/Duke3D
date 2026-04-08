/*
 * of_gpu.h -- Hardware GPU Accelerator API for openfpgaOS
 *
 * Asynchronous span + triangle rasteriser.  CPU submits commands to a
 * 16 KB ring buffer in GPU-internal M10K BRAM; the GPU processes them
 * in parallel, writing pixels to the framebuffer via AXI4.
 *
 * Ring buffer: 16 KB in M10K (no cache coherence issues).
 * CPU writes ring data via MMIO (GPU_RING_DATA, auto-increment).
 * CPU kicks GPU by writing GPU_RING_WRPTR.
 *
 * IMPORTANT: This header contains static mutable state (_gpu_wrptr, etc).
 * Include it from exactly ONE translation unit per program.
 */

#ifndef OF_GPU_H
#define OF_GPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

#ifndef OF_PC
#include "of_caps.h"
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define OF_GPU_CLEAR_COLOR      (1 << 0)
#define OF_GPU_CLEAR_DEPTH      (1 << 1)

#define OF_GPU_RING_SIZE        16384   /* 16 KB M10K BRAM ring */

/* Fixed-point helpers */
#define OF_GPU_FIXED_16_16(x)   ((int32_t)((x) * 65536))   /* float → 16.16 */
#define OF_GPU_SUBPIXEL(x)      ((int16_t)((x) * 16))       /* pixel → 12.4  */
#define OF_GPU_FP16(x)          OF_GPU_FIXED_16_16(x)
#define OF_GPU_SC(x)            OF_GPU_SUBPIXEL(x)

/* ================================================================
 * Enumerations
 * ================================================================ */

typedef enum {
    OF_GPU_DEPTH_NONE   = 0,
    OF_GPU_DEPTH_ALWAYS = 1,
    OF_GPU_DEPTH_LESS   = 2,
    OF_GPU_DEPTH_LEQUAL = 3,
    OF_GPU_DEPTH_EQUAL  = 4,
} of_gpu_depth_func_t;

typedef enum {
    OF_GPU_BLEND_NONE  = 0,
    OF_GPU_BLEND_ALPHA = 1,
    OF_GPU_BLEND_ADD   = 2,
} of_gpu_blend_t;

typedef enum {
    OF_GPU_TEXFMT_I8     = 0,
    OF_GPU_TEXFMT_RGB565 = 1,
} of_gpu_texfmt_t;

typedef enum {
    OF_GPU_WRAP_REPEAT = 0,
    OF_GPU_WRAP_CLAMP  = 1,
} of_gpu_wrap_t;

/* ================================================================
 * Span Flags
 * ================================================================ */

#define OF_GPU_SPAN_COLORMAP    (1 << 0)
#define OF_GPU_SPAN_COLUMN      (1 << 1)
#define OF_GPU_SPAN_SKIP_ZERO   (1 << 2)
#define OF_GPU_SPAN_DEPTH_TEST  (1 << 3)
#define OF_GPU_SPAN_DEPTH_WRITE (1 << 4)
#define OF_GPU_SPAN_PERSP       (1 << 5)

/* ================================================================
 * Data Structures
 * ================================================================ */

typedef struct {
    uint32_t fb_addr;
    uint32_t tex_addr;
    int32_t  s, t;
    int32_t  sstep, tstep;
    uint16_t count;
    uint8_t  light;
    uint8_t  flags;
    int16_t  fb_stride;
    uint16_t tex_width;
    uint8_t  tex_shift;
    uint8_t  tex_bits;
    uint32_t z_addr;
    int32_t  zi;
    int32_t  zistep;
    /* Perspective (optional, requires PERSP flag) */
    int32_t  sdivz, tdivz;
    int32_t  zi_persp;
    int32_t  sdivz_step, tdivz_step;
    int32_t  zi_step;
} of_gpu_span_t;

typedef struct {
    uint32_t        addr;
    uint16_t        width;
    uint16_t        height;
    of_gpu_texfmt_t format;
    of_gpu_wrap_t   wrap_s;
    of_gpu_wrap_t   wrap_t;
} of_gpu_texture_t;

typedef struct {
    int16_t  x, y;          /* Screen position, 12.4 fixed-point */
    uint16_t z;             /* Depth: 0 = near, 0xFFFF = far */
    uint16_t pad;
    int32_t  s, t;          /* Texture coordinates, 16.16 fixed-point */
    int32_t  w;             /* 1/W for perspective (0x10000 = affine) */
    uint8_t  r, g, b, a;   /* Vertex color / light / alpha */
} of_gpu_vertex_t;          /* 24 bytes = 6 words */

/* ================================================================
 * MMIO Registers
 * ================================================================ */

#ifndef OF_PC

/* GPU MMIO base. The kernel reports the per-target address via the
 * of_capabilities descriptor; of_gpu_init() reads it once and caches
 * it in _gpu_base, so the GPU register macros below dereference a
 * file-static variable instead of a hardcoded immediate. This is what
 * lets the same SDK app .elf run on a target whose GPU window sits
 * at a different CPU address. */
static uint32_t _gpu_base;

#define OF_GPU_REG(off)         (*(volatile uint32_t *)(_gpu_base + (off)))

#define GPU_CTRL                OF_GPU_REG(0x00)  /* W: bit0=enable, bit1=soft_reset, bit2=ring_reset */
#define GPU_RING_WRPTR          OF_GPU_REG(0x04)  /* W: CPU write pointer (byte offset) — kicks GPU */
#define GPU_RING_DATA           OF_GPU_REG(0x08)  /* W: write next word to ring BRAM (auto-increment) */
#define GPU_RING_RDPTR          OF_GPU_REG(0x10)  /* R: GPU read pointer */
#define GPU_STATUS              OF_GPU_REG(0x14)  /* R: {30'b0, ring_empty, busy} */
#define GPU_FENCE_REACHED       OF_GPU_REG(0x18)  /* R: last completed fence token */
#define GPU_STAT_PIXELS         OF_GPU_REG(0x1C)  /* R: pixel counter */
#define GPU_CMAP_ADDR           OF_GPU_REG(0x20)  /* W: colormap write address (auto-inc) */
#define GPU_CMAP_DATA           OF_GPU_REG(0x24)  /* W: colormap write data (byte) */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */
#define GPU_STAT_SPANS          OF_GPU_REG(0x2C)  /* R: span counter */

/* ================================================================
 * Command IDs
 * ================================================================ */

#define GPU_CMD_NOP             0x01
#define GPU_CMD_FENCE           0x02
#define GPU_CMD_CLEAR           0x10
#define GPU_CMD_SET_TEXTURE     0x20
#define GPU_CMD_SET_DEPTH_FUNC  0x21
#define GPU_CMD_SET_BLEND       0x22
#define GPU_CMD_SET_FB          0x23
#define GPU_CMD_SET_ZB          0x24
#define GPU_CMD_SET_SHADE       0x25
#define GPU_CMD_SET_ALPHA_REF   0x26
#define GPU_CMD_DRAW_TRIANGLES  0x30
#define GPU_CMD_DRAW_INDEXED    0x31
#define GPU_CMD_DRAW_SPAN       0x40
#define GPU_CMD_DRAW_SPANS      0x41

/* ================================================================
 * Ring Buffer State (app-side)
 *
 * Static mutable — include this header from one .c file only.
 * ================================================================ */

static uint32_t _gpu_wrptr;
static uint32_t _gpu_fence_next;

static const uint32_t _gpu_ring_mask = OF_GPU_RING_SIZE - 1;

/* ---- Internal helpers ---- */

static inline void _gpu_ring_ensure(uint32_t bytes) {
    while (((GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask) < bytes)
        ;
}

/* Write a word to the ring BRAM via MMIO (no cache issues). */
static inline void _gpu_ring_write(uint32_t w) {
    GPU_RING_DATA = w;
    _gpu_wrptr = (_gpu_wrptr + 4) & _gpu_ring_mask;
}

static inline void _gpu_cmd_header(uint8_t cmd, uint32_t payload_words) {
    _gpu_ring_ensure((1 + payload_words) * 4);
    _gpu_ring_write(((uint32_t)cmd << 24) | (payload_words & 0x00FFFFFF));
}

/* ================================================================
 * API Functions
 * ================================================================ */

static inline void of_gpu_init(void) {
    /* Resolve the GPU MMIO base from the runtime caps descriptor.
     * Must be called after main() (or after the SDK constructors run)
     * so _of_caps_ptr is populated. Apps that try to drive the GPU
     * before of_gpu_init() will dereference a NULL _gpu_base and
     * fault clearly. */
    _gpu_base = of_get_caps()->gpu_base;

    _gpu_wrptr = 0;
    _gpu_fence_next = 1;
    GPU_CTRL = 4;               /* ring_reset: clear wr_addr + wrptr + rdptr */
    GPU_RING_WRPTR = 0;
    GPU_CTRL = 1;               /* enable */
}

static inline void of_gpu_colormap_upload(const uint8_t *data, uint32_t size) {
    GPU_CMAP_ADDR = 0;
    const uint32_t *src32 = (const uint32_t *)data;
    uint32_t words = size >> 2;
    for (uint32_t i = 0; i < words; i++)
        GPU_CMAP_DATA = src32[i];
    for (uint32_t i = words << 2; i < size; i++)
        GPU_CMAP_DATA = data[i];
}

static inline void of_gpu_kick(void) {
    GPU_RING_WRPTR = _gpu_wrptr;
}

static inline uint32_t of_gpu_fence(void) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FENCE, 1);
    _gpu_ring_write(token);
    return token;
}

static inline uint32_t of_gpu_submit(void) {
    uint32_t token = of_gpu_fence();
    of_gpu_kick();
    return token;
}

static inline int of_gpu_fence_reached(uint32_t token) {
    return (int32_t)(GPU_FENCE_REACHED - token) >= 0;
}

static inline void of_gpu_wait(uint32_t token) {
    while (!of_gpu_fence_reached(token))
        ;
}

static inline void of_gpu_finish(void) {
    of_gpu_wait(of_gpu_submit());
}

static inline void of_gpu_shutdown(void) {
    of_gpu_finish();
    GPU_CTRL = 0;
}

static inline void of_gpu_nop(void) {
    _gpu_cmd_header(GPU_CMD_NOP, 0);
}

static inline uint32_t of_gpu_ring_free(void) {
    return (GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline int of_gpu_ring_can_fit(uint32_t bytes) {
    return of_gpu_ring_free() >= bytes;
}

/* ---- State commands ---- */

static inline void of_gpu_set_framebuffer(uint32_t addr, uint16_t stride) {
    _gpu_cmd_header(GPU_CMD_SET_FB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
}

static inline void of_gpu_set_zbuffer(uint32_t addr, uint16_t stride) {
    _gpu_cmd_header(GPU_CMD_SET_ZB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
}

static inline void of_gpu_depth_test(of_gpu_depth_func_t func) {
    _gpu_cmd_header(GPU_CMD_SET_DEPTH_FUNC, 1);
    _gpu_ring_write((uint32_t)func);
}

static inline void of_gpu_blend(of_gpu_blend_t mode) {
    _gpu_cmd_header(GPU_CMD_SET_BLEND, 1);
    _gpu_ring_write((uint32_t)mode);
}

static inline void of_gpu_alpha_ref(uint8_t ref) {
    _gpu_cmd_header(GPU_CMD_SET_ALPHA_REF, 1);
    _gpu_ring_write((uint32_t)ref);
}

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 4);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(((uint32_t)tex->width << 16) | tex->height);
    _gpu_ring_write(((uint32_t)tex->format << 16) | tex->wrap_s);
    _gpu_ring_write((uint32_t)tex->wrap_t);
}

static inline void of_gpu_shade_mode(int gouraud) {
    _gpu_cmd_header(GPU_CMD_SET_SHADE, 1);
    _gpu_ring_write(gouraud ? 1 : 0);
}

/* ---- Draw commands ---- */

static inline void of_gpu_clear(uint32_t flags, uint16_t color, uint16_t depth) {
    _gpu_cmd_header(GPU_CMD_CLEAR, 2);
    _gpu_ring_write((flags << 16) | color);
    _gpu_ring_write((uint32_t)depth);
}

/*
 * Draw a single span.  18 payload words: 9 core + 3 depth + 6 perspective.
 * All fields always transmitted; GPU ignores depth/perspective unless flagged.
 */
static inline void of_gpu_draw_span(const of_gpu_span_t *span) {
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN, 18);
    _gpu_ring_write(span->fb_addr);
    _gpu_ring_write(span->tex_addr);
    _gpu_ring_write((uint32_t)span->s);
    _gpu_ring_write((uint32_t)span->t);
    _gpu_ring_write((uint32_t)span->sstep);
    _gpu_ring_write((uint32_t)span->tstep);
    _gpu_ring_write(((uint32_t)span->count << 16) |
                    ((uint32_t)span->light << 8) |
                    ((uint32_t)span->flags));
    _gpu_ring_write(((uint32_t)(uint16_t)span->fb_stride << 16) |
                    (uint32_t)span->tex_width);
    _gpu_ring_write(((uint32_t)span->tex_shift << 8) |
                    (uint32_t)span->tex_bits);
    _gpu_ring_write(span->z_addr);
    _gpu_ring_write((uint32_t)span->zi);
    _gpu_ring_write((uint32_t)span->zistep);
    _gpu_ring_write((uint32_t)span->sdivz);
    _gpu_ring_write((uint32_t)span->tdivz);
    _gpu_ring_write((uint32_t)span->zi_persp);
    _gpu_ring_write((uint32_t)span->sdivz_step);
    _gpu_ring_write((uint32_t)span->tdivz_step);
    _gpu_ring_write((uint32_t)span->zi_step);
}

static inline void _gpu_write_vertex(const of_gpu_vertex_t *v) {
    _gpu_ring_write(((uint32_t)(uint16_t)v->x << 16) | (uint16_t)v->y);
    _gpu_ring_write(((uint32_t)v->z << 16));
    _gpu_ring_write((uint32_t)v->s);
    _gpu_ring_write((uint32_t)v->t);
    _gpu_ring_write((uint32_t)v->w);
    _gpu_ring_write(((uint32_t)v->a << 24) | ((uint32_t)v->b << 16) |
                    ((uint32_t)v->g << 8) | v->r);
}

/*
 * Draw triangles from a vertex array.
 * @param verts        Every 3 consecutive vertices form one triangle.
 * @param num_vertices Number of vertices (must be a multiple of 3).
 */
static inline void of_gpu_draw_triangles(const of_gpu_vertex_t *verts,
                                          uint32_t num_vertices) {
    _gpu_cmd_header(GPU_CMD_DRAW_TRIANGLES, 1 + num_vertices * 6);
    _gpu_ring_write(num_vertices);
    for (uint32_t i = 0; i < num_vertices; i++)
        _gpu_write_vertex(&verts[i]);
}

/* ---- Statistics ---- */

static inline uint32_t of_gpu_stat_pixels(void) { return GPU_STAT_PIXELS; }
static inline uint32_t of_gpu_stat_spans(void)  { return GPU_STAT_SPANS; }

#endif /* !OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_GPU_H */
