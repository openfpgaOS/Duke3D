/*
 * of_smp_bank.h -- On-device runtime bank format (.ofsf) for SF2
 *                  sample-based MIDI synthesis.
 *
 * A .ofsf file is a flat, pre-resolved binary loaded directly into
 * CRAM1 with no on-device parsing.  All SF2 generators are baked into
 * per-zone fields by the offline converter.
 *
 * Layout:  [header 32B] [preset_index 1024B] [zones N*80B] [sample blob]
 */

#ifndef OF_SMP_BANK_H
#define OF_SMP_BANK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OFSF_MAGIC      0x4F465346  /* 'OFSF' */
#define OFSF_VERSION    2

#define OFSF_PRESET_COUNT   256     /* 128 melodic (bank 0) + 128 drum (bank 128) */

/* Metadata strings (null-terminated, truncated if longer) */
#define OFSF_NAME_MAX       32
#define OFSF_AUTHOR_MAX     32

/* Loop modes */
#define OFSF_LOOP_NONE      0
#define OFSF_LOOP_FORWARD   1
#define OFSF_LOOP_BIDI      3

/* ---- File header (96 bytes) ----
 *
 * bank_name and bank_author are pulled from the SF2 INFO LIST (INAM/IENG)
 * by sf2_to_ofsf so the OS can display them at boot without opening the
 * SF2 source. */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* OFSF_MAGIC */
    uint32_t version;           /* OFSF_VERSION */
    uint32_t sample_rate;       /* native rate of all samples (e.g. 44100) */
    uint32_t zone_count;        /* total zones in file */
    uint32_t sample_data_offset;/* bytes from file start to sample blob */
    uint32_t sample_data_size;  /* sample blob size in bytes */
    uint32_t flags;             /* reserved, 0 */
    uint32_t reserved;
    char     bank_name[OFSF_NAME_MAX];     /* v2: SF2 INAM, null-terminated */
    char     bank_author[OFSF_AUTHOR_MAX]; /* v2: SF2 IENG, null-terminated */
} ofsf_header_t;

/* ---- Preset index (256 x 4 bytes = 1024 bytes) ---- */

typedef struct __attribute__((packed)) {
    uint16_t zone_start;        /* first zone index for this preset */
    uint16_t zone_count;        /* number of zones */
} ofsf_preset_t;

/* ---- Zone (80 bytes) ---- */

typedef struct __attribute__((packed)) {
    /* Key/velocity range */
    uint8_t  key_lo;
    uint8_t  key_hi;
    uint8_t  vel_lo;
    uint8_t  vel_hi;

    /* Sample reference (offsets relative to sample blob start) */
    uint32_t sample_offset;     /* bytes from sample blob start */
    uint32_t sample_length;     /* in samples */
    uint32_t loop_start;        /* in samples */
    uint32_t loop_end;          /* in samples */

    uint8_t  loop_mode;         /* OFSF_LOOP_* */
    uint8_t  root_key;          /* MIDI note of original pitch */
    int8_t   fine_tune;         /* cents, -99..+99 */
    int8_t   coarse_tune;       /* semitones */

    /* Volume envelope (DAHDSR) -- all timecents except sustain */
    int16_t  vol_delay;         /* timecents */
    int16_t  vol_attack;        /* timecents */
    int16_t  vol_hold;          /* timecents */
    int16_t  vol_decay;         /* timecents */
    int16_t  vol_sustain;       /* centibels attenuation */
    int16_t  vol_release;       /* timecents */

    /* Modulation envelope (DAHDSR) + routing */
    int16_t  mod_delay;         /* timecents */
    int16_t  mod_attack;        /* timecents */
    int16_t  mod_hold;          /* timecents */
    int16_t  mod_decay;         /* timecents */
    int16_t  mod_sustain;       /* centibels attenuation */
    int16_t  mod_release;       /* timecents */
    int16_t  mod_env_to_pitch;  /* cents */
    int16_t  mod_env_to_filter; /* cents */

    /* Modulation LFO */
    int16_t  mod_lfo_delay;     /* timecents */
    int16_t  mod_lfo_freq;      /* absolute cents */
    int16_t  mod_lfo_to_pitch;  /* cents */
    int16_t  mod_lfo_to_filter; /* cents */
    int16_t  mod_lfo_to_volume; /* centibels */

    /* Vibrato LFO */
    int16_t  vib_lfo_delay;     /* timecents */
    int16_t  vib_lfo_freq;      /* absolute cents */
    int16_t  vib_lfo_to_pitch;  /* cents */

    /* Filter */
    int16_t  initial_fc;        /* cents, 8400 = 20 kHz default */
    int16_t  initial_q;         /* centibels, 0-960 */

    /* Output */
    int16_t  initial_attn;      /* centibels */
    int16_t  pan;               /* -500..+500 (SF2 units) */

    uint8_t  exclusive_class;   /* 0 = none */
    uint8_t  _pad[3];
} ofsf_zone_t;

/* ---- Runtime API ----
 *
 * No load/unload: the kernel auto-loads a .ofsf file staged in any data
 * slot at boot, and an SDK constructor binds this module to it before
 * main() runs. Apps just use the query functions below. */

const ofsf_header_t *of_smp_bank_get(void);
const void          *of_smp_bank_sample_base(void);

int of_smp_zone_lookup(int bank, int program, int key, int velocity,
                       const ofsf_zone_t **zones_out, int max_zones);

#ifdef __cplusplus
}
#endif

#endif /* OF_SMP_BANK_H */
