/*
 * openfpgaOS MIDI Demo Application
 * Plays a MIDI file (loaded from data slot 3) through the OPL3 (YMF262) FM synthesizer.
 * Terminal displays playback status with note activity.
 *
 * Controls:
 *   START   = play/pause
 *   SELECT  = restart
 */

#include "of.h"
#include <stdio.h>

/* MIDI file is in data slot 3 */

/* Maximum MIDI file size we support */
#define MIDI_MAX_SIZE   (256 * 1024)

/* Buffer for the loaded MIDI file (aligned for DMA) */
static uint8_t midi_buf[MIDI_MAX_SIZE] __attribute__((aligned(512)));

/* ======================================================================
 * OPL3 (YMF262) instrument presets
 *
 * OPL3 has 9 channels, each with 2 operators (modulator + carrier).
 * Register layout per instrument:
 *   [0]  = feedback/connection (reg 0xC0+ch): FB[3:1], CNT[0]
 *   Modulator (op 0):
 *   [1]  = AM/VIB/EGT/KSR/MULT  (reg 0x20+op_off)
 *   [2]  = KSL/TL               (reg 0x40+op_off)
 *   [3]  = AR/DR                 (reg 0x60+op_off)
 *   [4]  = SL/RR                 (reg 0x80+op_off)
 *   [5]  = WS                    (reg 0xE0+op_off)
 *   Carrier (op 1):
 *   [6]  = AM/VIB/EGT/KSR/MULT  (reg 0x20+op_off)
 *   [7]  = KSL/TL               (reg 0x40+op_off)
 *   [8]  = AR/DR                 (reg 0x60+op_off)
 *   [9]  = SL/RR                 (reg 0x80+op_off)
 *   [10] = WS                    (reg 0xE0+op_off)
 * Total: 11 bytes per instrument
 * ====================================================================== */

#define INST_SIZE 11

/* OPL3 operator offset table: channels 0-8 map to register offsets.
 * Modulator offsets: 0,1,2,8,9,10,16,17,18
 * Carrier offsets:   3,4,5,11,12,13,19,20,21 */
