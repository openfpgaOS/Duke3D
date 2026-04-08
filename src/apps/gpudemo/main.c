/*
 * openfpgaOS GPU Accelerator Demo
 *
 * Showcases every GPU feature:
 *   Mode 0 — Doom-style textured walls + floor (column / horizontal spans
 *            with colormap lighting) plus a transparent SKIP_ZERO sprite
 *   Mode 1 — Rotating 3D cube via the hardware triangle rasteriser
 *   Mode 2 — Perspective-correct textured triangle (software rasterised,
 *            using SPAN_PERSP horizontal scanlines for the inner loop —
 *            the GPU does the 1/z reciprocal + multiply per 16-pixel
 *            sub-segment in hardware)
 *
 * Controls:
 *   A button — cycle through demo modes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "of.h"
#include "of_cache.h"
#define OF_GPU_DEBUG_RING   /* heartbeat print inside _gpu_ring_ensure spin */
#include "of_gpu.h"

#define SCREEN_W 320
#define SCREEN_H 240

/* of_gpu_clear's pixel count is hardware-hardcoded to 320*200 = 64000 bytes
 * (= 16000 32-bit AXI bursts), so it only clears the upper 200 rows of the
 * 320x240 framebuffer. The remaining 40 rows must be cleared by the CPU. */
#define GPU_CLEAR_ROWS 200
#define LETTERBOX_ROWS (SCREEN_H - GPU_CLEAR_ROWS)

/* Ring buffer is now 16 KB M10K BRAM inside the GPU — no CPU allocation needed */

/* Texture data lives on the heap (in SDRAM). The previous static `.sdram`
 * section was an orphan with no output mapping, so the storage ended up
 * in a non-loaded segment and the GPU saw zeros. Heap allocations come
 * from the SDRAM heap and are reachable from both CPU and GPU. */
static uint8_t *checkerboard_tex;   /* 64×64 floor */
static uint8_t *wall_tex;           /* 64×64 wall  */
static uint8_t *sprite_tex;         /* 16×16, 0xFF = transparent (SPAN_SKIP_ZERO) */
static uint8_t *persp_tex;          /* 64×64 grid for the perspective demo */

/* Colormap: 64 light levels */
static uint8_t colormap[64 * 256];

/* Simple sin/cos LUT (8-bit fixed-point, 256 entries = full circle) */
static int16_t sin_lut[256];
static int16_t cos_lut[256];

static void build_sin_table(void) {
    for (int i = 0; i < 256; i++) {
        double a = i * 2.0 * 3.14159265 / 256.0;
        sin_lut[i] = (int16_t)(sin(a) * 256.0);
        cos_lut[i] = (int16_t)(cos(a) * 256.0);
    }
}

static void build_colormap(void) {
    /* Generate a simple colormap: each light level dims the palette */
    for (int light = 0; light < 64; light++) {
        for (int i = 0; i < 256; i++) {
            int dimmed = (i * (63 - light)) / 63;
            if (dimmed > 255) dimmed = 255;
            colormap[light * 256 + i] = (uint8_t)dimmed;
        }
    }
}

static void build_checkerboard(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            checkerboard_tex[y * 64 + x] = ((x ^ y) & 8) ? 0xE0 : 0x40;
}

static void build_wall_texture(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            /* Brick-like pattern */
            int bx = x % 32, by = y % 16;
            int offset = (y / 16) & 1 ? 16 : 0;
            bx = (x + offset) % 32;
            if (bx == 0 || by == 0)
                wall_tex[y * 64 + x] = 0x30;  /* mortar */
            else
                wall_tex[y * 64 + x] = 0x90 + (bx & 3) * 4;  /* brick */
        }
}

