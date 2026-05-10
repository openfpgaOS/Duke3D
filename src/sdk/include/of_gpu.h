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
#include <stdlib.h>     /* malloc — used by of_gpu_init for the batch scratch buffer */

#ifndef OF_PC
#include "of_caps.h"
#include "of_cache.h"   /* of_cache_clean_range() — used by palookup upload + batch */
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define OF_GPU_CLEAR_COLOR      (1 << 0)
/* bit 1 reserved (was OF_GPU_CLEAR_DEPTH — Z buffer dropped) */

#define OF_GPU_RING_SIZE        16384   /* 16 KB M10K BRAM ring */

/* Fixed-point helpers */
#define OF_GPU_FIXED_16_16(x)   ((int32_t)((x) * 65536))   /* float → 16.16 */
#define OF_GPU_SUBPIXEL(x)      ((int16_t)((x) * 16))       /* pixel → 12.4  */
#define OF_GPU_FP16(x)          OF_GPU_FIXED_16_16(x)
#define OF_GPU_SC(x)            OF_GPU_SUBPIXEL(x)

/* ================================================================
 * Span Flags
 * ================================================================ */

#define OF_GPU_SPAN_COLORMAP     (1 << 0)
/* bit 1 reserved (was OF_GPU_SPAN_COLUMN — never wired in the RTL) */
#define OF_GPU_SPAN_SKIP_ZERO    (1 << 2)
/* bits 3/4 reserved (were SPAN_DEPTH_TEST/WRITE — Z buffer dropped) */
#define OF_GPU_SPAN_PERSP        (1 << 5)
#define OF_GPU_SPAN_TRANSLUC     (1 << 6)
/* bit 7 reserved (was OF_GPU_SPAN_TRANSLUC_REV — REV variant dropped) */

/* ================================================================
 * Data Structures
 * ================================================================ */

typedef struct {
    uint32_t fb_addr;
    uint32_t tex_addr;
    int32_t  s, t;
    int32_t  sstep, tstep;
    /* Pixel count.  Scalar DRAW_SPAN's wire count is 12 bits at word 6
     * [27:16], so helpers mask this to 0..4095.  Callers that need
     * longer scalar spans should decompose them before submission. */
    uint16_t count;
    uint8_t  light;
    uint8_t  flags;
    /* Word 6 [31:28]. 0 means use sticky GPU_CMD_SET_COLORMAP_ID state;
     * 1..15 explicitly select a palookup slot for this span. */
    uint8_t  colormap_id;
    int16_t  fb_stride;
    uint16_t tex_width;
    /* POT wrap masks (tex_w - 1 / tex_h - 1).  0 means "no wrap" — the
     * default for shift-free callers.  Set both to (tex_w-1) and
     * (tex_h-1) to reproduce BUILD/Quake-style shift-mode wrap inside
     * the GPU's multiply-mode addressing.  Both dimensions must be POT. */
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
    /* Perspective (optional, requires PERSP flag) */
    int32_t  sdivz, tdivz;
    int32_t  zi_persp;
    int32_t  sdivz_step, tdivz_step;
    int32_t  zi_step;
} of_gpu_span_t;

typedef struct {
    uint32_t fb_addr;        /* framebuffer byte address for lane 0 */
    uint32_t tex_addr[4];    /* per-lane texture/column base */
    int32_t  t[4];           /* per-lane Q16.16 T coordinate */
    int32_t  tstep[4];       /* per-lane Q16.16 T step */
    uint16_t count;          /* pixels per lane */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by all lanes */
    uint8_t  colormap_id;    /* explicit slot, including slot 0 */
    int16_t  fb_stride;      /* row stride between vertical pixels */
    uint16_t tex_width;      /* row pitch for tex_addr + t*tex_width */
    uint16_t tex_w_mask;     /* POT wrap mask for S, 0 means no wrap */
    uint16_t tex_h_mask;     /* POT wrap mask for T, 0 means no wrap */
    uint8_t  light[4];       /* per-lane palookup row */
} of_gpu_span4_t;

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
    /* r = light index into the active palookup slot.  Only `r` of v0
     * is sampled (flat shading per triangle).  CMD_DRAW_TRIANGLES
     * fragments ALWAYS route through palookup[colormap_id][r][texel];
     * any `r` value used MUST have its row populated, or the fragment
     * renders 0x00.  Convention: load row 0 with identity (cm[i]=i)
     * so r=0 means "raw textured / unlit". */
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
#define GPU_DMA_SRC             OF_GPU_REG(0x0C)  /* W: SDRAM byte address of command buffer to pull */
#define GPU_RING_RDPTR          OF_GPU_REG(0x10)  /* R: GPU read pointer */
#define GPU_STATUS              OF_GPU_REG(0x14)  /* R: bit2=upload busy, bit1=ring empty, bit0=busy */
#define GPU_FENCE_REACHED       OF_GPU_REG(0x18)  /* R: last completed fence token */
#define GPU_DMA_LEN             OF_GPU_REG(0x1C)  /* W: word count to pull (≤4096) */
#define GPU_TRANSLUC_ADDR       OF_GPU_REG(0x20)  /* W: byte addr into transluc[] (auto-inc by 4) */
#define GPU_TRANSLUC_DATA       OF_GPU_REG(0x24)  /* W: 32-bit word into transluc[] */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */
#define GPU_DMA_KICK            OF_GPU_REG(0x2C)  /* W: write 1 to fire DMA pull from (SRC, LEN) */
#define GPU_DMA_DBG             OF_GPU_REG(0x38)  /* R: compact DMA diagnostic status */
#define GPU_DBG_SELECT          OF_GPU_REG(0x3C)  /* area mode: legacy stall counter reads zero */

