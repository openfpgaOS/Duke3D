/*
 * moddemo — Minimal 4-channel MOD player for openfpgaOS
 *
 * All mixing in FPGA hardware. CPU only:
 *   1. Parses MOD header, uploads 8-bit samples to CRAM1
 *   2. Reads pattern data and updates voice registers at tick rate
 *   3. Computes effects (portamento, volume slide, arpeggio) in software
 *
 * Loads MOD file from data slot 3.
 * Press A to pause/resume, B to skip to next pattern.
 */

#include "of.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================
 * MOD format structures
 * ====================================================================== */

#define MOD_NUM_SAMPLES   31
#define MOD_NUM_CHANNELS  4
#define MOD_ROWS_PER_PAT  64
#define MOD_MAX_PATTERNS  128

typedef struct {
    char     name[23];
    uint16_t length;       /* in words (×2 for bytes) */
    int8_t   finetune;     /* -8 to +7 */
    uint8_t  volume;       /* 0-64 */
    uint16_t loop_start;   /* in words */
    uint16_t loop_length;  /* in words (>1 = looping) */
} mod_sample_t;

typedef struct {
    uint8_t  sample;
    uint16_t period;
    uint8_t  effect;
    uint8_t  param;
} mod_note_t;

typedef struct {
    char         title[21];
    mod_sample_t samples[MOD_NUM_SAMPLES];
    uint8_t      song_length;
    uint8_t      restart_pos;
    uint8_t      order[MOD_MAX_PATTERNS];
    int          num_patterns;
    uint8_t     *pattern_data;  /* raw pattern bytes */
    int8_t      *sample_data[MOD_NUM_SAMPLES]; /* pointers into CRAM1 */
} mod_file_t;

/* ======================================================================
 * Amiga period table (octaves 1-3, C-1 to B-3)
 * ====================================================================== */

static const uint16_t period_table[36] = {
    856,808,762,720,678,640,604,570,538,508,480,453,  /* Octave 1 */
    428,404,381,360,339,320,302,285,269,254,240,226,  /* Octave 2 */
    214,202,190,180,170,160,151,143,135,127,120,113   /* Octave 3 */
};

/* Convert Amiga period to playback rate in Hz.
 * Amiga base clock = 7093789.2 Hz (PAL), period is clock divider. */
#define AMIGA_CLOCK 7093789

/* Pre-computed 16.16 fixed-point rates for each period */
static uint32_t period_to_rate_fp16(uint16_t period) {
    if (period == 0) return 0;
    uint32_t hz = AMIGA_CLOCK / (period * 2);
    return OF_MIXER_RATE_FP16(hz);
}

/* Find note index (0-35) from period, or -1 */
static int period_to_note(uint16_t period) {
    for (int i = 0; i < 36; i++)
        if (period_table[i] == period) return i;
    /* Closest match */
    for (int i = 0; i < 35; i++)
        if (period <= period_table[i] && period >= period_table[i + 1])
            return i;
    return -1;
}

/* ======================================================================
 * Per-channel state
 * ====================================================================== */

typedef struct {
    int      voice;          /* hardware voice index, or -1 */
    int      sample_idx;     /* current sample (1-31, 0=none) */
    uint16_t period;         /* current period */
    uint32_t rate_fp16;      /* current rate as 16.16 FP */
    int      volume;         /* current volume 0-64 */
    int      pan;            /* pan 0-255 (default: LRRL) */
    /* Effect state */
    uint16_t target_period;  /* portamento to note target */
    int      porta_speed;
    int      vol_slide;
    int      arp_note;       /* base note index */
    uint8_t  arp_x, arp_y;
} channel_t;

static channel_t channels[MOD_NUM_CHANNELS];
static mod_file_t mod;

/* Default panning: LRRL (Amiga stereo) */
static const int default_pan[4] = { 64, 192, 192, 64 };

/* ======================================================================
 * MOD file parsing
 * ====================================================================== */

static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