/* 16×16 sprite — a small filled circle, 0xFF outside (transparent) */
static void build_sprite_texture(void) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int dx = x - 8, dy = y - 8;
            int d2 = dx*dx + dy*dy;
            if (d2 > 49) {
                sprite_tex[y * 16 + x] = 0xFF;            /* transparent */
            } else if (d2 > 25) {
                sprite_tex[y * 16 + x] = 0x40 + d2;       /* outline */
            } else {
                sprite_tex[y * 16 + x] = 0xC0 - (d2 * 2); /* fill */
            }
        }
    }
}

/* 64×64 grid texture for the perspective demo */
static void build_persp_texture(void) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int gx = x & 7, gy = y & 7;
            uint8_t v;
            if (gx == 0 || gy == 0)
                v = 0xFF;                                 /* grid line (white) */
            else
                v = 0x20 + ((x >> 3) ^ (y >> 3)) * 0x18;  /* checker fill */
            persp_tex[y * 64 + x] = v;
        }
    }
}

static void set_palette(void) {
    /* Simple grayscale + warm tint for upper half.
     *
     * IMPORTANT: skip indexes 0-15 — those are the OS terminal's VGA
     * 16-color palette (used for white text in OF_DISPLAY_OVERLAY mode).
     * Overwriting them blanks the overlay text. */
    for (int i = 16; i < 256; i++) {
        uint8_t r = i, g = i, b = i;
        if (i >= 0x80) { r = i; g = (i * 7) >> 3; b = (i * 5) >> 3; }
        of_video_palette(i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
}

/* ================================================================
 * Span Demo: Doom-style textured walls + floor
 * ================================================================ */
/* Per-frame FB setup. We CPU-side clear the entire 320x240 framebuffer
 * instead of using the hardware GPU CLEAR command, because on real
 * hardware the GPU's AXI write→read transition (M1 → M0) is buggy: a
 * long write burst from CLEAR leaves the SDRAM arbiter or slave in a
 * state where the very next AXI read (the texture cache miss for the
 * first DRAW_SPAN's first pixel) never returns. CPU memset uses the
 * normal CPU AXI master (M2), which we know works. cache_clean_range
 * flushes the dirty CPU lines so the GPU sees the cleared bytes when
 * it reads the framebuffer. (TODO: fix the M1→M0 hang in the SDRAM
 * arbiter / slave; see arbitration comments in axi_sdram_arbiter.v.) */
static void prepare_fb(uint8_t *fb, uint8_t color) {
    memset(fb, color, SCREEN_W * SCREEN_H);
    OF_SVC->cache_clean_range(fb, SCREEN_W * SCREEN_H);
    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb, SCREEN_W);
}

static void draw_span_demo(int frame) {
    int trace = (frame < 3);
    if (trace) printf("[span] enter frame=%d\n", frame);

    uint8_t *fb = of_video_surface();
    uint32_t fb_addr = (uint32_t)(uintptr_t)fb;
    uint32_t tex_addr = (uint32_t)(uintptr_t)wall_tex;
    uint32_t floor_addr = (uint32_t)(uintptr_t)checkerboard_tex;
    if (trace) printf("[span] fb=%p tex=%p floor=%p\n", fb, wall_tex, checkerboard_tex);

    prepare_fb(fb, 0x10);
    if (trace) printf("[span] prepare_fb done\n");

    /* Draw vertical wall columns (Doom R_DrawColumn style) */
    int wall_h = 80 + sin_lut[frame & 255] / 8;
    if (wall_h < 20) wall_h = 20;
    if (wall_h > 180) wall_h = 180;
    int wall_top = (SCREEN_H - wall_h) / 2;

    for (int x = 0; x < SCREEN_W; x++) {
        /* Trace ONLY the first few columns. */
        if (trace && (x == 0 || x == 1 || x == 2 || x == 3)) {
            uint32_t rd = GPU_RING_RDPTR;
            uint32_t st = GPU_STATUS;
            printf("[s] BEFORE x=%d r=%lu st=%lu busy=%lu\n",
                   x, (unsigned long)rd,
                   (unsigned long)((st >> 2) & 0x3F),
                   (unsigned long)(st & 1));
        }
        int light = (x * 40 / SCREEN_W);  /* darken towards edges */
        /* Column walks T (row) through the 64×64 texture, S (col) is fixed.
         * tex_width=64: addr = base + (t>>16)*64 + (s>>16) */
        of_gpu_span_t col = {
            .fb_addr   = fb_addr + wall_top * SCREEN_W + x,
            .tex_addr  = tex_addr,
            .s         = ((x + frame) & 63) << 16,  /* fixed U column */
            .t         = 0,                           /* start at row 0 */
            .sstep     = 0,                           /* S doesn't change */
            .tstep     = (64 << 16) / wall_h,        /* T walks down */
            .count     = wall_h,
            .light     = light,
            .flags     = OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_COLUMN,
            .fb_stride = SCREEN_W,
            .tex_width = 64,
        };
        of_gpu_draw_span(&col);
        if (trace && (x == 0 || x == 1 || x == 2 || x == 3))
            printf("[s] DRAW   x=%d done, kicking\n", x);

        /* Manual kick + busy poll, no FENCE. Mirrors the standalone
         * diagnostic that worked end-to-end. of_gpu_finish() (which
         * queues a CMD_FENCE and polls GPU_FENCE_REACHED) hangs on
         * the very first column even though the SAME draw_span
         * parameters worked in the diagnostic. */
        of_gpu_kick();
        {
            uint32_t spins = 0;
            while ((GPU_STATUS & 1) || ((GPU_STATUS & 2) == 0)) {
                /* busy=bit0, ring_empty=bit1 — exit when both clear */
                if (++spins == 5000000) {
                    uint32_t st = GPU_STATUS;
                    printf("[s] x=%d STUCK st=0x%lx state=%lu busy=%lu emp=%lu fps=%lu\n",
                           x, (unsigned long)st,
                           (unsigned long)((st >> 2) & 0x3F),
                           (unsigned long)(st & 1),
                           (unsigned long)((st >> 1) & 1),
                           (unsigned long)((st >> 12) & 1));
                    spins = 0;
                }
            }
        }
        if (trace && (x == 0 || x == 1 || x == 2 || x == 3))
            printf("[s] WAIT   x=%d ok\n", x);
    }

    if (trace) printf("[span] walls submitted, starting floor\n");

    /* Draw floor spans */
    for (int y = wall_top + wall_h; y < SCREEN_H; y++) {
        int dist = y - (SCREEN_H / 2);
        if (dist <= 0) dist = 1;
        int light = (dist > 50) ? 50 : dist;
        int step = (64 << 16) / (dist * 2);
        of_gpu_span_t span = {
            .fb_addr   = fb_addr + y * SCREEN_W,
            .tex_addr  = floor_addr,
            .s         = (frame * 0x4000),
            .t         = (y * 0x8000),
            .sstep     = step,
            .tstep     = 0,
            .count     = SCREEN_W,
            .light     = light > 63 ? 63 : light,
            .flags     = OF_GPU_SPAN_COLORMAP,
            .fb_stride = 1,
            .tex_width = 64,
        };
        of_gpu_draw_span(&span);
    }

    if (trace) printf("[span] floor submitted, starting sprite\n");

    /* SKIP_ZERO sprite overlay — bouncing transparent circle. The texel
     * value 0xFF marks transparent pixels and the GPU drops them at the
     * fragment stage so they never touch the framebuffer. */
    {
        int sx = (SCREEN_W / 2 - 8) + (sin_lut[frame & 255] * 80) / 256;
        int sy = (SCREEN_H / 2 - 8) + (cos_lut[(frame * 2) & 255] * 40) / 256;
        if (sx < 0) sx = 0;
        if (sx > SCREEN_W - 16) sx = SCREEN_W - 16;
        if (sy < 0) sy = 0;
        if (sy > SCREEN_H - 16) sy = SCREEN_H - 16;

        for (int row = 0; row < 16; row++) {
            of_gpu_span_t s = {
                .fb_addr   = fb_addr + (sy + row) * SCREEN_W + sx,
                .tex_addr  = (uint32_t)(uintptr_t)sprite_tex,
                .s         = 0,
                .t         = ((int32_t)row) << 16,
                .sstep     = 1 << 16,
                .tstep     = 0,
                .count     = 16,
                .light     = 0,
                .flags     = OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_SKIP_ZERO,
                .fb_stride = 1,
                .tex_width = 16,
            };
            of_gpu_draw_span(&s);
        }
    }

    if (trace) printf("[span] all spans submitted, calling gpu_finish\n");
    of_gpu_finish();
    if (trace) printf("[span] gpu_finish returned\n");
}

/* ================================================================
 * Triangle Demo: Rotating 3D cube
 * ================================================================ */

/* Cube: 8 vertices, 12 triangles (2 per face) */
static const int8_t cube_verts[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const uint8_t cube_faces[12][3] = {
    {0,1,2},{0,2,3}, {4,6,5},{4,7,6},  /* front, back */
    {0,4,5},{0,5,1}, {2,6,7},{2,7,3},  /* bottom, top */
    {0,3,7},{0,7,4}, {1,5,6},{1,6,2},  /* left, right */
};
static const uint8_t face_colors[6] = { 0xC0, 0xA0, 0x80, 0xE0, 0x60, 0xB0 };

/* Per-face 1-texel textures — avoids race between CPU write and GPU read.
 * Heap-allocated (in SDRAM) at startup, just like the other textures. */
static uint8_t *face_tex;

static void draw_triangle_demo(int frame) {
    uint8_t *fb = of_video_surface();

    prepare_fb(fb, 0x08);

    /* Initialise per-face texels and flush to SDRAM before GPU draws */
    for (int i = 0; i < 6; i++)
        face_tex[i] = face_colors[i];
    OF_SVC->cache_clean_range(face_tex, 6);

    int angle_y = frame * 2;
    int angle_x = frame;
    int sy = sin_lut[angle_y & 255], cy = cos_lut[angle_y & 255];
    int sx = sin_lut[angle_x & 255], cx = cos_lut[angle_x & 255];

    /* Transform and project vertices */
    int16_t proj_x[8], proj_y[8];
    for (int i = 0; i < 8; i++) {
        int x = cube_verts[i][0] * 80;
        int y = cube_verts[i][1] * 80;
        int z = cube_verts[i][2] * 80;
        /* Rotate Y */
        int rx = (x * cy - z * sy) >> 8;
        int rz = (x * sy + z * cy) >> 8;
        /* Rotate X */
        int ry = (y * cx - rz * sx) >> 8;
        rz = (y * sx + rz * cx) >> 8;
        /* Perspective projection */
        int d = 300 + rz;
        if (d < 50) d = 50;
        proj_x[i] = (int16_t)(SCREEN_W / 2 + (rx * 200) / d);
        proj_y[i] = (int16_t)(SCREEN_H / 2 + (ry * 200) / d);
    }

    /* Draw each face as 2 triangles */
    for (int f = 0; f < 12; f++) {
        int i0 = cube_faces[f][0];
        int i1 = cube_faces[f][1];
        int i2 = cube_faces[f][2];

        /* Simple backface cull via cross product */
        int dx1 = proj_x[i1] - proj_x[i0], dy1 = proj_y[i1] - proj_y[i0];
        int dx2 = proj_x[i2] - proj_x[i0], dy2 = proj_y[i2] - proj_y[i0];
        if (dx1 * dy2 - dx2 * dy1 <= 0) continue;

        /* Bind per-face 1-texel texture (no race: each face has its own) */
        of_gpu_texture_t solid_tex = {
            .addr = (uint32_t)(uintptr_t)&face_tex[f / 2],
            .width = 1, .height = 1,
            .format = OF_GPU_TEXFMT_I8,
            .wrap_s = OF_GPU_WRAP_REPEAT, .wrap_t = OF_GPU_WRAP_REPEAT,
        };
        of_gpu_bind_texture(&solid_tex);

        of_gpu_vertex_t tri[3] = {
            { .x = proj_x[i0] * 16, .y = proj_y[i0] * 16, .r = 0 },
            { .x = proj_x[i1] * 16, .y = proj_y[i1] * 16, .r = 0 },
            { .x = proj_x[i2] * 16, .y = proj_y[i2] * 16, .r = 0 },
        };
        of_gpu_draw_triangles(tri, 3);
    }

    of_gpu_finish();
}

/* ================================================================
 * Perspective Span Demo: rotating textured triangle, software rasterised
 * with hardware perspective-correct spans (SPAN_PERSP)
 * ================================================================
 *
 * The CPU walks the triangle scanline-by-scanline; for each scanline it
 * computes (s/z, t/z, 1/z) at the left and right edges in 16.16 fixed
 * point and feeds them to the GPU as a SPAN_PERSP. The GPU then runs the
 * recip + multiply per 16-pixel sub-segment in hardware, so the resulting
 * texturing is perspective-correct (no affine warping inside polygons).
 */

typedef struct { int32_t x, y, z; int32_t s, t; } persp_vert_t;

static int32_t fdiv16(int32_t num, int32_t den) {
    /* signed 16.16 division: (num << 16) / den, clamped */
    if (den == 0) return 0;
    int64_t n = ((int64_t)num) << 16;
    return (int32_t)(n / den);
}

static void draw_persp_demo(int frame) {
    uint8_t *fb = of_video_surface();
    uint32_t fb_addr = (uint32_t)(uintptr_t)fb;

    prepare_fb(fb, 0x10);

    /* Three vertices of a triangle in world space, rotating about the
     * world Y axis. Texture coords (s, t) are attached at vertex setup. */
    int ang = frame & 255;
    int s_a = sin_lut[ang], c_a = cos_lut[ang];

    persp_vert_t verts[3];
    static const int16_t base[3][3] = {
        { -80, -60, 0 },
        {  80, -60, 0 },
        {   0,  80, 0 },
    };
    static const int16_t tex[3][2] = {
        {   0,   0 },
        {  64,   0 },
        {  32,  64 },
    };
    for (int i = 0; i < 3; i++) {
        /* Rotate base[i] about Y so the triangle tilts in/out of the screen */
        int x0 = base[i][0], y0 = base[i][1], z0 = base[i][2];
        int rx = (x0 * c_a - z0 * s_a) >> 8;
        int rz = (x0 * s_a + z0 * c_a) >> 8;
        verts[i].x = rx;
        verts[i].y = y0;
        verts[i].z = 200 + rz;        /* push triangle in front of camera */
        verts[i].s = (int32_t)tex[i][0] << 16;
        verts[i].t = (int32_t)tex[i][1] << 16;
    }

    /* Project to screen + compute per-vertex (s/z, t/z, 1/z) */
    int32_t sx[3], sy[3];
    int32_t sZ[3], tZ[3], oZ[3];   /* projection-space attributes (16.16) */
    for (int i = 0; i < 3; i++) {
        if (verts[i].z < 16) verts[i].z = 16;
        int32_t zi = verts[i].z;
        sx[i] = (verts[i].x * 200) / zi + (SCREEN_W / 2);
        sy[i] = (verts[i].y * 200) / zi + (SCREEN_H / 2);
        sZ[i] = fdiv16(verts[i].s, zi << 0);  /* s/z, 16.16 */
        tZ[i] = fdiv16(verts[i].t, zi << 0);
        oZ[i] = fdiv16(1 << 16,    zi);       /* 1/z, 16.16 */
    }

    /* Sort vertices by Y (top, mid, bot) */
    int top = 0, mid = 1, bot = 2;
    if (sy[mid] < sy[top]) { int t = top; top = mid; mid = t; }
    if (sy[bot] < sy[top]) { int t = top; top = bot; bot = t; }
    if (sy[bot] < sy[mid]) { int t = mid; mid = bot; bot = t; }

    int y_top = sy[top], y_mid = sy[mid], y_bot = sy[bot];
    if (y_bot <= y_top) { of_gpu_finish(); return; }

    /* Walk each scanline from y_top..y_bot. For each scanline, the left
     * and right edges are interpolated linearly in screen y between the
     * appropriate pair of vertices. (s/z, t/z, 1/z) interpolate linearly
     * the same way — that's the whole point of perspective division. */
    for (int y = y_top; y <= y_bot; y++) {
        if (y < 0 || y >= SCREEN_H) continue;

        int e1_a = top, e1_b = bot;                          /* long edge */
        int e2_a, e2_b;
        if (y < y_mid) { e2_a = top; e2_b = mid; }
        else           { e2_a = mid; e2_b = bot; }

        int dy1 = sy[e1_b] - sy[e1_a];
        int dy2 = sy[e2_b] - sy[e2_a];
        if (dy1 == 0) dy1 = 1;
        if (dy2 == 0) dy2 = 1;
        int t1 = ((y - sy[e1_a]) << 16) / dy1;  /* 16.16 fraction */
        int t2 = ((y - sy[e2_a]) << 16) / dy2;

        /* Linearly interpolate edge x and projection-space attribs */
        int32_t x1 = sx[e1_a] + (((int64_t)(sx[e1_b] - sx[e1_a]) * t1) >> 16);
        int32_t x2 = sx[e2_a] + (((int64_t)(sx[e2_b] - sx[e2_a]) * t2) >> 16);
        int32_t sZ1 = sZ[e1_a] + (int32_t)(((int64_t)(sZ[e1_b] - sZ[e1_a]) * t1) >> 16);
        int32_t sZ2 = sZ[e2_a] + (int32_t)(((int64_t)(sZ[e2_b] - sZ[e2_a]) * t2) >> 16);
        int32_t tZ1 = tZ[e1_a] + (int32_t)(((int64_t)(tZ[e1_b] - tZ[e1_a]) * t1) >> 16);
        int32_t tZ2 = tZ[e2_a] + (int32_t)(((int64_t)(tZ[e2_b] - tZ[e2_a]) * t2) >> 16);
        int32_t oZ1 = oZ[e1_a] + (int32_t)(((int64_t)(oZ[e1_b] - oZ[e1_a]) * t1) >> 16);
        int32_t oZ2 = oZ[e2_a] + (int32_t)(((int64_t)(oZ[e2_b] - oZ[e2_a]) * t2) >> 16);

        int32_t xl = x1 < x2 ? x1 : x2;
        int32_t xr = x1 < x2 ? x2 : x1;
        if (x2 < x1) {
            int32_t tmp;
            tmp = sZ1; sZ1 = sZ2; sZ2 = tmp;
            tmp = tZ1; tZ1 = tZ2; tZ2 = tmp;
            tmp = oZ1; oZ1 = oZ2; oZ2 = tmp;
        }

        if (xl < 0) xl = 0;
        if (xr >= SCREEN_W) xr = SCREEN_W - 1;
        int count = xr - xl + 1;
        if (count <= 0) continue;

        int32_t inv_count = (1 << 16) / count;  /* 16.16 of 1/count */
        of_gpu_span_t span = {
            .fb_addr     = fb_addr + y * SCREEN_W + xl,
            .tex_addr    = (uint32_t)(uintptr_t)persp_tex,
            .s           = 0,                              /* unused (PERSP) */
            .t           = 0,
            .sstep       = 0,
            .tstep       = 0,
            .count       = count,
            .light       = 0,
            .flags       = OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_PERSP,
            .fb_stride   = 1,
            .tex_width   = 64,
            .sdivz       = sZ1,
            .tdivz       = tZ1,
            .zi_persp    = oZ1,
            .sdivz_step  = (int32_t)(((int64_t)(sZ2 - sZ1) * inv_count) >> 16),
            .tdivz_step  = (int32_t)(((int64_t)(tZ2 - tZ1) * inv_count) >> 16),
            .zi_step     = (int32_t)(((int64_t)(oZ2 - oZ1) * inv_count) >> 16),
        };
        of_gpu_draw_span(&span);
    }

    of_gpu_finish();
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    set_palette();
    printf("[gpudemo] start\n");

    /* Allocate textures from the heap (SDRAM, reachable by both CPU and GPU). */
    checkerboard_tex = malloc(64 * 64);
    wall_tex         = malloc(64 * 64);
    sprite_tex       = malloc(16 * 16);
    persp_tex        = malloc(64 * 64);
    face_tex         = malloc(8);  /* 6 bytes rounded up for alignment */
    printf("[gpudemo] malloc: cb=%p wall=%p sprite=%p persp=%p face=%p\n",
           checkerboard_tex, wall_tex, sprite_tex, persp_tex, face_tex);
    if (!checkerboard_tex || !wall_tex || !sprite_tex || !persp_tex || !face_tex) {
        printf("[gpudemo] FATAL: out of heap for textures\n");
        return 1;
    }

    build_sin_table();
    build_colormap();
    build_checkerboard();
    build_wall_texture();
    build_sprite_texture();
    build_persp_texture();
    printf("[gpudemo] textures built\n");

    /* Flush texture data from CPU D-cache to SDRAM so the GPU can read it */
    OF_SVC->cache_clean_range(checkerboard_tex, 64 * 64);
    OF_SVC->cache_clean_range(wall_tex,         64 * 64);
    OF_SVC->cache_clean_range(sprite_tex,       16 * 16);
    OF_SVC->cache_clean_range(persp_tex,        64 * 64);
    OF_SVC->cache_clean_range(face_tex,         8);
    printf("[gpudemo] cache_clean done\n");

    /* The stock of_gpu_init() only pulses ring_reset (GPU_CTRL=4). On real
     * hardware the GPU FSM may have been processing garbage from boot-time
     * ring BRAM, so we also pulse soft_reset (bit1) here to force the FSM
     * back to S_IDLE before init clears the ring. */
    GPU_CTRL = 6;  /* soft_reset | ring_reset */
    {
        volatile int i;
        for (i = 0; i < 100; i++) ;
    }
    of_gpu_init();
    printf("[gpudemo] gpu_init ok\n");

    of_gpu_colormap_upload(colormap, sizeof(colormap));
    printf("[gpudemo] colormap uploaded\n");

    {
        uint8_t *fb0 = of_video_surface();
        printf("[gpudemo] surface[0] = %p\n", fb0);
    }

    printf("[gpudemo] entering main loop — A = cycle mode\n");

    int mode = 0;  /* 0 = spans+sprite, 1 = triangle cube, 2 = persp */
    int frame = 0;

    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) {
            mode = (mode + 1) % 3;
            printf("[gpudemo] switched to mode %d\n", mode);
        }

        if (frame < 5 || (frame % 60) == 0)
            printf("[gpudemo] frame=%d mode=%d enter draw\n", frame, mode);

        switch (mode) {
            case 0: draw_span_demo(frame);     break;
            case 1: draw_triangle_demo(frame); break;
            case 2: draw_persp_demo(frame);    break;
        }

        if (frame < 5 || (frame % 60) == 0)
            printf("[gpudemo] frame=%d draw returned, flipping\n", frame);

        of_video_flip();
        frame++;
    }
}