/* Texture-cache diagnostic counters may be compact in small bitstreams.  Consumers
 * should compute deltas modulo this mask. */
#define OF_GPU_TEX_DBG_COUNTER_BITS 20u
#define OF_GPU_TEX_DBG_COUNTER_MASK ((1u << OF_GPU_TEX_DBG_COUNTER_BITS) - 1u)

/* GPU_STATUS bit definitions */
#define GPU_STATUS_BUSY        0x1u
#define GPU_STATUS_RING_EMPTY  0x2u
#define GPU_STATUS_DMA_BUSY    0x4u  /* SDRAM command/payload DMA busy */

enum {
    OF_GPU_STALL_TEX_WAIT = 0,
    OF_GPU_STALL_CMAP_WAIT,
    OF_GPU_STALL_CMAP_ISSUE,
    OF_GPU_STALL_FBSS_BUSY,
    OF_GPU_STALL_FB_WRITE,
    OF_GPU_STALL_INFLIGHT,
    OF_GPU_STALL_PERSP_WAIT,
    OF_GPU_STALL_COUNT
};

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
/* 0x21 GPU_CMD_SET_DEPTH_FUNC retired in lean Phase 2.3 (Z dropped). */
#define GPU_CMD_SET_FB          0x23
/* 0x24 GPU_CMD_SET_ZB         retired in lean Phase 2.3 (Z dropped). */
#define GPU_CMD_SET_COLORMAP_ID 0x28  /* 1-word payload: [3:0] = palookup slot */
#define GPU_CMD_DRAW_TRIANGLES  0x30
#define GPU_CMD_DRAW_SPAN       0x40
/* CMD_DRAW_SPANS_BATCH: header carries `15*N` payload words (no count
 * word — N derives from header).  Decoder loops the existing CMD_DRAW_SPAN
 * fragment-pipe path once per 15-word span.  Saves the per-span MMIO
 * header + (when paired with the doorbell-DMA path) lets the CPU stream
 * the whole batch as cached scalar stores into SDRAM, then kick the GPU
 * to pull it via its own AXI master — eliminates the 120 ns/word MMIO
 * stall that dominates per-span dispatch cost. */
#define GPU_CMD_DRAW_SPANS_BATCH 0x41
/* GPU-triggered display flip (cr-gpu-triggered-flip.md).  2-word payload:
 *   word 0: bits[1:0] = back-buffer index (0/1/2 → FB_ADDR_{0,1,2})
 *   word 1: fence token (published to GPU_FENCE_REACHED after the swap)
 * Drains all outstanding m_wr_* writes (same primitive as the upgraded
 * CMD_FENCE), pulses the swap side-port to axi_periph_slave for one
 * cycle, then publishes the fence token. */
#define GPU_CMD_FLIP             0x42
#define GPU_CMD_DRAW_SPAN4       0x43  /* 17-word compact four-lane column */
#define GPU_CMD_DRAW_SPAN4_BATCH 0x44  /* payload = 17*N compact columns */

/* Maximum spans per CMD_DRAW_SPANS_BATCH dispatch.  At 15 words/span
 * that's 1920 words = 7680 bytes per batch — fits comfortably in the
 * 16 KB ring with room for state-change commands.  of_gpu_draw_spans_batch
 * splits longer arrays across multiple kicks.  Exposed so callers
 * (e.g. game-side accumulators) can size their own buffers to flush at
 * the same boundary the SDK helper uses internally. */
#define OF_GPU_BATCH_MAX_SPANS  128
#define OF_GPU_SPAN4_WORDS      17u
#define OF_GPU_SPAN4_BATCH_MAX  128

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
/* GPU AXI M0 byte addr of palookup slot 0.  Was 0x00100000 but that
 * collided with OF_TARGET_FB1_BASE = 0x10100000, so every FB1 frame
 * overwrote the palookup table.  Moved to the 3 MB gap between heap
 * end (0x13400000) and the audio sample pool (0x13700000).  MUST stay
 * in sync with PALOOKUP_BASE in src/fpga/common/gpu_core.v. */