static int parse_mod(const uint8_t *data, uint32_t len) {
    if (len < 1084) return -1;

    /* Check for M.K. signature at offset 1080 */
    if (memcmp(data + 1080, "M.K.", 4) != 0 &&
        memcmp(data + 1080, "4CHN", 4) != 0 &&
        memcmp(data + 1080, "FLT4", 4) != 0 &&
        memcmp(data + 1080, "M!K!", 4) != 0) {
        printf("Not a 4-channel MOD file\n");
        return -1;
    }

    /* Title */
    memcpy(mod.title, data, 20);
    mod.title[20] = '\0';
    printf("Title: %s\n", mod.title);

    /* Sample descriptors */
    const uint8_t *p = data + 20;
    for (int i = 0; i < MOD_NUM_SAMPLES; i++) {
        memcpy(mod.samples[i].name, p, 22);
        mod.samples[i].name[22] = '\0';
        mod.samples[i].length      = read_be16(p + 22) * 2;
        mod.samples[i].finetune    = p[24] & 0x0F;
        if (mod.samples[i].finetune > 7) mod.samples[i].finetune -= 16;
        mod.samples[i].volume      = p[25];
        mod.samples[i].loop_start  = read_be16(p + 26) * 2;
        mod.samples[i].loop_length = read_be16(p + 28) * 2;
        p += 30;
    }

    /* Song info */
    mod.song_length  = data[950];
    mod.restart_pos  = data[951];
    memcpy(mod.order, data + 952, 128);

    /* Count patterns (highest pattern number in order table + 1) */
    mod.num_patterns = 0;
    for (int i = 0; i < mod.song_length; i++)
        if (mod.order[i] >= mod.num_patterns)
            mod.num_patterns = mod.order[i] + 1;

    printf("Patterns: %d, Length: %d\n", mod.num_patterns, mod.song_length);

    /* Pattern data starts at offset 1084 */
    mod.pattern_data = (uint8_t *)(data + 1084);

    /* Sample data follows patterns */
    uint32_t sample_offset = 1084 + mod.num_patterns * MOD_ROWS_PER_PAT * MOD_NUM_CHANNELS * 4;

    /* Upload samples to CRAM1, converting 8-bit signed → 16-bit signed.
     * The hardware mixer expects 16-bit samples via of_mixer_play. */
    for (int i = 0; i < MOD_NUM_SAMPLES; i++) {
        uint32_t slen = mod.samples[i].length;
        if (slen == 0) {
            mod.sample_data[i] = NULL;
            continue;
        }
        if (sample_offset + slen > len) {
            mod.sample_data[i] = NULL;
            continue;
        }

        /* Allocate 2 bytes per sample (16-bit) */
        int16_t *cram = (int16_t *)of_mixer_alloc_samples(slen * 2);
        if (!cram) {
            printf("CRAM1 full at sample %d\n", i + 1);
            mod.sample_data[i] = NULL;
            continue;
        }
        /* Convert 8-bit signed → 16-bit signed */
        const int8_t *src = (const int8_t *)(data + sample_offset);
        for (uint32_t j = 0; j < slen; j++)
            cram[j] = (int16_t)src[j] << 8;

        mod.sample_data[i] = (int8_t *)cram;
        sample_offset += slen;
    }

    return 0;
}

/* ======================================================================
 * Pattern reading
 * ====================================================================== */

static mod_note_t read_note(int pattern, int row, int channel) {
    uint32_t offset = (pattern * MOD_ROWS_PER_PAT * MOD_NUM_CHANNELS + row * MOD_NUM_CHANNELS + channel) * 4;
    const uint8_t *p = mod.pattern_data + offset;
    mod_note_t n;
    n.sample = (p[0] & 0xF0) | (p[2] >> 4);
    n.period = ((p[0] & 0x0F) << 8) | p[1];
    n.effect = p[2] & 0x0F;
    n.param  = p[3];
    return n;
}

/* ======================================================================
 * Playback engine
 * ====================================================================== */

static int current_order = 0;
static int current_row = 0;
static int current_tick = 0;
static int ticks_per_row = 6;     /* default MOD speed */
static int bpm = 125;             /* default MOD tempo */
static int playing = 1;
static int pattern_break = 0;     /* set by effect 0xD to prevent double-break */

