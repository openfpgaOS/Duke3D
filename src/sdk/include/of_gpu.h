/*
 * of_gpu.h -- Hardware GPU Accelerator API for openfpgaOS
 *
 * Asynchronous span + triangle rasteriser.  CPU submits commands to a
 * 16 KB ring buffer in GPU-internal M10K BRAM; the GPU processes them
 * in parallel, writing pixels to the framebuffer via AXI4.
 *
 * Ring buffer: 16 KB in GPU-internal M10K BRAM.  CPU builds command
 * streams in a cached SDRAM scratch buffer, flushes and drains those
 * cache lines, then the GPU doorbell-DMA pulls the words into the ring
 * and publishes the write pointer atomically.  There is no CPU MMIO
 * command-data path.
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
#include "of_cache.h"
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define OF_GPU_CLEAR_COLOR      (1 << 0)
/* bit 1 reserved */

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
/* bit 1 reserved */
#define OF_GPU_SPAN_SKIP_ZERO    (1 << 2)
/* bits 3/4 reserved */
#define OF_GPU_SPAN_PERSP        (1 << 5)
#define OF_GPU_SPAN_TRANSLUC     (1 << 6)
/* bit 7 reserved */

/* ================================================================
 * Data Structures
 * ================================================================ */

typedef struct {
    uint8_t  lane_count;     /* 1..8; SDK splits to 4-lane native chunks */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by all lanes */
    uint8_t  reserved[2];
    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;
    int32_t  fb_step;        /* byte step per pixel inside each span */
    uint32_t fb_addr[8];
    uint32_t tex_addr[8];
    uint16_t count[8];
    int32_t  s[8], t[8];
    int32_t  sstep[8], tstep[8];
    uint8_t  light[8];       /* low 6 bits select palookup shade row */
    uint8_t  colormap_id[8]; /* explicit slot per lane, including slot 0 */
} of_gpu_affine_span_group_t;

typedef struct {
    uint32_t fb_addr;
    uint32_t tex_addr;

    uint8_t  lane_count;     /* 1..8; SDK splits to 4-lane native chunks */
    uint8_t  flags;          /* OF_GPU_SPAN_* shared by generated spans */
    uint8_t  reserved;
    uint8_t  colormap_id;

    int32_t  major_fb_step;  /* byte step between adjacent lanes */
    int32_t  minor_fb_step;  /* byte step per pixel inside each span */

    uint16_t tex_width;
    uint16_t tex_w_mask;
    uint16_t tex_h_mask;

    int16_t  start[8];       /* per-lane minor start */
    uint16_t count[8];       /* per-lane pixel count */

    int32_t  sdivz, tdivz, zi_persp;
    int32_t  sdivz_major_step, tdivz_major_step, zi_major_step;
    int32_t  sdivz_minor_step, tdivz_minor_step, zi_minor_step;

    int32_t  light;          /* signed Q6.16, low 24 bits used */
    int32_t  light_major_step;
    int32_t  light_minor_step;
} of_gpu_persp_span_group_t;

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
    /* r = light index into the active palookup slot; low 6 bits select
     * the 64-row shade table.  Only `r` of v0
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
#define GPU_RING_WRPTR          OF_GPU_REG(0x04)  /* R: published write pointer */
#define GPU_DMA_SRC             OF_GPU_REG(0x0C)  /* W: SDRAM byte address of command buffer to pull */
#define GPU_RING_RDPTR          OF_GPU_REG(0x10)  /* R: GPU read pointer */
#define GPU_STATUS              OF_GPU_REG(0x14)  /* R: bit6=DMA desc full, bit3=transluc busy, bit2=DMA busy, bit1=ring empty, bit0=busy */
#define GPU_FENCE_REACHED       OF_GPU_REG(0x18)  /* R: last completed fence token */
#define GPU_DMA_LEN             OF_GPU_REG(0x1C)  /* W: word count to pull (≤4096) */
#define GPU_TRANSLUC_ADDR       OF_GPU_REG(0x20)  /* W: byte addr into transluc[] (auto-inc by 4) */
#define GPU_TRANSLUC_DATA       OF_GPU_REG(0x24)  /* W: 32-bit word into transluc[] */
#define GPU_TEX_FLUSH           OF_GPU_REG(0x28)  /* W: flush texture cache */
#define GPU_DMA_KICK            OF_GPU_REG(0x2C)  /* W: write 1 to fire DMA pull from (SRC, LEN) */