#define OF_GPU_PALOOKUP_AXI_OFFSET 0x03400000u
#define OF_GPU_PALOOKUP_STRIDE     0x00004000u  /* 16 KB per slot */
#define OF_GPU_PALOOKUP_SLOTS      16

/* Doorbell-DMA scratch region — must live in SDRAM because gpu_core's
 * m_rd_* AXI master only reaches the SDRAM arbiter (see core_top.v's
 * sdram_arb instantiation: GPU is m0, no other targets are wired).
 * Placed right above the palookup window (0x100000..0x140000); 16 KB
 * reserved leaves headroom over the 10 KB mixed command-stream payload. */
#define OF_GPU_BATCH_BUF_AXI_OFFSET  0x00140000u
#define OF_GPU_BATCH_BUF_BYTES       0x00004000u  /* 16 KB reserved */

/* ================================================================
 * Ring Buffer State (app-side)
 *
 * Static mutable — include this header from one .c file only.
 * ================================================================ */

static uint32_t _gpu_wrptr;
static uint32_t _gpu_fence_next;

static const uint32_t _gpu_ring_mask = OF_GPU_RING_SIZE - 1;

/* Doorbell-DMA scratch buffer.  Pinned to a fixed SDRAM offset by
 * of_gpu_init — gpu_core's m_rd_* AXI master only reaches the SDRAM
 * arbiter, so the scratch must live in SDRAM.  Apps' malloc heap
 * starts in CRAM0 territory and would be unreachable to the GPU.
 * NULL on targets that don't expose SDRAM via caps; the helper then
 * falls back to per-span MMIO. */
static uint32_t *_gpu_batch_buf;
static uint32_t  _gpu_batch_slot;
static uint32_t  _gpu_dbg_dma_waits;
static uint32_t  _gpu_dbg_dma_spin_iters;
static uint32_t  _gpu_dbg_ring_waits;
static uint32_t  _gpu_dbg_ring_spin_iters;
static uint32_t  _gpu_dbg_min_ring_free;

#define OF_GPU_BATCH_WORDS_PER_SPAN   15u
#define OF_GPU_COMMAND_STREAM_BATCH_WORDS 2560u  /* 10 KB raw mixed stream */
#define OF_GPU_BATCH_SLOT_WORDS       OF_GPU_COMMAND_STREAM_BATCH_WORDS
#define OF_GPU_BATCH_SLOT_BYTES       (OF_GPU_BATCH_SLOT_WORDS * 4u)
#define OF_GPU_BATCH_SLOTS            (OF_GPU_BATCH_BUF_BYTES / OF_GPU_BATCH_SLOT_BYTES)

#if OF_GPU_BATCH_SLOTS < 1
#error "OF_GPU_BATCH_BUF_BYTES must hold at least one GPU batch slot"
#endif

#if (OF_GPU_BATCH_MAX_SPANS * OF_GPU_BATCH_WORDS_PER_SPAN) > OF_GPU_BATCH_SLOT_WORDS
#error "OF_GPU_BATCH_SLOT_WORDS too small for scalar span batch"
#endif

#if (OF_GPU_SPAN4_BATCH_MAX * OF_GPU_SPAN4_WORDS) > OF_GPU_BATCH_SLOT_WORDS
#error "OF_GPU_BATCH_SLOT_WORDS too small for SPAN4 batch"
#endif

#if OF_GPU_BATCH_SLOT_BYTES > (OF_GPU_RING_SIZE - 4u)
#error "GPU batch slot must fit in the ring with guard space"
#endif

/* ---- Internal helpers ---- */

static inline void _gpu_wait_dma_idle_debug(void) {
    uint32_t dma_spins = 0;
    while (GPU_STATUS & GPU_STATUS_DMA_BUSY)
        dma_spins++;
    if (dma_spins) {
        _gpu_dbg_dma_waits++;
        _gpu_dbg_dma_spin_iters += dma_spins;
    }
}

