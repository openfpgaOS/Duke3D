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
#include "of_cache.h"   /* of_cache_clean_range() — used by palookup upload */
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
    OF_GPU_DEPTH_NONE     = 0,
    OF_GPU_DEPTH_ALWAYS   = 1,
    OF_GPU_DEPTH_LESS     = 2,
    OF_GPU_DEPTH_LEQUAL   = 3,
    OF_GPU_DEPTH_EQUAL    = 4,
    OF_GPU_DEPTH_GEQUAL   = 5,
    OF_GPU_DEPTH_GREATER  = 6,
    OF_GPU_DEPTH_NOTEQUAL = 7,
} of_gpu_depth_func_t;

/* ================================================================
 * Span Flags
 * ================================================================ */

#define OF_GPU_SPAN_COLORMAP     (1 << 0)
/* bit 1 reserved (was OF_GPU_SPAN_COLUMN — never wired in the RTL) */
#define OF_GPU_SPAN_SKIP_ZERO    (1 << 2)
#define OF_GPU_SPAN_DEPTH_TEST   (1 << 3)
#define OF_GPU_SPAN_DEPTH_WRITE  (1 << 4)
#define OF_GPU_SPAN_PERSP        (1 << 5)
#define OF_GPU_SPAN_TRANSLUC     (1 << 6)
#define OF_GPU_SPAN_TRANSLUC_REV (1 << 7)

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
    /* POT wrap masks (tex_w - 1 / tex_h - 1).  0 means "no wrap" — the
     * legacy default and what the formerly-reserved word 8 carried.
     * Set both to (tex_w-1) and (tex_h-1) to reproduce BUILD/Quake-style
     * shift-mode wrap inside the GPU's multiply-mode addressing.  Both
     * dimensions must be powers of two.  Renamed from the dead
     * tex_shift/tex_bits fields that fed the retired shift-mode path. */
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
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
    uint32_t addr;
    uint16_t width;
    uint16_t height;
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
#define GPU_TRANSLUC_ADDR       OF_GPU_REG(0x20)  /* W: byte addr into transluc[] (auto-inc by 4) */
#define GPU_TRANSLUC_DATA       OF_GPU_REG(0x24)  /* W: 32-bit word into transluc[] */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */

/* ================================================================
 * Command IDs
 * ================================================================ */

#define GPU_CMD_NOP             0x01
#define GPU_CMD_FENCE           0x02
#define GPU_CMD_CLEAR           0x10
#define GPU_CMD_CLEAR_RECT      0x11  /* 3-word payload: start byte addr,
                                       * {w,h}, {pad,color}. Color's low
                                       * byte is replicated 4× per FB
                                       * word, matching CMD_CLEAR. */
#define GPU_CMD_SET_TEXTURE     0x20
#define GPU_CMD_SET_DEPTH_FUNC  0x21
#define GPU_CMD_SET_FB          0x23
#define GPU_CMD_SET_ZB          0x24
#define GPU_CMD_SET_COLORMAP_ID 0x28  /* 1-word payload: [3:0] = palookup slot */
#define GPU_CMD_DRAW_TRIANGLES  0x30
#define GPU_CMD_DRAW_SPAN       0x40

/* ================================================================
 * Palookup (colormap) layout in SDRAM — must match gpu_core.v's
 * PALOOKUP_BASE / PALOOKUP_STRIDE constants.
 *
 * Each slot holds a Quake/BUILD-shape shade × texel table.  Slot 0
 * is the default (used by callers that don't issue CMD_SET_COLORMAP_ID,
 * preserving single-palookup compatibility).  Up to 16 slots; the
 * GPU reads palookup[slot][shade][texel] from
 *   GPU_AXI_BASE + 0x100000 + slot*0x4000 + shade*256 + texel
 * via gpu_tex_cache port B (the prior on-chip cmap_bram is retired).
 *
 * The CPU-visible address depends on how the target maps the GPU's
 * AXI M0 into the CPU address space — apps should obtain it via the
 * runtime caps descriptor and add the per-slot offset.  These
 * constants encode the GPU-side AXI offset (0x100000) and per-slot
 * stride; the kernel's caps descriptor adds the per-target physical
 * base.  The lookup is target-portable as long as the kernel
 * advertises a `palookup_base` field that maps the same 26-bit
 * GPU AXI offset.
 * ================================================================ */
