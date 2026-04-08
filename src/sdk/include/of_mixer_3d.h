/*
 * of_mixer_3d.h -- 3D Audio Spatialization for openfpgaOS
 *
 * CPU-side distance/angle calculation that drives of_mixer volume and pan.
 * No hardware changes — all math runs on the CPU.
 *
 * Usage:
 *   of_mixer_3d_init();
 *   of_mixer_3d_set_listener(player_x, player_y, player_angle);
 *   int voice = of_mixer_play(...);
 *   of_mixer_3d_set_source(voice, source_x, source_y);
 *   // Each frame:
 *   of_mixer_3d_update();   // recomputes all active voice volumes/pans
 *
 * IMPORTANT: This header contains static mutable state (source table,
 * listener position, attenuation parameters). Include it from exactly
 * ONE translation unit per program — multi-TU apps would otherwise end
 * up with independent copies of the mixer state.
 */

#ifndef OF_MIXER_3D_H
#define OF_MIXER_3D_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "of_mixer.h"

/* Fixed-point helpers (16.16) */
#define OF_MIXER_3D_FP(x)    ((int32_t)((x) * 65536))
#define OF_MIXER_3D_INT(x)   ((x) >> 16)

/* Private implementation helpers (prefix _m3d_) are subject to change
 * without notice — use the of_mixer_3d_* API. */

/* Attenuation models */
#define OF_MIXER_3D_ATTEN_LINEAR  0   /* vol = 1 - dist/max_dist            */
#define OF_MIXER_3D_ATTEN_INV     1   /* vol = ref_dist / dist              */
#define OF_MIXER_3D_ATTEN_INV_CLAMP 2 /* vol = ref_dist / max(dist, ref_dist) */

/* ======================================================================
 * State
 * ====================================================================== */

#define OF_MIXER_3D_MAX_SOURCES 32

typedef struct {
    int32_t  x, y;          /* 16.16 fixed-point position */
    int      active;
    int      min_dist;      /* distance below which volume is max (16.16) */
    int      max_dist;      /* distance above which volume is 0 (16.16) */
    int      base_vol;      /* 0-255, source volume before attenuation */
} _m3d_source_t;

static int32_t _m3d_listener_x;
static int32_t _m3d_listener_y;
static int32_t _m3d_listener_angle;  /* 0-65535 = full circle (16-bit BAM) */
static int _m3d_atten_model = OF_MIXER_3D_ATTEN_INV_CLAMP;
static int _m3d_default_ref_dist = OF_MIXER_3D_FP(64);
static int _m3d_default_max_dist = OF_MIXER_3D_FP(1024);
static _m3d_source_t _m3d_sources[OF_MIXER_3D_MAX_SOURCES];

/* ======================================================================
 * Integer sqrt (16-bit result from 32-bit input)
 * ====================================================================== */

static inline uint16_t _m3d_isqrt(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n;
    uint32_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return (uint16_t)x;
}

/* ======================================================================
 * Sine table (256 entries, 8.8 fixed-point, quarter-wave)
 * ====================================================================== */

static const int16_t _m3d_sin_tab[65] = {
       0,   6,  13,  19,  25,  31,  37,  44,
      50,  56,  62,  68,  74,  80,  86,  92,
      98, 103, 109, 115, 120, 126, 131, 136,
     142, 147, 152, 157, 162, 167, 171, 176,
     181, 185, 189, 193, 197, 201, 205, 209,
     212, 216, 219, 222, 225, 228, 231, 234,
     236, 238, 241, 243, 244, 246, 248, 249,
     251, 252, 253, 254, 254, 255, 255, 255,
     256,
};

/* sin(angle) where angle is 0-255 = full circle. Returns -256..+256. */
static inline int _m3d_sin(int angle) {
    angle &= 255;
    int quadrant = angle >> 6;
    int idx = angle & 63;
    int val;
    switch (quadrant) {
        case 0: val =  _m3d_sin_tab[idx]; break;
        case 1: val =  _m3d_sin_tab[64 - idx]; break;
        case 2: val = -_m3d_sin_tab[idx]; break;
        default: val = -_m3d_sin_tab[64 - idx]; break;
    }
    return val;
}

static inline int _m3d_cos(int angle) {
    return _m3d_sin(angle + 64);
}

/* ======================================================================
 * API
 * ====================================================================== */

static inline void of_mixer_3d_init(void) {
    _m3d_listener_x = 0;
    _m3d_listener_y = 0;
    _m3d_listener_angle = 0;
    for (int i = 0; i < OF_MIXER_3D_MAX_SOURCES; i++)
        _m3d_sources[i].active = 0;
}

static inline void of_mixer_3d_set_listener(int32_t x, int32_t y, int32_t angle) {
    _m3d_listener_x = x;
    _m3d_listener_y = y;
    _m3d_listener_angle = angle;
}