static inline void _gpu_ring_ensure(uint32_t bytes) {
    /* Wait for any in-flight doorbell-DMA before letting the caller
     * write into the ring.  CPU MMIO writes and DMA writes share
     * ring_wr_addr in gpu_core.v — overlapping them shifts DMA's
     * payload by however many CPU writes interleave.  By gating
     * here, every command-emitting call site (header + payload + DMA
     * kick) is implicitly serialized against the previous batch's
     * DMA fill, while of_gpu_draw_spans_batch can return immediately
     * after kicking — overlapping the DMA's ~23 µs fill with the
     * caller's between-batch CPU work. */
    _gpu_wait_dma_idle_debug();

    /* Fast path: enough free space already, return immediately. */
    uint32_t ring_free = (GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask;
    if (ring_free < _gpu_dbg_min_ring_free)
        _gpu_dbg_min_ring_free = ring_free;
    if (ring_free >= bytes)
        return;

    /* Slow path: ring is full.  Publish our current write pointer to
     * the GPU first — otherwise the GPU is sitting at its old wrptr
     * with nothing to drain, and we'd spin forever. */
    GPU_RING_WRPTR = _gpu_wrptr;
    {
        uint32_t ring_spins = 0;
        do {
            ring_free = (GPU_RING_RDPTR - _gpu_wrptr - 4) & _gpu_ring_mask;
            if (ring_free < _gpu_dbg_min_ring_free)
                _gpu_dbg_min_ring_free = ring_free;
            if (ring_free >= bytes)
                break;
            ring_spins++;
        } while (1);
        _gpu_dbg_ring_waits++;
        _gpu_dbg_ring_spin_iters += ring_spins;
    }
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
    _gpu_batch_slot = 0;
    _gpu_dbg_dma_waits = 0;
    _gpu_dbg_dma_spin_iters = 0;
    _gpu_dbg_ring_waits = 0;
    _gpu_dbg_ring_spin_iters = 0;
    _gpu_dbg_min_ring_free = OF_GPU_RING_SIZE;
    GPU_CTRL = 4;               /* ring_reset: clear wr_addr + wrptr + rdptr */
    GPU_RING_WRPTR = 0;
    GPU_CTRL = 1;               /* enable */

    /* Pin the doorbell-DMA scratch buffer at a known SDRAM offset.
     * gpu_core's m_rd_* AXI master goes only to the SDRAM arbiter
     * (core_top.v: sdram_arb m0) so the scratch MUST live in SDRAM —
     * a malloc()'d pointer that early-init resolved to CRAM0 (where
     * apps' .text/.data live) is unreachable and the DMA would pull
     * garbage.  Same scheme palookup uses (caps->sdram_base +
     * OF_GPU_PALOOKUP_AXI_OFFSET); we sit one window above. */
    {
        const struct of_capabilities *caps = of_get_caps();
        if (caps && caps->sdram_base != 0)
            _gpu_batch_buf = (uint32_t *)(uintptr_t)
                (caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET);
        else
            _gpu_batch_buf = NULL;   /* helper falls back to per-MMIO */
    }
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
    const struct of_capabilities *caps = of_get_caps();
    if (caps->sdram_base == 0) return;  /* target without exposed SDRAM */
    /* Write through the uncached SDRAM alias so the bytes hit DRAM
     * directly — no D-cache pollution, no flush dependency.  The GPU's
     * tex_cache port B reads the same physical bytes via AXI; on the
     * first fill it sees the committed data unconditionally.
     *
     * Use 32-bit word writes, not single-byte writes.  Pocket's SDRAM
     * controller passes wstrb through, but the 16-bit PHY behind it
     * resolves byte-strobed writes via a read-modify-write path that
     * has produced "palookup mostly zeros" symptoms in practice — the
     * GPU then sees a near-uniform table and every pixel resolves to
     * the same palette index regardless of (texel, shade), which reads
     * on screen as a uniform colour with no fade.  Full-word writes
     * sidestep the RMW path; the SDRAM slave's [25:2] address decode
     * means consecutive uint32_t writes hit consecutive SDRAM words.
     *
     * The prior cached + cache_clean version had the same class of
     * stale-data bug; uncached alias is the right destination, just
     * in 32-bit chunks rather than bytes. */
    uint32_t cached_base = caps->sdram_base
                         + OF_GPU_PALOOKUP_AXI_OFFSET
                         + (uint32_t)slot * OF_GPU_PALOOKUP_STRIDE;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)
        ((cached_base - caps->sdram_base) + caps->sdram_uncached_base);
    const uint32_t *src = (const uint32_t *)data;
    uint32_t words = size >> 2;
    for (uint32_t i = 0; i < words; i++) dst[i] = src[i];
    /* Tail bytes (size not a multiple of 4) — fold into a final word
     * so we still write every byte the caller passed.  Pad the unused
     * lanes with zero rather than skipping, so the SDRAM word is
     * fully defined regardless of whatever was there before. */
    uint32_t tail = size & 3u;
    if (tail) {
        uint32_t w = 0;
        const uint8_t *tb = data + (words << 2);
        for (uint32_t i = 0; i < tail; i++)
            w |= ((uint32_t)tb[i]) << (i * 8);
        dst[words] = w;
    }
}

/* Select the active palookup slot for subsequent SPAN_COLORMAP draws.
 * Sticky state — stays in effect until the next of_gpu_set_colormap_id().
 * Default at GPU reset is slot 0 (matching the legacy single-palookup
 * behaviour). */
static inline void of_gpu_set_colormap_id(uint8_t slot) {
    _gpu_cmd_header(GPU_CMD_SET_COLORMAP_ID, 1);
    _gpu_ring_write((uint32_t)(slot & 0xF));
}

/* Decimates BUILD's 64 KB transluc[256][256] to the fabric's 32 KB
 * / 128×256 quantised LUT (low bit of source axis dropped) during the
 * upload. */
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