static void play_note(int ch, mod_note_t *n) {
    channel_t *c = &channels[ch];

    /* New sample? */
    if (n->sample > 0 && n->sample <= MOD_NUM_SAMPLES) {
        c->sample_idx = n->sample;
        c->volume = mod.samples[n->sample - 1].volume;
    }

    /* New note? */
    if (n->period > 0 && n->effect != 0x3) {  /* 0x3 = portamento to note, don't restart */
        c->period = n->period;
        c->rate_fp16 = period_to_rate_fp16(n->period);
        c->arp_note = period_to_note(n->period);

        int si = c->sample_idx - 1;
        if (si >= 0 && mod.sample_data[si]) {
            mod_sample_t *s = &mod.samples[si];

            /* Stop old voice */
            if (c->voice >= 0)
                of_mixer_stop(c->voice);

            /* Start new voice — 8-bit samples, FMT16=0 in CTRL.
             * of_mixer_play assumes 16-bit, so we use it for allocation
             * and then the hardware handles 8-bit via CTRL flag.
             * For now, play as-is — the hardware default is FMT16=1
             * but 8-bit support is in the new FPGA. */
            c->voice = of_mixer_play((const uint8_t *)mod.sample_data[si],
                                     s->length, AMIGA_CLOCK / (n->period * 2),
                                     0, c->volume * 4);

            if (c->voice >= 0) {
                /* Set up loop if sample has one */
                if (s->loop_length > 2)
                    of_mixer_set_loop(c->voice, s->loop_start,
                                      s->loop_start + s->loop_length);

                /* Apply channel pan */
                of_mixer_set_pan(c->voice, c->pan);
            }
        }
    } else if (n->period > 0 && n->effect == 0x3) {
        /* Portamento to note — set target, don't restart sample */
        c->target_period = n->period;
    }

    /* Parse effect */
    c->vol_slide = 0;
    c->porta_speed = (n->effect == 0x3) ? (n->param ? n->param : c->porta_speed) : 0;
    c->arp_x = 0;
    c->arp_y = 0;

    switch (n->effect) {
    case 0x0:  /* Arpeggio */
        if (n->param) {
            c->arp_x = n->param >> 4;
            c->arp_y = n->param & 0xF;
        }
        break;
    case 0xA:  /* Volume slide */
        c->vol_slide = (n->param >> 4) - (n->param & 0xF);
        break;
    case 0xC:  /* Set volume */
        c->volume = n->param;
        if (c->volume > 64) c->volume = 64;
        break;
    case 0xD:  /* Pattern break */
        if (!pattern_break) {
            int break_row = (n->param >> 4) * 10 + (n->param & 0xF);
            if (break_row >= MOD_ROWS_PER_PAT) break_row = 0;
            current_row = break_row - 1;  /* -1 because tick advance will +1 */
            current_order++;
            if (current_order >= mod.song_length)
                current_order = mod.restart_pos;
            pattern_break = 1;
        }
        break;
    case 0xF:  /* Set speed/tempo */
        if (n->param < 32)
            ticks_per_row = n->param ? n->param : 1;
        else
            bpm = n->param;
        break;
    }
}

static void tick_effects(int ch) {
    channel_t *c = &channels[ch];
    if (c->voice < 0) return;

    /* Arpeggio */
    if (c->arp_x || c->arp_y) {
        int offsets[3] = { 0, c->arp_x, c->arp_y };
        int note = c->arp_note + offsets[current_tick % 3];
        if (note >= 0 && note < 36)
            c->rate_fp16 = period_to_rate_fp16(period_table[note]);
    }

    /* Portamento to note */
    if (c->porta_speed && c->target_period) {
        if (c->period > c->target_period) {
            c->period -= c->porta_speed;
            if (c->period < c->target_period)
                c->period = c->target_period;
        } else if (c->period < c->target_period) {
            c->period += c->porta_speed;
            if (c->period > c->target_period)
                c->period = c->target_period;
        }
        c->rate_fp16 = period_to_rate_fp16(c->period);
    }

    /* Volume slide */
    if (c->vol_slide) {
        c->volume += c->vol_slide;
        if (c->volume < 0) c->volume = 0;
        if (c->volume > 64) c->volume = 64;
    }
}

static void update_hardware(void) {
    for (int ch = 0; ch < MOD_NUM_CHANNELS; ch++) {
        channel_t *c = &channels[ch];
        if (c->voice < 0) continue;

        /* Map MOD volume (0-64) + pan to L/R */
        int vol = c->volume * 4;  /* 0-255 */
        int vol_l = (vol * (255 - c->pan)) >> 8;
        int vol_r = (vol * c->pan) >> 8;

        of_mixer_set_voice_raw(c->voice, c->rate_fp16, vol_l, vol_r);
    }
}

/* ======================================================================
 * Timer callback — runs at tick rate
 * ====================================================================== */

static volatile int tick_pending = 0;

static void mod_timer_tick(void) {
    tick_pending = 1;
}