#define OF_GPU_PALOOKUP_AXI_OFFSET 0x00100000u  /* GPU AXI M0 byte addr of slot 0 */
#define OF_GPU_PALOOKUP_STRIDE     0x00004000u  /* 16 KB per slot */
#define OF_GPU_PALOOKUP_SLOTS      16

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
    /* Fast path: enough free space already, return immediately. */
    if (((GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask) >= bytes)
        return;

    /* Slow path: ring is full. Publish our current write pointer to
     * the GPU first — otherwise the GPU is sitting at its old wrptr
     * with nothing to drain, and we'd spin forever. (The GPU only
     * starts consuming commands when wrptr advances; queuing many
     * spans without an intervening kick deadlocks the producer when
     * the ring fills.) Then spin until enough space frees up. */
    GPU_RING_WRPTR = _gpu_wrptr;
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

/* Upload a palookup table to slot N in SDRAM.  The GPU reads palookup
 * bytes through gpu_tex_cache port B; this helper writes the table
 * directly to the SDRAM region the cache pulls from, then flushes the
 * CPU L1 lines so the GPU sees committed data.  16 KB per slot, up to
 * 16 slots (cf. OF_GPU_PALOOKUP_*).
 *
 * Slot selection at draw time is sticky: call of_gpu_set_colormap_id()
 * to switch.  Reset default is slot 0, so single-palookup apps just
 * call of_gpu_palookup_upload(0, …) and never issue a SET. */
static inline void of_gpu_palookup_upload(uint8_t slot, const uint8_t *data,
                                           uint32_t size) {
    if (slot >= OF_GPU_PALOOKUP_SLOTS || size > OF_GPU_PALOOKUP_STRIDE) return;
    uint32_t sdram_base = of_get_caps()->sdram_base;
    if (sdram_base == 0) return;  /* target without exposed SDRAM */
    uint8_t *dst = (uint8_t *)(sdram_base
                              + OF_GPU_PALOOKUP_AXI_OFFSET
                              + (uint32_t)slot * OF_GPU_PALOOKUP_STRIDE);
    /* Plain memcpy — palookup uploads are level-load events, not per-
     * frame.  The of_cache_clean_range that follows ensures the writes
     * reach SDRAM before the GPU's next tex_cache fill consumes them. */
    for (uint32_t i = 0; i < size; i++) dst[i] = data[i];
    of_cache_clean_range(dst, size);
}

/* See SDK of_gpu.h for full documentation.  Decimates BUILD's 64 KB
 * transluc[256][256] to the fabric's 32 KB / 128×256 quantised LUT
 * (low bit of source axis dropped) during the upload. */
/* Select the active palookup slot for subsequent SPAN_COLORMAP draws.
 * Sticky state — stays in effect until the next of_gpu_set_colormap_id().
 * Default at GPU reset is slot 0 (matching the legacy single-palookup
 * behaviour). */
static inline void of_gpu_set_colormap_id(uint8_t slot) {
    _gpu_cmd_header(GPU_CMD_SET_COLORMAP_ID, 1);
    _gpu_ring_write((uint32_t)(slot & 0xF));
}

static inline void of_gpu_translucency_upload(const uint8_t *table, uint32_t size) {
    if (size != 65536) return;
    GPU_TRANSLUC_ADDR = 0;
    for (int s7 = 0; s7 < 128; s7++) {
        const uint8_t *row = &table[(s7 << 1) << 8];
        const uint32_t *row32 = (const uint32_t *)row;
        for (int w = 0; w < 64; w++)
            GPU_TRANSLUC_DATA = row32[w];
    }
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
    /* Bounded spin — if the GPU hangs (fb_acc never flushes, tex cache
     * stuck in fill, pipeline deadlocked), the old unbounded spin
     * silently froze the machine with no diagnostic.  Timeout triggers
     * an illegal-instruction trap so fatal_trap dumps the GPU state;
     * the registers to inspect on the trap side are:
     *   GPU_STATUS    (0x14) — busy + ring_empty
     *   GPU_RING_RDPTR (0x10) — where the GPU last stopped fetching
     *
     * Uses a plain iteration counter rather than a cycle CSR: this
     * VexiiRiscv build is compiled without --performance-counters so
     * rdcycle / mcycle both trap illegal-instruction (mtval=0xc8002873
     * / 0xb8002873 observed).  Iteration count of 50M with a ~10-cycle
     * body gives roughly ~5 s of wait at 100 MHz — generous enough
     * that normal fence completions always beat it, tight enough that
     * a genuine hang surfaces quickly. */
    uint32_t spins = 50000000u;
    while (!of_gpu_fence_reached(token)) {
        if (--spins == 0) {
            __builtin_trap();  /* → illegal-instruction trap, mcause=2 */
        }
    }
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

/* of_gpu_blend / of_gpu_alpha_ref / of_gpu_shade_mode helpers removed
 * along with their underlying SET_BLEND / SET_ALPHA_REF / SET_SHADE
 * commands — the datapath never implemented the corresponding combine,
 * alpha-test, or Gouraud gradient logic. */

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 2);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(((uint32_t)tex->width << 16) | tex->height);
}