/* GPU-triggered page flip — emits CMD_FLIP into the ring with the
 * given back-buffer index and a fresh fence token.  The GPU's command
 * processor drains all outstanding m_wr_* writes, pulses the swap
 * side-port to the display controller (queued for next vsync), and
 * publishes the fence token to GPU_FENCE_REACHED.  Non-blocking — the
 * returned token proves the swap request reached the display
 * controller, not that the next vsync has presented it.
 *
 * Pair with the kernel's of_video_acquire_next(idx) to get the next
 * free draw buffer.  See docs/cr-gpu-triggered-flip.md for the
 * standard call pattern. */
/* CMD_FLIP re-enabled with diagnostic counters in place (2026-04-30).
 * The kernel side of_video_acquire_next() retains a bounded fence-wait
 * and only uses a CPU FB_SWAP_CTRL write as a timeout fallback, so a
 * healthy CMD_FLIP path stays non-blocking and does not double-kick
 * the same swap. */
static inline uint32_t of_gpu_flip_to(int idx) {
    uint32_t token = _gpu_fence_next++;
    _gpu_cmd_header(GPU_CMD_FLIP, 2);
    _gpu_ring_write((uint32_t)idx & 0x3u);
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

typedef struct {
    uint32_t status;
    uint32_t rdptr;
    uint32_t wrptr;
    uint32_t fence_reached;
    uint32_t tex_req_count;
    uint32_t tex_miss_count;
    uint32_t stall_count[OF_GPU_STALL_COUNT];
    uint32_t dma_waits;
    uint32_t dma_spin_iters;
    uint32_t ring_waits;
    uint32_t ring_spin_iters;
    uint32_t min_ring_free;
    uint32_t ring_free;
} of_gpu_debug_snapshot_t;

static inline void of_gpu_debug_snapshot(of_gpu_debug_snapshot_t *snap,
                                         int reset_wait_counters) {
    if (snap == NULL)
        return;

    memset(snap, 0, sizeof(*snap));
    snap->status = GPU_STATUS;
    snap->rdptr = GPU_RING_RDPTR;
    snap->wrptr = GPU_RING_WRPTR;
    snap->fence_reached = GPU_FENCE_REACHED;
    snap->tex_req_count = GPU_TRANSLUC_ADDR;
    snap->tex_miss_count = GPU_TRANSLUC_DATA;

    for (uint32_t i = 0; i < OF_GPU_STALL_COUNT; i++) {
        GPU_DBG_SELECT = i;
        snap->stall_count[i] = GPU_DBG_SELECT;
    }

    snap->ring_free = of_gpu_ring_free();
    snap->min_ring_free = _gpu_dbg_min_ring_free < snap->ring_free ?
        _gpu_dbg_min_ring_free : snap->ring_free;
    snap->dma_waits = _gpu_dbg_dma_waits;
    snap->dma_spin_iters = _gpu_dbg_dma_spin_iters;
    snap->ring_waits = _gpu_dbg_ring_waits;
    snap->ring_spin_iters = _gpu_dbg_ring_spin_iters;

    if (reset_wait_counters) {
        _gpu_dbg_dma_waits = 0;
        _gpu_dbg_dma_spin_iters = 0;
        _gpu_dbg_ring_waits = 0;
        _gpu_dbg_ring_spin_iters = 0;
        _gpu_dbg_min_ring_free = snap->ring_free;
    }
}

/* ---- State commands ---- */

static inline void of_gpu_set_framebuffer(uint32_t addr, uint16_t stride) {
    _gpu_cmd_header(GPU_CMD_SET_FB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
}

/* Z-buffer / depth-test API retired with the lean Z-removal in Phase 2.3.
 * Quake / SDL2 / Doom-style renderers do their own visibility (BSP /
 * paint-order); the GPU is now strictly a paint-order rasterizer. */

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 2);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(((uint32_t)tex->width << 16) | tex->height);
}

/* ---- Draw commands ---- */

/* Whole-FB clear.  flags bit 0 = clear color (the only flag still
 * accepted; the bit-1 depth-clear path was retired with the Z buffer). */
static inline void of_gpu_clear(uint32_t flags, uint16_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR, 2);
    _gpu_ring_write((flags << 16) | color);
    _gpu_ring_write(0);  /* word 1 reserved (was depth value) */
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

/* Strided clear_rect — word 2 of the payload carries the row stride at
 * bits [31:16].  When stride==0 the GPU falls back to the SET_FB-
 * resident global stride (matches plain of_gpu_clear_rect).  See
 * docs/cr-gpu-clear-rect-stride.md for the rationale. */
static inline void of_gpu_clear_rect_strided(uint32_t start_byte_addr,
                                              uint16_t w, uint16_t h,
                                              uint16_t stride,
                                              uint8_t color) {
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(start_byte_addr);
    _gpu_ring_write(((uint32_t)w << 16) | (uint32_t)h);
    _gpu_ring_write(((uint32_t)stride << 16) | (uint32_t)color);
}