static void process_tick(void) {
    if (!playing) return;

    if (current_tick == 0) {
        /* Row tick: read new notes */
        pattern_break = 0;
        int pattern = mod.order[current_order];
        for (int ch = 0; ch < MOD_NUM_CHANNELS; ch++) {
            mod_note_t n = read_note(pattern, current_row, ch);
            play_note(ch, &n);
        }
    } else {
        /* Inter-row tick: process effects */
        for (int ch = 0; ch < MOD_NUM_CHANNELS; ch++)
            tick_effects(ch);
    }

    /* Write final values to hardware */
    update_hardware();

    /* Advance tick/row/order */
    current_tick++;
    if (current_tick >= ticks_per_row) {
        current_tick = 0;
        current_row++;
        if (current_row >= MOD_ROWS_PER_PAT) {
            current_row = 0;
            current_order++;
            if (current_order >= mod.song_length)
                current_order = mod.restart_pos;
        }
    }
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    /* Audio-only app — stay in terminal mode for status output */
    printf("=== MOD Player Demo ===\n\n");

    /* Initialize mixer */
    of_mixer_init(32, OF_MIXER_OUTPUT_RATE);

    /* Load MOD from data slot 3 */
    printf("Loading MOD from slot 3...\n");

    FILE *f = fopen("slot:3", "rb");
    if (!f) {
        printf("No MOD file in data slot 3.\n");
        printf("Add a .mod file to your core's data.json as slot 3.\n");
        for (;;) usleep(100000);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    printf("File size: %ld bytes\n", size);

    if (size <= 0) {
        printf("Slot 3 is empty (size=%ld).\n", size);
        printf("Ensure your instance JSON maps slot 3 to a .mod file\n");
        printf("and the file is in Assets/openfpgaos/common/\n");
        fclose(f);
        for (;;) usleep(100000);
    }
    if (size < 1084) {
        printf("File too small for MOD (%ld bytes)\n", size);
        fclose(f);
        for (;;) usleep(100000);
    }

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        printf("Out of memory\n");
        fclose(f);
        for (;;) usleep(100000);
    }

    size_t got = fread(data, 1, size, f);
    fclose(f);
    printf("Read %u of %ld bytes\n", (unsigned)got, size);

    if ((long)got < size) {
        printf("Short read — file may be corrupt\n");
    }

    /* Debug: print signature bytes at offset 1080 */
    if (got >= 1084) {
        printf("Signature: %c%c%c%c (0x%02x%02x%02x%02x)\n",
               data[1080], data[1081], data[1082], data[1083],
               data[1080], data[1081], data[1082], data[1083]);
    }

    if (parse_mod(data, (uint32_t)got) < 0) {
        printf("Failed to parse MOD\n");
        for (;;) usleep(100000);
    }

    /* NOTE: can't free(data) — mod.pattern_data points into it.
     * Sample data was copied to CRAM1, but pattern data is read
     * directly from the file buffer during playback. */

    /* Initialize channels */
    for (int i = 0; i < MOD_NUM_CHANNELS; i++) {
        channels[i].voice = -1;
        channels[i].pan = default_pan[i];
        channels[i].volume = 0;
    }

    /* Start tick timer: MOD tick rate = BPM * 2 / 5 */
    int tick_hz = (bpm * 2) / 5;
    printf("Tick rate: %d Hz (BPM=%d, speed=%d)\n", tick_hz, bpm, ticks_per_row);
    of_timer_set_callback(mod_timer_tick, tick_hz);

    printf("\nPlaying... A=pause B=next pattern\n");
    printf("Waiting for first tick...\n");

    /* Display loop */
    int display_counter = 0;
    int first_tick = 1;
    for (;;) {
        /* Process pending tick from timer ISR */
        if (tick_pending) {
            tick_pending = 0;
            if (first_tick) {
                printf("Timer OK, processing first tick\n");
                first_tick = 0;
            }
            process_tick();

            /* Update timer rate if BPM changed */
            int new_hz = (bpm * 2) / 5;
            if (new_hz != tick_hz) {
                tick_hz = new_hz;
                of_timer_set_callback(mod_timer_tick, tick_hz);
            }
        }

        /* Poll input and update display at ~30 Hz (not every tick) */
        display_counter++;
        if (display_counter >= 2) {
            display_counter = 0;

            of_input_poll();

            if (of_btn_pressed(OF_BTN_A)) {
                playing = !playing;
                if (!playing) of_mixer_stop_all();
            }
            if (of_btn_pressed(OF_BTN_B)) {
                current_row = 0;
                current_tick = 0;
                current_order++;
                if (current_order >= mod.song_length)
                    current_order = 0;
            }

            printf("\rOrd:%02d Pat:%02d Row:%02d Spd:%d BPM:%d ",
                   current_order, mod.order[current_order],
                   current_row, ticks_per_row, bpm);
        }

        /* Sleep until next tick (~20ms at 50Hz default) */
        usleep(5000);
    }
}