/* GPU_STATUS bit definitions */
#define GPU_STATUS_BUSY        0x1u
#define GPU_STATUS_RING_EMPTY  0x2u
#define GPU_STATUS_DMA_BUSY    0x4u  /* SDRAM command/payload DMA busy */
#define GPU_STATUS_TRANSLUC_BUSY 0x8u /* SRAM translucency LUT upload/lookup busy */
#define GPU_STATUS_DMA_DESC_FULL 0x40u /* command DMA descriptor FIFO full */

/* ================================================================
 * Command IDs
 * ================================================================ */

#define GPU_CMD_FENCE           0x02
#define GPU_CMD_CLEAR_RECT      0x11  /* 3-word payload: start byte addr,
                                       * {w,h}, {pad,color}. Color's low
                                       * byte is replicated 4× per FB word. */
#define GPU_CMD_SET_TEXTURE     0x20
#define GPU_CMD_SET_FB          0x23
#define GPU_CMD_DRAW_TRIANGLES  0x30
/* GPU-triggered display flip (cr-gpu-triggered-flip.md).  2-word payload:
 *   word 0: bits[1:0] = back-buffer index (0/1/2 → FB_ADDR_{0,1,2})
 *   word 1: fence token (published to GPU_FENCE_REACHED after the swap)
 * Drains all outstanding m_wr_* writes (same primitive as the upgraded
 * CMD_FENCE), pulses the swap side-port to axi_periph_slave for one
 * cycle, then publishes the fence token. */
#define GPU_CMD_FLIP             0x42
#define GPU_CMD_DRAW_PERSP_SPAN_GROUP 0x46  /* 23-word clipped perspective group */
#define GPU_CMD_DRAW_AFFINE_SPAN_GROUP 0x47 /* 1..4 independent affine spans */

#define OF_GPU_PERSP_SPAN_GROUP_WORDS 23u
#define OF_GPU_AFFINE_SPAN_GROUP_COMMON_WORDS 4u
#define OF_GPU_AFFINE_SPAN_GROUP_LANE_WORDS 7u
#define OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES 4u
#define OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES 8u
#define OF_GPU_AFFINE_SPAN_GROUP_WORDS(lanes) \
    (OF_GPU_AFFINE_SPAN_GROUP_COMMON_WORDS + \
     ((uint32_t)(lanes) * OF_GPU_AFFINE_SPAN_GROUP_LANE_WORDS))

/* ================================================================
 * Palookup (colormap) layout in SDRAM — must match gpu_core.v's
 * PALOOKUP_BASE / PALOOKUP_STRIDE constants.
 *
 * Each slot holds a Quake/BUILD-shape shade × texel table.  Scalar and
 * perspective span paths carry an explicit colormap_id in each command;
 * affine span groups carry an explicit colormap_id per lane. Triangles
 * currently use slot 0. Up to 16 slots; the GPU reads
 * palookup[slot][shade & 63][texel] from
 *   GPU_AXI_BASE + OF_GPU_PALOOKUP_AXI_OFFSET
 *              + slot*0x4000 + shade*256 + texel
 * via gpu_tex_cache port B.
 *
 * The CPU-visible address depends on how the target maps the GPU's
 * AXI M0 into the CPU address space — apps should obtain it via the
 * runtime caps descriptor and add the per-slot offset.  These
 * constants encode the GPU-side AXI offset and per-slot
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
 * CPU writes this window through the cached alias for speed.  The RTL DMA
 * puller has a two-entry descriptor FIFO, so the SDK alternates between
 * two scratch buffers and can build one command stream while the prior
 * stream is still being copied into ring BRAM.  Before each GPU DMA kick,
 * of_gpu drains the flushed cache lines with same-master readbacks so the
 * GPU cannot read stale command words. */