/*
 * Draw a single span.  15 payload words: 9 core + 6 perspective.
 * GPU ignores the perspective words unless OF_GPU_SPAN_PERSP is set.
 */
static inline void of_gpu_draw_span(const of_gpu_span_t *span) {
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN, 15);
    _gpu_ring_write(span->fb_addr);
    _gpu_ring_write(span->tex_addr);
    _gpu_ring_write((uint32_t)span->s);
    _gpu_ring_write((uint32_t)span->t);
    _gpu_ring_write((uint32_t)span->sstep);
    _gpu_ring_write((uint32_t)span->tstep);
    _gpu_ring_write((((uint32_t)span->colormap_id & 0xFu) << 28) |
                    (((uint32_t)span->count & 0x0FFFu) << 16) |
                    ((uint32_t)span->light << 8) |
                    ((uint32_t)span->flags));
    _gpu_ring_write(((uint32_t)(uint16_t)span->fb_stride << 16) |
                    (uint32_t)span->tex_width);
    /* Word 8: POT wrap masks (high 16 = T, low 16 = S).  Both = 0
     * means no wrap.  RTL decodes 0 as 0xFFFF internally. */
    _gpu_ring_write(((uint32_t)span->tex_h_mask << 16) |
                    (uint32_t)span->tex_w_mask);
    _gpu_ring_write((uint32_t)span->sdivz);
    _gpu_ring_write((uint32_t)span->tdivz);
    _gpu_ring_write((uint32_t)span->zi_persp);
    _gpu_ring_write((uint32_t)span->sdivz_step);
    _gpu_ring_write((uint32_t)span->tdivz_step);
    _gpu_ring_write((uint32_t)span->zi_step);
}

/* Encode one span into 15 words (the same field layout as of_gpu_draw_span's
 * payload, sans header).  Used by the batch helper to build the SDRAM
 * scratch buffer at scalar-store speed. */
static inline void _gpu_encode_span(uint32_t *p, const of_gpu_span_t *s) {
    p[0]  = s->fb_addr;
    p[1]  = s->tex_addr;
    p[2]  = (uint32_t)s->s;
    p[3]  = (uint32_t)s->t;
    p[4]  = (uint32_t)s->sstep;
    p[5]  = (uint32_t)s->tstep;
    p[6]  = (((uint32_t)s->colormap_id & 0xFu) << 28) |
            (((uint32_t)s->count & 0x0FFFu) << 16) |
            ((uint32_t)s->light << 8) |
            (uint32_t)s->flags;
    p[7]  = ((uint32_t)(uint16_t)s->fb_stride << 16) | (uint32_t)s->tex_width;
    p[8]  = ((uint32_t)s->tex_h_mask << 16) | (uint32_t)s->tex_w_mask;
    p[9]  = (uint32_t)s->sdivz;
    p[10] = (uint32_t)s->tdivz;
    p[11] = (uint32_t)s->zi_persp;
    p[12] = (uint32_t)s->sdivz_step;
    p[13] = (uint32_t)s->tdivz_step;
    p[14] = (uint32_t)s->zi_step;
}

/* Encode one compact four-lane vertical affine span.  The hardware
 * serialises lanes through the same fragment path as four scalar spans:
 * fb_addr + lane selects the adjacent destination column; tex_addr[lane],
 * t[lane], tstep[lane] and light[lane] select each column's source and
 * shade.  tex_w_mask/tex_h_mask match scalar span POT wrapping.  colormap_id
 * is explicit, including slot 0. */
static inline void _gpu_encode_span4(uint32_t *p, const of_gpu_span4_t *s) {
    p[0] = s->fb_addr;
    p[1] = ((uint32_t)s->count << 16) |
           ((uint32_t)s->flags << 8) |
           ((uint32_t)s->colormap_id & 0x0Fu);
    p[2] = ((uint32_t)(uint16_t)s->fb_stride << 16) |
           (uint32_t)s->tex_width;
    p[3] = s->tex_addr[0];
    p[4] = s->tex_addr[1];
    p[5] = s->tex_addr[2];
    p[6] = s->tex_addr[3];
    p[7] = (uint32_t)s->t[0];
    p[8] = (uint32_t)s->t[1];
    p[9] = (uint32_t)s->t[2];
    p[10] = (uint32_t)s->t[3];
    p[11] = (uint32_t)s->tstep[0];
    p[12] = (uint32_t)s->tstep[1];
    p[13] = (uint32_t)s->tstep[2];
    p[14] = (uint32_t)s->tstep[3];
    p[15] = ((uint32_t)s->light[3] << 24) |
            ((uint32_t)s->light[2] << 16) |
            ((uint32_t)s->light[1] << 8) |
            (uint32_t)s->light[0];
    p[16] = ((uint32_t)s->tex_h_mask << 16) |
            (uint32_t)s->tex_w_mask;
}