/* ---- Draw commands ---- */

static inline void of_gpu_clear(uint32_t flags, uint16_t color, uint16_t depth) {
    _gpu_cmd_header(GPU_CMD_CLEAR, 2);
    _gpu_ring_write((flags << 16) | color);
    _gpu_ring_write((uint32_t)depth);
}

/* Clear a rectangular region of the framebuffer to a constant color.
 * Caller computes the start byte address (fb_base + y*stride + x); the
 * GPU walks `h` rows × `w` bytes from there, advancing each row by the
 * active st_fb_stride.  Color's low byte is replicated 4× per FB word
 * (matches CMD_CLEAR's shape).  Word-aligned full-width strips
 * (letterbox / status bar) hit the 4-byte fast path; arbitrary x/w
 * paths byte-strobe the partial-word edges.  Used to retire the last
 * per-frame CPU memset(frameplace, …) categories — see
 * project_gpu_owns_framebuffer.md. */
static inline void of_gpu_clear_rect(uint32_t start_byte_addr,
                                      uint16_t w, uint16_t h,
                                      uint8_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(start_byte_addr);
    _gpu_ring_write(((uint32_t)w << 16) | (uint32_t)h);
    _gpu_ring_write((uint32_t)color);
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
    /* Word 8: POT wrap masks (high 16 = T, low 16 = S).  Both = 0
     * means no wrap (legacy callers).  RTL decodes 0 as 0xFFFF
     * internally so the addressing pass-through is unchanged. */
    _gpu_ring_write(((uint32_t)span->tex_h_mask << 16) |
                    (uint32_t)span->tex_w_mask);
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
 *
 * Emits one CMD_DRAW_TRIANGLES per triangle (1 count word + 3 verts ×
 * 6 words = 19 payload words each).  Suitable when the surrounding
 * GPU state changes between triangles (e.g. per-triangle texture).
 */
static inline void of_gpu_draw_triangles(const of_gpu_vertex_t *verts,
                                          uint32_t num_vertices) {
    for (uint32_t i = 0; i < num_vertices; i += 3) {
        _gpu_cmd_header(GPU_CMD_DRAW_TRIANGLES, 19);
        _gpu_ring_write(3);
        _gpu_write_vertex(&verts[i + 0]);
        _gpu_write_vertex(&verts[i + 1]);
        _gpu_write_vertex(&verts[i + 2]);
    }
}

/*
 * Draw N triangles in a single batched DRAW_TRIANGLES command.
 *
 * Payload layout: 1 count word + N × 18 vertex words (6 per vertex).
 * The GPU FSM renders each triangle as it streams in and re-enters the
 * payload loop for the next one, so the only difference from the
 * per-triangle helper above is one cmd_header + cmd_decode pass per
 * batch instead of per triangle.
 *
 * Constraint: every triangle in the batch shares the currently bound
 * texture and other GPU state.  Group triangles by state and submit
 * each group as one batch to amortise the command overhead.
 */
static inline void of_gpu_draw_triangles_batch(const of_gpu_vertex_t *verts,
                                                uint32_t num_vertices) {
    if (num_vertices < 3 || (num_vertices % 3) != 0) return;
    _gpu_cmd_header(GPU_CMD_DRAW_TRIANGLES, 1 + num_vertices * 6);
    _gpu_ring_write(num_vertices);
    for (uint32_t i = 0; i < num_vertices; i++)
        _gpu_write_vertex(&verts[i]);
}

#endif /* !OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_GPU_H */