static const uint8_t opl_mod_off[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static const uint8_t opl_car_off[9] = { 3, 4, 5, 11, 12, 13, 19, 20, 21 };

/* Piano: bright attack, moderate sustain */
static const uint8_t inst_piano[INST_SIZE] = {
    0x31,  /* FB=6, CNT=1 (additive) */
    /* Mod */ 0x01, 0x10, 0xF2, 0x72, 0x00,
    /* Car */ 0x01, 0x00, 0xF2, 0x72, 0x00,
};

/* Electric piano: bell-like, FM */
static const uint8_t inst_epiano[INST_SIZE] = {
    0x06,  /* FB=0, CNT=0 (FM) */
    /* Mod */ 0x02, 0x28, 0xF5, 0x35, 0x00,
    /* Car */ 0x01, 0x00, 0xF2, 0x72, 0x00,
};

/* Organ: sustained, additive harmonics */
static const uint8_t inst_organ[INST_SIZE] = {
    0x01,  /* FB=0, CNT=1 (additive) */
    /* Mod */ 0x21, 0x12, 0xF0, 0x10, 0x00,
    /* Car */ 0x21, 0x00, 0xF0, 0x10, 0x00,
};

/* Strings: slow attack, sustained */
static const uint8_t inst_strings[INST_SIZE] = {
    0x00,  /* FB=0, CNT=0 (FM) */
    /* Mod */ 0x21, 0x1A, 0x82, 0x42, 0x00,
    /* Car */ 0x21, 0x00, 0x72, 0x32, 0x00,
};

/* Brass: punchy attack */
static const uint8_t inst_brass[INST_SIZE] = {
    0x04,  /* FB=0, CNT=0 (FM) */
    /* Mod */ 0x21, 0x18, 0xF5, 0x55, 0x00,
    /* Car */ 0x21, 0x00, 0xF2, 0x52, 0x00,
};

/* Synth lead */
static const uint8_t inst_synth[INST_SIZE] = {
    0x02,  /* FB=0, CNT=0 (FM) */
    /* Mod */ 0x22, 0x1E, 0xF3, 0x45, 0x01,
    /* Car */ 0x21, 0x00, 0xF2, 0x44, 0x00,
};

/* Bass: low, punchy */
static const uint8_t inst_bass[INST_SIZE] = {
    0x00,  /* FB=0, CNT=0 (FM) */
    /* Mod */ 0x01, 0x18, 0xF5, 0x75, 0x00,
    /* Car */ 0x01, 0x00, 0xF2, 0x73, 0x00,
};

/* Drum: noise-like, fast decay */
static const uint8_t inst_drum[INST_SIZE] = {
    0x00,  /* FB=0, CNT=0 (FM) */
    /* Mod */ 0x05, 0x00, 0xFF, 0x8F, 0x00,
    /* Car */ 0x01, 0x00, 0xFF, 0x8F, 0x00,
};

static const uint8_t *gm_to_opl(int program) {
    int group = program >> 3;
    switch (group) {
    case 0:  return inst_piano;
    case 1:  return inst_epiano;
    case 2:  return inst_organ;
    case 3:  return inst_bass;
    case 4:  return inst_bass;
    case 5:  return inst_strings;
    case 6:  return inst_strings;
    case 7:  return inst_brass;
    case 8:  return inst_synth;
    case 9:  return inst_synth;
    case 10: return inst_synth;
    case 11: return inst_synth;
    default: return inst_organ;
    }
}

/* ======================================================================
 * MIDI note -> OPL3 F-Number/Block conversion
 *
 * OPL3 frequency registers:
 *   0xA0+ch: F-Number low 8 bits
 *   0xB0+ch: KEY-ON[5], Block[4:2], F-Number high 2 bits[1:0]
 *
 * F-Number = freq * 2^(20-block) / (clock/72)
 * We precompute F-Numbers for one octave and shift via block.
 * ====================================================================== */

/* F-Numbers for C4..B4 at block 4, assuming 3.571 MHz clock:
 * sample_rate = 3571429 / 72 = 49603 Hz
 * F = note_freq * 2^(20-block) / (3571429/72)
 * At block 4: F = note_freq * 2^16 / 49603 */
static const uint16_t fnums[12] = {
    345, 365, 387, 410, 435, 460,  /* C, C#, D, D#, E, F */
    488, 517, 548, 580, 615, 651   /* F#, G, G#, A, A#, B */
};

static void midi_note_to_opl(int note, uint8_t *flo, uint8_t *fhi_blk) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    int oct = note / 12;
    int semi = note % 12;

    /* Map MIDI octave to OPL3 block (0-7) */
    int block = oct - 1;
    if (block < 0) block = 0;
    if (block > 7) block = 7;

    uint16_t fnum = fnums[semi];

    *flo = fnum & 0xFF;
    *fhi_blk = (uint8_t)((block << 2) | ((fnum >> 8) & 0x03));
}

/* ======================================================================
 * OPL3 channel management
 * ====================================================================== */

#define OPL_CHANNELS 9

static int opl_ch_note[OPL_CHANNELS];
static int opl_ch_midi[OPL_CHANNELS];
static const uint8_t *opl_ch_inst[OPL_CHANNELS];
static uint32_t opl_ch_age[OPL_CHANNELS];
static uint32_t opl_age_counter;
static int midi_program[16];

static void load_instrument(int ch, const uint8_t *p) {
    if (opl_ch_inst[ch] == p)
        return;

    uint8_t mod = opl_mod_off[ch];
    uint8_t car = opl_car_off[ch];

    /* Feedback/Connection */
    of_audio_opl_write(0xC0 + ch, p[0]);

    /* Modulator registers */
    of_audio_opl_write(0x20 + mod, p[1]);
    of_audio_opl_write(0x40 + mod, p[2]);
    of_audio_opl_write(0x60 + mod, p[3]);
    of_audio_opl_write(0x80 + mod, p[4]);
    of_audio_opl_write(0xE0 + mod, p[5]);

    /* Carrier registers */
    of_audio_opl_write(0x20 + car, p[6]);
    of_audio_opl_write(0x40 + car, p[7]);
    of_audio_opl_write(0x60 + car, p[8]);
    of_audio_opl_write(0x80 + car, p[9]);
    of_audio_opl_write(0xE0 + car, p[10]);

    opl_ch_inst[ch] = p;
}