static inline void of_gpu_draw_span4(const of_gpu_span4_t *span) {
    uint32_t w[OF_GPU_SPAN4_WORDS];
    if (span == NULL) return;
    _gpu_encode_span4(w, span);
    _gpu_cmd_header(GPU_CMD_DRAW_SPAN4, OF_GPU_SPAN4_WORDS);
    for (uint32_t i = 0; i < OF_GPU_SPAN4_WORDS; i++)
        _gpu_ring_write(w[i]);
}

static inline void of_gpu_draw_span4_batch(const of_gpu_span4_t *spans,
                                            int count) {
    if (count <= 0 || spans == NULL) return;

    if (_gpu_batch_buf == NULL) {
        for (int i = 0; i < count; i++)
            of_gpu_draw_span4(&spans[i]);
        return;
    }

    while (count > 0) {
        int n = (count > OF_GPU_SPAN4_BATCH_MAX) ? OF_GPU_SPAN4_BATCH_MAX
                                                 : count;
        uint32_t payload_words = (uint32_t)(OF_GPU_SPAN4_WORDS * n);
        uint32_t *batch_buf = _gpu_batch_buf;

#if OF_GPU_BATCH_SLOTS > 1
        batch_buf += _gpu_batch_slot * OF_GPU_BATCH_SLOT_WORDS;
        _gpu_batch_slot++;
        if (_gpu_batch_slot >= OF_GPU_BATCH_SLOTS)
            _gpu_batch_slot = 0;
#else
        _gpu_wait_dma_idle_debug();
#endif
        for (int i = 0; i < n; i++)
            _gpu_encode_span4(&batch_buf[i * OF_GPU_SPAN4_WORDS],
                              &spans[i]);

        of_cache_flush_range(batch_buf, payload_words * 4);

        _gpu_ring_ensure((1 + payload_words) * 4);
        _gpu_ring_write(((uint32_t)GPU_CMD_DRAW_SPAN4_BATCH << 24) |
                        payload_words);

        GPU_DMA_SRC  = (uint32_t)(uintptr_t)batch_buf;
        GPU_DMA_LEN  = payload_words;
        GPU_DMA_KICK = 1;

        _gpu_wrptr = (_gpu_wrptr + payload_words * 4) & _gpu_ring_mask;

        spans += n;
        count -= n;
    }
}

/* Submit an already-encoded command stream through the doorbell-DMA path.
 *
 * `words` must contain complete GPU commands, including each command
 * header.  Unlike DRAW_SPANS_BATCH/DRAW_SPAN4_BATCH, this is not a
 * homogeneous command with one payload; the hardware pulls raw command
 * words from SDRAM into the canonical ring, then publishes ring_wrptr when
 * the stream has landed.  That lets callers batch
 * order-sensitive mixtures such as DRAW_SPAN and DRAW_SPAN4 without
 * flushing whenever descriptor type changes, while avoiding per-word MMIO
 * writes and any extra on-chip staging copy.
 *
 * The helper does not split the stream because splitting inside a
 * command would publish an incomplete command to the decoder.  Callers
 * must cap each stream to OF_GPU_COMMAND_STREAM_BATCH_WORDS and flush
 * only at command boundaries. */