static inline void of_mixer_3d_set_attenuation(int model, int ref_dist, int max_dist) {
    _m3d_atten_model = model;
    _m3d_default_ref_dist = ref_dist;
    _m3d_default_max_dist = max_dist;
}

static inline void of_mixer_3d_set_source(int voice, int32_t x, int32_t y) {
    if (voice < 0 || voice >= OF_MIXER_3D_MAX_SOURCES) return;
    _m3d_sources[voice].x = x;
    _m3d_sources[voice].y = y;
    _m3d_sources[voice].active = 1;
    _m3d_sources[voice].min_dist = _m3d_default_ref_dist;
    _m3d_sources[voice].max_dist = _m3d_default_max_dist;
    _m3d_sources[voice].base_vol = 255;
}

static inline void of_mixer_3d_set_source_dist(int voice, int min_dist, int max_dist) {
    if (voice < 0 || voice >= OF_MIXER_3D_MAX_SOURCES) return;
    _m3d_sources[voice].min_dist = min_dist;
    _m3d_sources[voice].max_dist = max_dist;
}

static inline void of_mixer_3d_set_source_volume(int voice, int volume) {
    if (voice < 0 || voice >= OF_MIXER_3D_MAX_SOURCES) return;
    _m3d_sources[voice].base_vol = volume & 0xFF;
}

static inline void of_mixer_3d_remove(int voice) {
    if (voice < 0 || voice >= OF_MIXER_3D_MAX_SOURCES) return;
    _m3d_sources[voice].active = 0;
}

/* Recompute volume + pan for all active 3D sources. Call once per frame. */
static inline void of_mixer_3d_update(void) {
    for (int i = 0; i < OF_MIXER_3D_MAX_SOURCES; i++) {
        _m3d_source_t *s = &_m3d_sources[i];
        if (!s->active) continue;

        /* Distance from listener to source (fixed-point) */
        int32_t dx = s->x - _m3d_listener_x;
        int32_t dy = s->y - _m3d_listener_y;

        /* Compute distance using integer sqrt.
         * Scale down to avoid overflow: dx/dy are 16.16, shift to 16.8 before squaring. */
        int32_t dx8 = dx >> 8;
        int32_t dy8 = dy >> 8;
        uint32_t dist_sq = (uint32_t)(dx8 * dx8 + dy8 * dy8);
        int32_t dist = (int32_t)_m3d_isqrt(dist_sq) << 8;  /* back to 16.16 */

        /* Attenuation */
        int vol;
        switch (_m3d_atten_model) {
            case OF_MIXER_3D_ATTEN_LINEAR:
                if (dist >= s->max_dist)
                    vol = 0;
                else if (dist <= s->min_dist)
                    vol = s->base_vol;
                else
                    vol = (s->base_vol * (s->max_dist - dist)) / (s->max_dist - s->min_dist);
                break;
            case OF_MIXER_3D_ATTEN_INV:
                if (dist <= 0) dist = 1;
                vol = (s->base_vol * s->min_dist) / dist;
                if (vol > s->base_vol) vol = s->base_vol;
                break;
            case OF_MIXER_3D_ATTEN_INV_CLAMP:
            default:
                if (dist < s->min_dist) dist = s->min_dist;
                if (dist >= s->max_dist) { vol = 0; break; }
                if (dist <= 0) dist = 1;
                vol = (s->base_vol * s->min_dist) / dist;
                if (vol > s->base_vol) vol = s->base_vol;
                break;
        }

        /* Angle from listener to source, relative to listener facing direction.
         * Use atan2 approximation via quadrant + ratio. */
        int pan = 128;  /* default center */
        if (dist > s->min_dist / 4) {
            /* Compute angle: listener_angle is 16-bit BAM (0-65535).
             * Convert to 256-step for sin/cos lookup. */
            int la = (_m3d_listener_angle >> 8) & 255;

            /* Rotate dx/dy by -listener_angle to get relative direction.
             * right_component = dx*cos + dy*sin (positive = right of listener)
             * We only need the sign and magnitude for panning. */
            int c = _m3d_cos(la);
            int sn = _m3d_sin(la);

            /* dx/dy are 16.16, c/sn are -256..256 (8.8-ish).
             * Shift dx/dy down to avoid overflow: use top 16 bits. */
            int32_t dx16 = dx >> 16;
            int32_t dy16 = dy >> 16;
            int32_t right = dx16 * c + dy16 * sn;  /* positive = source is to the right */

            /* Normalize: pan range 0-255, where 128=center */
            if (dist > 0) {
                int32_t dist16 = dist >> 16;
                if (dist16 <= 0) dist16 = 1;
                int pan_offset = (right * 127) / dist16;
                if (pan_offset > 127) pan_offset = 127;
                if (pan_offset < -128) pan_offset = -128;
                pan = 128 + pan_offset;
            }
        }

        of_mixer_set_volume(i, vol);
        of_mixer_set_pan(i, pan);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* OF_MIXER_3D_H */
