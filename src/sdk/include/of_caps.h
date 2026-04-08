/*
 * of_caps.h -- openfpgaOS Capability Descriptors
 *
 * The OS populates a capability struct at a fixed BRAM address before
 * launching the application. Apps read it to discover the platform,
 * available hardware, memory layout, and OS services.
 *
 * This replaces hardcoded addresses (FB0_BASE, CRAM1 sample pool, etc.)
 * and enables the same app binary to run on different targets (Pocket,
 * MiSTer) and core variants (full, lite, 3d).
 *
 *   const struct of_capabilities *caps = of_get_caps();
 *   if (of_has_feature(OF_HW_MIXER))
 *       init_pcm_audio();
 */

#ifndef OF_CAPS_H
#define OF_CAPS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_CAPS_MAGIC   0x43415053  /* 'CAPS' */
#define OF_CAPS_VERSION 1
#define OF_CAPS_ADDR    0x00007800  /* BRAM — fast access, no D-cache pollution */

/* Platform IDs */
#define OF_PLATFORM_POCKET    1
#define OF_PLATFORM_MISTER    2
#define OF_PLATFORM_SIM       255

/* Hardware feature flags — must match RTL HW_FEATURES bit layout in
 * src/fpga/common/axi_periph_slave.v and src/firmware/os/hal/regs.h. */
#define OF_HW_MIXER         (1 << 0)    /* PCM hardware mixer */
#define OF_HW_OPL3          (1 << 1)    /* OPL3 FM synthesis */
#define OF_HW_NET           (1 << 2)    /* Networking (link cable / serial / wifi) */
#define OF_HW_ANALOGIZER    (1 << 3)    /* Analog video output */
#define OF_HW_GPU_SPAN      (1 << 4)    /* GPU span renderer (always set) */
#define OF_HW_GPU_TRIANGLE  (1 << 5)    /* GPU triangle rasterizer (Full only) */
#define OF_HW_MIDI          (1 << 6)    /* MIDI playback via OPL3 */
#define OF_HW_WIFI          (1 << 7)    /* Wireless networking */
#define OF_HW_FPU           (1 << 8)    /* Hardware FPU (RISC-V F extension) */
#define OF_HW_SAVE_SLOTS    (1 << 9)    /* Persistent save storage */
#define OF_HW_GPU_VCOLOR    (1 << 10)   /* GPU vertex color interpolation */
#define OF_HW_GPU_BILINEAR  (1 << 11)   /* GPU bilinear texture filter */
#define OF_HW_GPU_ALPHA     (1 << 12)   /* GPU alpha / additive blending */
#define OF_HW_GPU_PERSP     (1 << 13)   /* GPU perspective spans (Quake-style) */
#define OF_HW_GPU_FRAGPIPE  (1 << 14)   /* GPU 1-px/cycle fragment pipeline */

/* Convenience: all the GPU bits an app might care about for renderer choice. */
#define OF_HW_GPU_LITE_MASK  (OF_HW_GPU_SPAN | OF_HW_GPU_PERSP | OF_HW_GPU_FRAGPIPE)
#define OF_HW_GPU_FULL_MASK  (OF_HW_GPU_LITE_MASK | OF_HW_GPU_TRIANGLE | \
                              OF_HW_GPU_VCOLOR | OF_HW_GPU_BILINEAR | OF_HW_GPU_ALPHA)

struct of_capabilities {
    uint32_t magic;             /* OF_CAPS_MAGIC */
    uint32_t version;           /* OF_CAPS_VERSION */

    /* Memory regions (base + size, 0 = not available) */
    uint32_t heap_base;         /* App heap start (SDRAM) */
    uint32_t heap_size;         /* App heap size in bytes */
    uint32_t fb_base;           /* Framebuffer 0 base address */
    uint32_t fb_size;           /* Single framebuffer size in bytes */
    uint32_t fb_width;          /* Framebuffer width in pixels */
    uint32_t fb_height;         /* Framebuffer height in pixels */
    uint32_t fb_stride;         /* Bytes per row */
    uint32_t sample_base;       /* Audio sample pool base (CRAM1) */
    uint32_t sample_size;       /* Audio sample pool size in bytes */

    /* Hardware features */
    uint32_t hw_features;       /* OF_HW_* bitmask */
    uint32_t mixer_voices;      /* 0 = no mixer, 32 = full */
    uint32_t mixer_rate;        /* Output sample rate (48000) */

    /* Platform identity */
    uint32_t platform_id;       /* OF_PLATFORM_* */
    uint32_t core_variant;      /* Bitstream variant (0 = default) */
    uint32_t sdram_size;        /* Total SDRAM in bytes */
    uint32_t cram_size;         /* Per-bank CRAM size in bytes */

    /* OS info */
    uint32_t os_version;        /* Packed: major.minor.patch */
    uint32_t cpu_freq_hz;       /* CPU clock frequency */
    uint32_t services_table;    /* Address of OS services table (0 = none) */
};

#ifndef OF_PC

static inline const struct of_capabilities *of_get_caps(void) {
    return (const struct of_capabilities *)OF_CAPS_ADDR;
}

static inline int of_has_feature(uint32_t feature) {
    return (of_get_caps()->hw_features & feature) != 0;
}

#else /* OF_PC */

const struct of_capabilities *of_get_caps(void);
int of_has_feature(uint32_t feature);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_CAPS_H */