static inline void of_gpu_submit_command_stream_batch(const uint32_t *words,
                                                       int word_count) {
    if (word_count <= 0 || words == NULL) return;
    if ((uint32_t)word_count > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();

    uint32_t payload_words = (uint32_t)word_count;

    if (_gpu_batch_buf == NULL) {
        _gpu_ring_ensure(payload_words * 4u);
        for (int i = 0; i < word_count; i++)
            _gpu_ring_write(words[i]);
        GPU_RING_WRPTR = _gpu_wrptr;
        return;
    }

    uint32_t *batch_buf = _gpu_batch_buf;

#if OF_GPU_BATCH_SLOTS > 1
    batch_buf += _gpu_batch_slot * OF_GPU_BATCH_SLOT_WORDS;
    _gpu_batch_slot++;
    if (_gpu_batch_slot >= OF_GPU_BATCH_SLOTS)
        _gpu_batch_slot = 0;
#else
    _gpu_wait_dma_idle_debug();
#endif

    for (int i = 0; i < word_count; i++)
        batch_buf[i] = words[i];

    of_cache_flush_range(batch_buf, payload_words * 4u);

    _gpu_ring_ensure(payload_words * 4u);

    GPU_DMA_SRC  = (uint32_t)(uintptr_t)batch_buf;
    GPU_DMA_LEN  = payload_words;
    GPU_DMA_KICK = 1;

    _gpu_wrptr = (_gpu_wrptr + payload_words * 4u) & _gpu_ring_mask;
}

/* of_gpu_draw_spans_batch — submit N spans in one go.
 *
 * Same per-span semantics as of_gpu_draw_span; the spans are processed
 * back-to-back through the existing fragment pipeline.  No state is
 * shared between spans — callers must emit any SET_FB / SET_TEXTURE /
 * SET_COLORMAP_ID etc. before the batch.
 *
 * Fast path (default): builds the entire batch in an SDRAM scratch
 * buffer using cached scalar stores (~5 ns/word), flushes the populated
 * range, then writes one CMD_DRAW_SPANS_BATCH header into the ring and
 * kicks the GPU's doorbell-DMA puller.  GPU streams the buffer via its
 * own AXI master while the CPU is free to do other work.  Per-span
 * cost: 15 cached stores ≈ 75 ns vs 16 MMIO writes × 120 ns = 1920 ns
 * — about 25× faster, plus the CPU is no longer stalled on the AXI bus.
 *
 * Fallback (scratch malloc failed at init): emits N standalone
 * CMD_DRAW_SPAN commands via per-word MMIO.  Slower but correct.
 *
 * Caps the batch at OF_GPU_BATCH_MAX_SPANS spans per kick — longer
 * arrays split across multiple kicks.  No upper bound on the total
 * `count`; throughput is amortised. */
static inline void of_gpu_draw_spans_batch(const of_gpu_span_t *spans,
                                            int count) {
    if (count <= 0 || spans == NULL) return;

    if (_gpu_batch_buf == NULL) {
        /* Fallback: emit N standalone spans via MMIO. */
        for (int i = 0; i < count; i++)
            of_gpu_draw_span(&spans[i]);
        return;
    }

    while (count > 0) {
        int n = (count > OF_GPU_BATCH_MAX_SPANS) ? OF_GPU_BATCH_MAX_SPANS
                                                  : count;
        uint32_t payload_words = (uint32_t)(OF_GPU_BATCH_WORDS_PER_SPAN * n);
        uint32_t *batch_buf = _gpu_batch_buf;

#if OF_GPU_BATCH_SLOTS > 1
        batch_buf += _gpu_batch_slot * OF_GPU_BATCH_SLOT_WORDS;
        _gpu_batch_slot++;
        if (_gpu_batch_slot >= OF_GPU_BATCH_SLOTS)
            _gpu_batch_slot = 0;
#else
        _gpu_wait_dma_idle_debug();
#endif

        /* Build the batch in cached SDRAM at scalar-store speed. */
        for (int i = 0; i < n; i++)
            _gpu_encode_span(&batch_buf[i * OF_GPU_BATCH_WORDS_PER_SPAN],
                             &spans[i]);

        /* Force the populated range out of L1 so the GPU's m_rd_*
         * AXI master sees committed bytes.  cbo.flush (writeback +
         * invalidate) is required on this VexiiRiscv config — cbo.clean
         * alone has been observed to leave dirty lines in L1, and the
         * GPU reads DRAM directly (not through the CPU's cache).
         * Same rationale as bank_preload's flush of the SF2 sample
         * blob before handing it to the audio mixer. */
        of_cache_flush_range(batch_buf, payload_words * 4);

        /* Reserve enough ring space for header + DMA-streamed payload.
         * Write the header word via MMIO; this advances ring_wr_addr in
         * the BRAM but does NOT publish ring_wrptr — we let the DMA's
         * end-of-kick pulse publish header + payload atomically.  This
         * avoids a race where the decoder fetches the header and runs
         * into ring BRAM cells the DMA hasn't filled yet. */
        _gpu_ring_ensure((1 + payload_words) * 4);
        _gpu_ring_write(((uint32_t)GPU_CMD_DRAW_SPANS_BATCH << 24) |
                        payload_words);

        /* Arm and fire the DMA puller.  When DMA finishes the last
         * sub-burst, the GPU FSM lifts ring_wrptr to the post-DMA
         * ring_wr_addr — exposing the full command (header + payload)
         * to the decoder in a single atomic step. */
        GPU_DMA_SRC  = (uint32_t)(uintptr_t)batch_buf;
        GPU_DMA_LEN  = payload_words;
        GPU_DMA_KICK = 1;

        /* Track the words the DMA will write — keep _gpu_wrptr in sync
         * with the GPU's eventual ring_wrptr so subsequent
         * _gpu_ring_ensure calls compute free space correctly. */
        _gpu_wrptr = (_gpu_wrptr + payload_words * 4) & _gpu_ring_mask;

        /* DO NOT wait for DMA here — return immediately and let the
         * caller's between-batch CPU work overlap the ~23 µs DMA fill.
         * Safety: any subsequent ring writer goes through
         * _gpu_ring_ensure(), which now spins on GPU_STATUS_DMA_BUSY
         * before allowing CPU MMIO writes to advance ring_wr_addr —
         * see the wait at the top of _gpu_ring_ensure for the
         * ring_wr_addr / DMA-payload interleave hazard. */

        spans += n;
        count -= n;
    }
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
 * Payload layout: 1 count word + N × 6 vertex words (6 words per vertex).
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