static int alloc_channel(int midi_ch) {
    const uint8_t *inst = (midi_ch == 9) ? inst_drum :
                          gm_to_opl(midi_program[midi_ch]);

    /* Reuse channel already assigned to this MIDI channel */
    for (int i = 0; i < OPL_CHANNELS; i++) {
        if (opl_ch_midi[i] == midi_ch && opl_ch_note[i] < 0)
            return i;
    }
    /* Find a free channel, prefer one with same instrument */
    int best = -1;
    for (int i = 0; i < OPL_CHANNELS; i++) {
        if (opl_ch_note[i] < 0) {
            if (opl_ch_inst[i] == inst) {
                opl_ch_midi[i] = midi_ch;
                return i;
            }
            if (best < 0)
                best = i;
        }
    }
    if (best >= 0) {
        opl_ch_midi[best] = midi_ch;
        load_instrument(best, inst);
        return best;
    }
    /* Steal least-recently-released channel */
    int oldest = 0;
    uint32_t oldest_age = opl_ch_age[0];
    for (int i = 1; i < OPL_CHANNELS; i++) {
        if (opl_ch_age[i] < oldest_age) {
            oldest_age = opl_ch_age[i];
            oldest = i;
        }
    }
    /* KEY-OFF stolen channel */
    of_audio_opl_write(0xB0 + oldest, 0x00);
    opl_ch_note[oldest] = -1;
    opl_ch_midi[oldest] = midi_ch;
    load_instrument(oldest, inst);
    return oldest;
}

static void opl_note_on(int midi_ch, int note, int vel) {
    (void)vel;
    int ch = alloc_channel(midi_ch);
    opl_ch_note[ch] = note;

    uint8_t flo, fhi_blk;
    midi_note_to_opl(note, &flo, &fhi_blk);

    /* KEY-OFF first to reset envelope if channel was releasing */
    of_audio_opl_write(0xB0 + ch, fhi_blk);       /* freq hi + block, KEY-ON=0 */
    of_audio_opl_write(0xA0 + ch, flo);            /* freq low */
    of_audio_opl_write(0xB0 + ch, 0x20 | fhi_blk); /* KEY-ON */
}

static void opl_note_off(int midi_ch, int note) {
    for (int i = 0; i < OPL_CHANNELS; i++) {
        if (opl_ch_midi[i] == midi_ch && opl_ch_note[i] == note) {
            /* KEY-OFF: clear bit 5 of 0xB0+ch, keep freq/block */
            uint8_t flo, fhi_blk;
            midi_note_to_opl(note, &flo, &fhi_blk);
            of_audio_opl_write(0xB0 + i, fhi_blk);  /* KEY-ON=0 */
            opl_ch_note[i] = -1;
            opl_ch_age[i] = opl_age_counter++;
            return;
        }
    }
}

static void all_notes_off(void) {
    for (int i = 0; i < OPL_CHANNELS; i++) {
        of_audio_opl_write(0xB0 + i, 0x00);  /* KEY-OFF */
        opl_ch_note[i] = -1;
        opl_ch_midi[i] = -1;
    }
}

/* ======================================================================
 * MIDI parser
 * ====================================================================== */

static const uint8_t *midi_data;
static uint32_t midi_len;
static uint32_t midi_pos;
static uint32_t midi_track_start;
static uint32_t midi_track_len;
static uint32_t midi_us_per_tick;
static uint8_t  midi_running;
static int      midi_playing;
static int      midi_done;