#define OF_GPU_BATCH_BUF_AXI_OFFSET  0x00140000u
#define OF_GPU_BATCH_BUFFER_COUNT    2u
#define OF_GPU_BATCH_BUFFER_BYTES    0x00004000u  /* 16 KB per buffer */
#define OF_GPU_BATCH_BUF_BYTES       (OF_GPU_BATCH_BUFFER_COUNT * OF_GPU_BATCH_BUFFER_BYTES)
#define OF_GPU_CACHE_LINE_BYTES      64u

/* ================================================================
 * Ring Buffer State (app-side)
 *
 * Static mutable — include this header from one .c file only.
 * ================================================================ */

static uint32_t _gpu_wrptr;
static uint32_t _gpu_known_rdptr;
static uint32_t _gpu_fence_next;
static uint32_t _gpu_cmd_words;
static uint32_t _gpu_batch_dma_base;
static uint32_t _gpu_batch_dma_addr;
static uint32_t _gpu_batch_index;
static uint32_t _gpu_batch_inflight_mask;

static const uint32_t _gpu_ring_mask = OF_GPU_RING_SIZE - 1;

/* Doorbell-DMA scratch buffer.  Pinned to a fixed SDRAM offset by
 * of_gpu_init and kept cached so command construction does not stall on
 * every store.  NULL on targets that don't expose SDRAM; command
 * submission traps on those targets. */
static uint32_t *_gpu_batch_buf_base;
static uint32_t *_gpu_batch_buf;
static uint32_t  _gpu_dbg_dma_waits;
static uint32_t  _gpu_dbg_dma_spin_iters;
static uint32_t  _gpu_dbg_ring_waits;
static uint32_t  _gpu_dbg_ring_spin_iters;
static uint32_t  _gpu_dbg_min_ring_free;

static uint32_t _gpu_state_valid;
static uint32_t _gpu_state_fb_addr;
static uint32_t _gpu_state_fb_stride;
static uint32_t _gpu_state_tex_addr;
static uint32_t _gpu_state_tex_dims;

#define OF_GPU_STATE_FB       (1u << 0)
#define OF_GPU_STATE_TEXTURE  (1u << 1)

#define OF_GPU_COMMAND_STREAM_BATCH_WORDS ((OF_GPU_RING_SIZE / 4u) - 1u)

#if (OF_GPU_COMMAND_STREAM_BATCH_WORDS * 4u) > OF_GPU_BATCH_BUFFER_BYTES
#error "GPU command stream buffer must fit in one reserved SDRAM scratch buffer"
#endif

#if OF_GPU_BATCH_BUFFER_COUNT != 2u
#error "GPU command staging assumes two scratch buffers"
#endif

/* ---- Internal helpers ---- */

static inline void _gpu_select_batch_buffer(uint32_t index) {
    _gpu_batch_index = index & 1u;
    _gpu_batch_dma_addr = _gpu_batch_dma_base +
        (_gpu_batch_index * OF_GPU_BATCH_BUFFER_BYTES);
    _gpu_batch_buf = _gpu_batch_buf_base +
        (_gpu_batch_index * (OF_GPU_BATCH_BUFFER_BYTES / sizeof(uint32_t)));
}

static inline void _gpu_wait_dma_idle_debug(void) {
    uint32_t dma_spins = 0;
    while (GPU_STATUS & GPU_STATUS_DMA_BUSY)
        dma_spins++;
    _gpu_batch_inflight_mask = 0;
    if (dma_spins) {
        _gpu_dbg_dma_waits++;
        _gpu_dbg_dma_spin_iters += dma_spins;
    }
}

static inline void _gpu_wait_dma_desc_slot_debug(void) {
    uint32_t dma_spins = 0;
    while (GPU_STATUS & GPU_STATUS_DMA_DESC_FULL)
        dma_spins++;
    if (dma_spins) {
        _gpu_dbg_dma_waits++;
        _gpu_dbg_dma_spin_iters += dma_spins;
    }
}

static inline void _gpu_wait_transluc_idle(void) {
    while (GPU_STATUS & GPU_STATUS_TRANSLUC_BUSY) {
    }
}

static inline uint32_t _gpu_ring_free_now(void) {
    uint32_t rdptr = GPU_RING_RDPTR;
    _gpu_known_rdptr = rdptr;
    return (rdptr - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline uint32_t _gpu_ring_free_known(void) {
    return (_gpu_known_rdptr - _gpu_wrptr - 4) & _gpu_ring_mask;
}

static inline void _gpu_note_ring_free(uint32_t ring_free) {
    if (ring_free < _gpu_dbg_min_ring_free)
        _gpu_dbg_min_ring_free = ring_free;
}

static inline void _gpu_cbo_flush_line(void *addr) {
    __asm__ volatile(".insn i 0x0F, 2, x0, %0, 2" :: "r"(addr) : "memory");
}

static inline void _gpu_flush_cmd_cache_range(void *addr, uint32_t bytes) {
    __asm__ volatile("fence" ::: "memory");
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(OF_GPU_CACHE_LINE_BYTES - 1u);
    uintptr_t end = (uintptr_t)addr + bytes;
    for (; a < end; a += OF_GPU_CACHE_LINE_BYTES)
        _gpu_cbo_flush_line((void *)a);
    __asm__ volatile("fence" ::: "memory");
}

static inline void _gpu_drain_cmd_writeback(uint32_t bytes) {
    if (bytes == 0)
        return;

    volatile const uint32_t *p = (volatile const uint32_t *)_gpu_batch_buf;
    uint32_t words = (bytes + 3u) >> 2;
    uint32_t sink = 0;

    /* of_cache_flush_range() invalidates each line after scheduling its
     * writeback.  These cached reads use the same d_axi master as those
     * writebacks, unlike a p_axi uncached read, so they force the CPU-side
     * memory stream to observe the flushed command data before GPU DMA
     * gets priority on the SDRAM arbiter. */
    for (uint32_t off = 0; off < bytes; off += OF_GPU_CACHE_LINE_BYTES)
        sink ^= p[off >> 2];
    sink ^= p[words - 1u];

    __asm__ volatile("" :: "r"(sink) : "memory");
    __asm__ volatile("fence" ::: "memory");
}

static inline void _gpu_flush_cmd_stream(void) {
    if (_gpu_cmd_words == 0)
        return;
    if (_gpu_batch_buf == NULL)
        __builtin_trap();

    uint32_t submit_words = _gpu_cmd_words;
    uint32_t submit_index = _gpu_batch_index;

    _gpu_flush_cmd_cache_range(_gpu_batch_buf, submit_words * 4u);
    _gpu_drain_cmd_writeback(submit_words * 4u);
    _gpu_wait_dma_desc_slot_debug();

    /* The writeback drain above is the data-visibility barrier.  The GPU MMIO
     * doorbell registers sit on a single in-order peripheral path, so extra
     * CPU fences between these volatile writes only add submit latency. */
    GPU_DMA_SRC  = _gpu_batch_dma_addr;
    GPU_DMA_LEN  = submit_words;
    GPU_DMA_KICK = 1;
    __asm__ volatile("" ::: "memory");

    _gpu_batch_inflight_mask |= (1u << submit_index);
    _gpu_cmd_words = 0;

    uint32_t next_index = submit_index ^ 1u;
    if (_gpu_batch_inflight_mask & (1u << next_index))
        _gpu_wait_dma_idle_debug();
    _gpu_select_batch_buffer(next_index);
}

static inline void _gpu_ring_ensure(uint32_t bytes) {
    if (bytes > (OF_GPU_RING_SIZE - 4u))
        __builtin_trap();

    uint32_t ring_free = _gpu_ring_free_known();
    _gpu_note_ring_free(ring_free);
    if (ring_free >= bytes)
        return;

    /* Slow path: if commands are only staged in SDRAM, first publish them
     * through the doorbell DMA so the GPU has something to drain. */
    _gpu_flush_cmd_stream();
    _gpu_wait_dma_idle_debug();

    {
        uint32_t ring_spins = 0;
        do {
            ring_free = _gpu_ring_free_now();
            _gpu_note_ring_free(ring_free);
            if (ring_free >= bytes)
                break;
            ring_spins++;
        } while (1);
        _gpu_dbg_ring_waits++;
        _gpu_dbg_ring_spin_iters += ring_spins;
    }
}

static inline void _gpu_stream_reserve_words(uint32_t words) {
    if (words == 0)
        return;
    if (_gpu_batch_buf == NULL || words > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();
    if (_gpu_cmd_words + words > OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        _gpu_flush_cmd_stream();
    _gpu_ring_ensure(words * 4u);
}

/* Append one word to the staged SDRAM command stream.  Callers reserve
 * a whole command first so this helper never flushes in the middle of a
 * command payload. */
static inline void _gpu_ring_write(uint32_t w) {
    if (_gpu_batch_buf == NULL || _gpu_cmd_words >= OF_GPU_COMMAND_STREAM_BATCH_WORDS)
        __builtin_trap();
    _gpu_batch_buf[_gpu_cmd_words++] = w;
    _gpu_wrptr = (_gpu_wrptr + 4) & _gpu_ring_mask;
}

static inline void _gpu_cmd_header(uint8_t cmd, uint32_t payload_words) {
    _gpu_stream_reserve_words(1u + payload_words);
    _gpu_ring_write(((uint32_t)cmd << 24) | (payload_words & 0x00FFFFFF));
}

/* ================================================================
 * API Functions
 * ================================================================ */

static inline void of_gpu_init(void) {
    /* Resolve the GPU MMIO base from the runtime caps descriptor.
     * Must be called after main() (or after the SDK constructors run)
     * so _of_caps_ptr is populated. Apps that try to drive the GPU
     * before of_gpu_init() may write address 0, which is valid BRAM on
     * Pocket, so always initialize before touching GPU helpers or MMIO. */
    _gpu_base = of_get_caps()->gpu_base;

    _gpu_wrptr = 0;
    _gpu_known_rdptr = 0;
    _gpu_fence_next = 1;
    _gpu_cmd_words = 0;
    _gpu_batch_dma_base = 0;
    _gpu_batch_dma_addr = 0;
    _gpu_batch_index = 0;
    _gpu_batch_inflight_mask = 0;
    _gpu_batch_buf_base = NULL;
    _gpu_batch_buf = NULL;
    _gpu_dbg_dma_waits = 0;
    _gpu_dbg_dma_spin_iters = 0;
    _gpu_dbg_ring_waits = 0;
    _gpu_dbg_ring_spin_iters = 0;
    _gpu_dbg_min_ring_free = OF_GPU_RING_SIZE;
    _gpu_state_valid = 0;
    GPU_CTRL = 4;               /* ring_reset: clear wr_addr + wrptr + rdptr */
    GPU_CTRL = 1;               /* enable */

    /* Pin the doorbell-DMA scratch buffer at a known SDRAM offset.
     * Command words are written through the cached alias for normal CPU
     * store speed.  _gpu_flush_cmd_stream() handles the external-master
     * handoff by flushing and then reading back the invalidated lines on
     * the same d_axi path before GPU_DMA_KICK. */
    {
        const struct of_capabilities *caps = of_get_caps();
        if (caps && caps->sdram_base != 0) {
            _gpu_batch_dma_base = caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET;
            _gpu_batch_buf_base = (uint32_t *)(uintptr_t)
                (caps->sdram_base + OF_GPU_BATCH_BUF_AXI_OFFSET);
            _gpu_select_batch_buffer(0);
        }
    }
}

/* Upload a palookup table to slot N in SDRAM.  The GPU reads palookup
 * bytes through gpu_tex_cache port B; this helper writes the table
 * directly through the uncached SDRAM alias so the GPU sees committed
 * data.  16 KB per slot, up to 16 slots (cf. OF_GPU_PALOOKUP_*).
 *
 * Span and span-group draw commands select slots explicitly with their
 * colormap_id fields. Triangle draws use slot 0, so apps that submit
 * triangles should upload row 0 as identity when they want raw texels. */
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
    const uint8_t *src = data;
    uint32_t words = size >> 2;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t off = i << 2;
        dst[i] = (uint32_t)src[off + 0] |
                 ((uint32_t)src[off + 1] << 8) |
                 ((uint32_t)src[off + 2] << 16) |
                 ((uint32_t)src[off + 3] << 24);
    }
    /* Tail bytes (size not a multiple of 4) — fold into a final word
     * so we still write every byte the caller passed.  Pad the unused
     * lanes with zero rather than skipping, so the SDRAM word is
     * fully defined regardless of whatever was there before. */
    uint32_t tail = size & 3u;
    if (tail) {
        uint32_t w = 0;
        const uint8_t *tb = src + (words << 2);
        for (uint32_t i = 0; i < tail; i++)
            w |= ((uint32_t)tb[i]) << (i * 8);
        dst[words] = w;
    }
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
        for (int w = 0; w < 64; w++) {
            _gpu_wait_transluc_idle();
            GPU_TRANSLUC_DATA = row32[w];
        }
    }
    _gpu_wait_transluc_idle();
}

static inline void of_gpu_kick(void) {
    _gpu_flush_cmd_stream();
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
     *   GPU_STATUS    (0x14) — busy, ring_empty, DMA state/queue
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

/* Engines that mix GPU rendering with direct CPU framebuffer access should
 * call this before reading from the framebuffer, or before CPU overlays that
 * must land after GPU-rendered pixels. */
static inline void of_gpu_prepare_framebuffer_for_cpu(void) {
    of_gpu_finish();
}

static inline void of_gpu_shutdown(void) {
    of_gpu_finish();
    GPU_CTRL = 0;
    _gpu_state_valid = 0;
}

typedef struct {
    uint32_t status;
    uint32_t rdptr;
    uint32_t wrptr;
    uint32_t fence_reached;
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
    snap->wrptr = _gpu_wrptr;
    snap->fence_reached = GPU_FENCE_REACHED;

    snap->ring_free = _gpu_ring_free_now();
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
    if ((_gpu_state_valid & OF_GPU_STATE_FB) &&
        _gpu_state_fb_addr == addr &&
        _gpu_state_fb_stride == (uint32_t)stride)
        return;

    _gpu_cmd_header(GPU_CMD_SET_FB, 2);
    _gpu_ring_write(addr);
    _gpu_ring_write((uint32_t)stride);
    _gpu_state_fb_addr = addr;
    _gpu_state_fb_stride = (uint32_t)stride;
    _gpu_state_valid |= OF_GPU_STATE_FB;
}

static inline void of_gpu_bind_texture(const of_gpu_texture_t *tex) {
    uint32_t dims = ((uint32_t)tex->width << 16) | tex->height;
    if ((_gpu_state_valid & OF_GPU_STATE_TEXTURE) &&
        _gpu_state_tex_addr == tex->addr &&
        _gpu_state_tex_dims == dims)
        return;

    _gpu_cmd_header(GPU_CMD_SET_TEXTURE, 2);
    _gpu_ring_write(tex->addr);
    _gpu_ring_write(dims);
    _gpu_state_tex_addr = tex->addr;
    _gpu_state_tex_dims = dims;
    _gpu_state_valid |= OF_GPU_STATE_TEXTURE;
}

/* ---- Draw commands ---- */

/* Whole-FB clear.  flags bit 0 = clear color. */
static inline void of_gpu_clear(uint32_t flags, uint16_t color) {
    if ((flags & OF_GPU_CLEAR_COLOR) == 0)
        return;

    /* The old whole-FB clear was a fixed 320x200 contiguous write from
     * the current framebuffer base.  Keep that public behavior but encode
     * it as a normal rectangle clear so the RTL only has one clear path. */
    _gpu_cmd_header(GPU_CMD_CLEAR_RECT, 3);
    _gpu_ring_write(_gpu_state_fb_addr);
    _gpu_ring_write((320u << 16) | 200u);
    _gpu_ring_write((320u << 16) | ((uint32_t)color & 0xFFu));
}

/* Clear a rectangular region of the framebuffer to a constant color.
 * Caller computes the start byte address (fb_base + y*stride + x); the
 * GPU walks `h` rows × `w` bytes from there, advancing each row by the
 * active st_fb_stride.  Color's low byte is replicated 4× per FB word.
 * Word-aligned full-width strips
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

static inline uint32_t _gpu_affine_group_lane_count(uint32_t lane_count) {
    if (lane_count > OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES)
        return OF_GPU_AFFINE_SPAN_GROUP_MAX_LANES;
    return lane_count;
}

static inline void
of_gpu_draw_affine_span_group(const of_gpu_affine_span_group_t *group) {
    uint32_t lane_count;

    if (group == NULL)
        return;

    lane_count = _gpu_affine_group_lane_count(group->lane_count);
    if (lane_count == 0)
        return;

    for (uint32_t first = 0; first < lane_count;) {
        uint32_t chunk = lane_count - first;
        uint32_t any_pixels = 0;
        if (chunk > OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES)
            chunk = OF_GPU_AFFINE_SPAN_GROUP_MAX_NATIVE_LANES;

        for (uint32_t i = 0; i < chunk; i++)
            any_pixels |= group->count[first + i];

        if (any_pixels != 0) {
            _gpu_cmd_header(GPU_CMD_DRAW_AFFINE_SPAN_GROUP,
                            OF_GPU_AFFINE_SPAN_GROUP_WORDS(chunk));
            _gpu_ring_write((chunk << 28) |
                            ((((uint32_t)group->flags & ~OF_GPU_SPAN_PERSP) & 0xFFu) << 20));
            _gpu_ring_write((uint32_t)group->tex_width);
            _gpu_ring_write(((uint32_t)group->tex_h_mask << 16) |
                            (uint32_t)group->tex_w_mask);
            _gpu_ring_write((uint32_t)group->fb_step);

            for (uint32_t lane = 0; lane < chunk; lane++) {
                uint32_t src = first + lane;
                _gpu_ring_write(group->fb_addr[src]);
                _gpu_ring_write(group->tex_addr[src]);
                _gpu_ring_write((((uint32_t)group->colormap_id[src] & 0x0Fu) << 28) |
                                (((uint32_t)group->light[src] & 0x3Fu) << 16) |
                                (uint32_t)group->count[src]);
                _gpu_ring_write((uint32_t)group->s[src]);
                _gpu_ring_write((uint32_t)group->t[src]);
                _gpu_ring_write((uint32_t)group->sstep[src]);
                _gpu_ring_write((uint32_t)group->tstep[src]);
            }
        }

        first += chunk;
    }
}

/* Perspective variable-count adjacent span group.  The hardware native
 * command expands up to four clipped lanes into scalar perspective spans.
 * The public helper accepts up to eight lanes and splits wide submissions.
 *
 * For lane i:
 *   fb   = fb_addr + i*major_fb_step + start[i]*minor_fb_step
 *   s/z  = sdivz + i*sdivz_major_step + start[i]*sdivz_minor_step
 *   t/z  = tdivz + i*tdivz_major_step + start[i]*tdivz_minor_step
 *   1/z  = zi_persp + i*zi_major_step + start[i]*zi_minor_step
 * Then count[i] pixels are generated along minor_fb_step. */
static inline uint32_t
_gpu_encode_persp_span_group_chunk(uint32_t *p,
                                   const of_gpu_persp_span_group_t *s,
                                   uint32_t first_lane,
                                   uint32_t lane_count) {
    uint32_t live = 0;
    uint32_t flags = (uint32_t)s->flags | OF_GPU_SPAN_PERSP;
    uint32_t fb_major = (uint32_t)s->major_fb_step * first_lane;
    uint32_t sZ_major = (uint32_t)s->sdivz_major_step * first_lane;
    uint32_t tZ_major = (uint32_t)s->tdivz_major_step * first_lane;
    uint32_t zi_major = (uint32_t)s->zi_major_step * first_lane;
    uint32_t light_major = (uint32_t)s->light_major_step * first_lane;
    uint16_t start[4] = {0, 0, 0, 0};
    uint16_t count[4] = {0, 0, 0, 0};

    if (lane_count > 4u)
        lane_count = 4u;

    for (uint32_t i = 0; i < lane_count; i++) {
        uint32_t src = first_lane + i;
        start[i] = (uint16_t)s->start[src];
        count[i] = s->count[src];
        live |= (uint32_t)(count[i] != 0);
    }

    p[0] = s->fb_addr + fb_major;
    p[1] = s->tex_addr;
    p[2] = ((lane_count & 0x0Fu) << 28) |
           ((flags & 0xFFu) << 20) |
           (((uint32_t)s->reserved & 0x0Fu) << 16) |
           ((uint32_t)s->colormap_id & 0x0Fu);
    p[3] = (uint32_t)s->major_fb_step;
    p[4] = (uint32_t)s->minor_fb_step;
    p[5] = (uint32_t)s->tex_width;
    p[6] = ((uint32_t)s->tex_h_mask << 16) |
           (uint32_t)s->tex_w_mask;
    p[7] = ((uint32_t)start[1] << 16) | (uint32_t)start[0];
    p[8] = ((uint32_t)start[3] << 16) | (uint32_t)start[2];
    p[9] = ((uint32_t)count[1] << 16) | (uint32_t)count[0];
    p[10] = ((uint32_t)count[3] << 16) | (uint32_t)count[2];
    p[11] = (uint32_t)s->sdivz + sZ_major;
    p[12] = (uint32_t)s->tdivz + tZ_major;
    p[13] = (uint32_t)s->zi_persp + zi_major;
    p[14] = (uint32_t)s->sdivz_major_step;
    p[15] = (uint32_t)s->tdivz_major_step;
    p[16] = (uint32_t)s->zi_major_step;
    p[17] = (uint32_t)s->sdivz_minor_step;
    p[18] = (uint32_t)s->tdivz_minor_step;
    p[19] = (uint32_t)s->zi_minor_step;
    p[20] = (uint32_t)s->light + light_major;
    p[21] = (uint32_t)s->light_major_step;
    p[22] = (uint32_t)s->light_minor_step;
    return live;
}

static inline void
of_gpu_draw_persp_span_group(const of_gpu_persp_span_group_t *span) {
    uint32_t w[OF_GPU_PERSP_SPAN_GROUP_WORDS];
    if (span == NULL) return;

    uint32_t lanes_left = span->lane_count;
    if (lanes_left == 0)
        return;
    if (lanes_left > 8u)
        lanes_left = 8u;

    for (uint32_t first = 0; lanes_left != 0;) {
        uint32_t n = (lanes_left >= 4u) ? 4u : lanes_left;
        if (_gpu_encode_persp_span_group_chunk(w, span, first, n)) {
            _gpu_cmd_header(GPU_CMD_DRAW_PERSP_SPAN_GROUP,
                            OF_GPU_PERSP_SPAN_GROUP_WORDS);
            for (uint32_t i = 0; i < OF_GPU_PERSP_SPAN_GROUP_WORDS; i++)
                _gpu_ring_write(w[i]);
        }
        first += n;
        lanes_left -= n;
    }
}

static inline void
of_gpu_draw_persp_span_group_batch(const of_gpu_persp_span_group_t *spans,
                                   int count) {
    if (count <= 0 || spans == NULL) return;

    for (int i = 0; i < count; i++)
        of_gpu_draw_persp_span_group(&spans[i]);
    _gpu_flush_cmd_stream();
}

/* Submit an already-encoded command stream through the doorbell-DMA path.
 *
 * `words` must contain complete GPU commands, including each command
 * header.  This can batch order-sensitive mixtures of commands without
 * flushing whenever descriptor type changes.
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

    uint32_t stream_words = (uint32_t)word_count;
    _gpu_stream_reserve_words(stream_words);

    for (int i = 0; i < word_count; i++)
        _gpu_batch_buf[_gpu_cmd_words + (uint32_t)i] = words[i];

    _gpu_cmd_words += stream_words;
    _gpu_wrptr = (_gpu_wrptr + stream_words * 4u) & _gpu_ring_mask;
    _gpu_state_valid = 0;
    _gpu_flush_cmd_stream();
}

static inline void _gpu_write_vertex(const of_gpu_vertex_t *v) {
    _gpu_ring_write(((uint32_t)(uint16_t)v->x << 16) | (uint16_t)v->y);
    _gpu_ring_write(((uint32_t)v->z << 16));
    _gpu_ring_write((uint32_t)v->s);
    _gpu_ring_write((uint32_t)v->t);
    _gpu_ring_write((uint32_t)v->w);
    _gpu_ring_write(((uint32_t)v->a << 24) | ((uint32_t)v->b << 16) |
                    ((uint32_t)v->g << 8) | (v->r & 0x3Fu));
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