static uint32_t read_var_len(void) {
    uint32_t val = 0;
    while (midi_pos < midi_len) {
        uint8_t b = midi_data[midi_pos++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static uint32_t read_u16(uint32_t offset) {
    return ((uint32_t)midi_data[offset] << 8) | midi_data[offset + 1];
}

static uint32_t read_u32(uint32_t offset) {
    return ((uint32_t)midi_data[offset] << 24) |
           ((uint32_t)midi_data[offset + 1] << 16) |
           ((uint32_t)midi_data[offset + 2] << 8) |
           midi_data[offset + 3];
}

static void midi_init(void) {
    midi_data = midi_buf;

    uint16_t division = read_u16(12);
    midi_track_start = 22;
    midi_track_len = read_u32(18);

    uint32_t us_per_beat = 500000;
    midi_us_per_tick = us_per_beat / division;

    midi_pos = midi_track_start;
    midi_running = 0;
    midi_playing = 1;
    midi_done = 0;

    for (int i = 0; i < 16; i++)
        midi_program[i] = 0;
    opl_age_counter = 0;
    for (int i = 0; i < OPL_CHANNELS; i++) {
        opl_ch_note[i] = -1;
        opl_ch_midi[i] = -1;
        opl_ch_inst[i] = 0;
        opl_ch_age[i] = 0;
    }
}

static uint32_t midi_next_event(void) {
    if (midi_pos >= midi_track_start + midi_track_len) {
        midi_done = 1;
        return 0;
    }

    uint32_t delta = read_var_len();

    uint8_t status = midi_data[midi_pos];
    if (status < 0x80) {
        status = midi_running;
    } else {
        midi_pos++;
        if (status < 0xF0)
            midi_running = status;
    }

    uint8_t cmd = status & 0xF0;
    uint8_t ch = status & 0x0F;

    if (status == 0xFF) {
        uint8_t meta_type = midi_data[midi_pos++];
        uint32_t mlen = read_var_len();

        if (meta_type == 0x51 && mlen == 3) {
            uint32_t us_per_beat =
                ((uint32_t)midi_data[midi_pos] << 16) |
                ((uint32_t)midi_data[midi_pos + 1] << 8) |
                midi_data[midi_pos + 2];
            uint16_t division = read_u16(12);
            midi_us_per_tick = us_per_beat / division;
        } else if (meta_type == 0x2F) {
            midi_done = 1;
        }
        midi_pos += mlen;
    } else if (status >= 0xF0 && status <= 0xF7) {
        uint32_t slen = read_var_len();
        midi_pos += slen;
    } else if (cmd == 0x90) {
        uint8_t note = midi_data[midi_pos++];
        uint8_t vel = midi_data[midi_pos++];
        if (vel > 0)
            opl_note_on(ch, note, vel);
        else
            opl_note_off(ch, note);
    } else if (cmd == 0x80) {
        uint8_t note = midi_data[midi_pos++];
        midi_pos++;
        opl_note_off(ch, note);
    } else if (cmd == 0xC0) {
        midi_program[ch] = midi_data[midi_pos++];
    } else if (cmd == 0xB0 || cmd == 0xE0) {
        midi_pos += 2;
    } else if (cmd == 0xD0) {
        midi_pos += 1;
    } else {
        midi_pos += 2;
    }

    return delta;
}

/* ======================================================================
 * Terminal UI
 * ====================================================================== */

static const char *note_letters[] = {
    "C-", "C#", "D-", "D#", "E-", "F-",
    "F#", "G-", "G#", "A-", "A#", "B-"
};

static int ui_drawn = 0;

static void draw_static_ui(void) {
    printf("\033[2J\033[H");  /* clear screen, cursor home */
    printf("    openfpgaOS MIDI Player\n");
    printf("    ======================\n\n");
    printf(" OPL3 Channels:\n");
    for (int i = 0; i < OPL_CHANNELS; i++)
        printf(" %d: --- ..  \n", i);
    printf("\n [PLAYING] \n");
    printf("\n START=play/pause SEL=restart\n");
    ui_drawn = 1;
}

static void draw_full_ui(int paused) {
    if (!ui_drawn)
        draw_static_ui();

    /* Update only the channel note fields (col 4, rows 4-12) */
    for (int i = 0; i < OPL_CHANNELS; i++) {
        printf("\033[%d;5H", 5 + i);  /* row 5+i, col 5 (1-based) */
        if (opl_ch_note[i] >= 0) {
            int n = opl_ch_note[i];
            int oct = n / 12 - 1;
            int semi = n % 12;
            printf("%s%d ##", note_letters[semi], oct);
        } else {
            printf("--- ..");
        }
    }

    /* Update status line (row 14) */
    printf("\033[15;2H");  /* row 15, col 2 (1-based) */
    if (midi_done)
        printf("[FINISHED]");
    else if (paused)
        printf("[PAUSED]  ");
    else
        printf("[PLAYING] ");
}

/* ======================================================================
 * MIDI file loading
 * ====================================================================== */

static int load_midi_file(void) {
    FILE *f = fopen("music.mid", "rb");
    if (!f)
        return -1;

    size_t n = fread(midi_buf, 1, MIDI_MAX_SIZE, f);
    fclose(f);

    if (n < 14)
        return -2;

    if (midi_buf[0] != 'M' || midi_buf[1] != 'T' ||
        midi_buf[2] != 'h' || midi_buf[3] != 'd')
        return -10;

    midi_len = (uint32_t)n;
    return 0;
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(void) {
    of_file_slot_register(3, "music.mid");
    of_audio_opl_reset();

    /* Enable waveform select (OPL3 register 0x01 bit 5) */
    of_audio_opl_write(0x01, 0x20);

    printf("\033[2J\033[H");  /* clear screen, cursor home */
    printf("    openfpgaOS MIDI Player\n");
    printf("    ======================\n\n");
    printf(" Loading MIDI file...\n");

    int rc = load_midi_file();
    if (rc < 0) {
        printf(" Error loading MIDI! rc=%d\n", rc);
        while (1) {}
    }

    midi_init();

    midi_playing = 1;
    int paused = 0;
    int prev_notes[OPL_CHANNELS];
    uint32_t pending_delay = 0;

    for (int i = 0; i < OPL_CHANNELS; i++)
        prev_notes[i] = -1;

    draw_static_ui();

    while (1) {
        /* Input handling */
        of_input_poll();
        of_input_state_t state;
        of_input_state(0, &state);

        if (state.buttons_pressed & OF_BTN_START) {
            if (midi_done) {
                all_notes_off();
                of_audio_opl_reset();
                of_audio_opl_write(0x01, 0x20);
                midi_init();
                paused = 0;
                midi_playing = 1;
                ui_drawn = 0;
                pending_delay = 0;
                draw_static_ui();
            } else {
                paused = !paused;
                midi_playing = !paused;
            }
        }
        if (state.buttons_pressed & OF_BTN_SELECT) {
            all_notes_off();
            of_audio_opl_reset();
            of_audio_opl_write(0x01, 0x20);
            midi_init();
            paused = 0;
            midi_playing = 1;
            ui_drawn = 0;
            pending_delay = 0;
            draw_static_ui();
        }

        /* UI update — only changed channel fields */
        int need_redraw = 0;
        for (int i = 0; i < OPL_CHANNELS; i++) {
            if (opl_ch_note[i] != prev_notes[i]) {
                need_redraw = 1;
                prev_notes[i] = opl_ch_note[i];
            }
        }
        if (need_redraw)
            draw_full_ui(paused);

        /* MIDI event processing */
        if (midi_playing && !midi_done) {
            /* Wait for pending delay, then process next event */
            if (pending_delay > 0) {
                of_delay_us(pending_delay);
                pending_delay = 0;
            }
            /* Process all simultaneous events (delta=0) */
            while (!midi_done) {
                uint32_t delta = midi_next_event();
                if (delta > 0) {
                    pending_delay = delta * midi_us_per_tick;
                    break;
                }
            }
        } else if (paused || midi_done) {
            of_delay_ms(16);
        }
    }

    return 0;
}
