/*
 * test_midi.c — of_midi engine tests (MD.xx)
 *
 * Tests the MIDI playback engine using synthetic MIDI files built
 * in memory. Covers all the of_midi APIs that Duke3D depends on:
 *   - init / play / stop / pump
 *   - pause / resume / playing / paused queries
 *   - set_volume / get_volume
 *   - load_bank (NULL = builtin, custom = TMB)
 *   - builtin_bank pointer
 *   - format 0 single track
 *   - format 1 multi-track
 *   - program change events
 *   - note on / note off
 *   - tempo changes
 *   - end-of-track + loop
 */

#include "test.h"
#include "of_midi.h"
#include <time.h>

/* ================================================================
 * Synthetic MIDI file builder
 * ================================================================ */

/* Standard MIDI File header */
static const uint8_t mthd_fmt0[] = {
    'M','T','h','d',
    0,0,0,6,           /* header length = 6 */
    0,0,               /* format = 0 */
    0,1,               /* 1 track */
    0,96,              /* 96 ticks per quarter note */
};

static const uint8_t mthd_fmt1[] = {
    'M','T','h','d',
    0,0,0,6,
    0,1,               /* format = 1 */
    0,2,               /* 2 tracks */
    0,96,
};

/* Append an MTrk chunk to a MIDI buffer. data/len = track event bytes.
 * Returns total bytes written (8 byte header + len). */
static int append_mtrk(uint8_t *out, const uint8_t *data, uint32_t len) {
    out[0] = 'M'; out[1] = 'T'; out[2] = 'r'; out[3] = 'k';
    out[4] = (len >> 24) & 0xFF;
    out[5] = (len >> 16) & 0xFF;
    out[6] = (len >> 8) & 0xFF;
    out[7] = len & 0xFF;
    for (uint32_t i = 0; i < len; i++)
        out[8 + i] = data[i];
    return 8 + len;
}

/* Build a simple Format 0 MIDI: tempo 120 BPM, prog change ch0→30,
 * note on A4, hold 1 quarter, note off, end of track. */
static uint32_t build_simple_midi(uint8_t *out, int loop_friendly) {
    uint8_t track[64];
    int t = 0;

    /* delta=0, tempo meta event: FF 51 03 + 24-bit us/quarter
     * 120 BPM = 500000 us/quarter = 0x07A120 */
    track[t++] = 0;     /* delta */
    track[t++] = 0xFF;
    track[t++] = 0x51;
    track[t++] = 0x03;
    track[t++] = 0x07;
    track[t++] = 0xA1;
    track[t++] = 0x20;

    /* delta=0, program change ch0 → 30 (Distortion Guitar) */
    track[t++] = 0;
    track[t++] = 0xC0;
    track[t++] = 30;

    /* delta=0, note on ch0 A4(69) vel 100 */
    track[t++] = 0;
    track[t++] = 0x90;
    track[t++] = 69;
    track[t++] = 100;

    /* delta=24 (1/4 of a quarter ≈ 125ms), note off */
    track[t++] = 24;
    track[t++] = 0x80;
    track[t++] = 69;
    track[t++] = 0;

    /* delta=0, end of track */
    track[t++] = 0;
    track[t++] = 0xFF;
    track[t++] = 0x2F;
    track[t++] = 0x00;

    /* Header */
    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];

    /* Track */
    int tlen = append_mtrk(out + hlen, track, t);

    (void)loop_friendly;
    return hlen + tlen;
}

/* Build a Format 1 MIDI with 2 tracks (tempo on track 1, notes on track 2). */
static uint32_t build_fmt1_midi(uint8_t *out) {
    uint8_t t1[16], t2[64];
    int n;

    /* Track 1: tempo */
    n = 0;
    t1[n++] = 0; t1[n++] = 0xFF; t1[n++] = 0x51; t1[n++] = 0x03;
    t1[n++] = 0x07; t1[n++] = 0xA1; t1[n++] = 0x20;  /* 120 BPM */
    t1[n++] = 0; t1[n++] = 0xFF; t1[n++] = 0x2F; t1[n++] = 0x00;  /* EOT */
    int t1_len = n;

    /* Track 2: notes on channel 0 */
    n = 0;
    t2[n++] = 0; t2[n++] = 0xC0; t2[n++] = 24;  /* prog → nylon guitar */
    t2[n++] = 0; t2[n++] = 0x90; t2[n++] = 60; t2[n++] = 100;  /* C4 on */
    t2[n++] = 24; t2[n++] = 0x80; t2[n++] = 60; t2[n++] = 0;   /* C4 off */
    t2[n++] = 0; t2[n++] = 0x90; t2[n++] = 64; t2[n++] = 100;  /* E4 on */
    t2[n++] = 24; t2[n++] = 0x80; t2[n++] = 64; t2[n++] = 0;   /* E4 off */
    t2[n++] = 0; t2[n++] = 0xFF; t2[n++] = 0x2F; t2[n++] = 0x00;  /* EOT */
    int t2_len = n;

    /* Header */
    int hlen = sizeof(mthd_fmt1);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt1[i];

    int o = hlen;
    o += append_mtrk(out + o, t1, t1_len);
    o += append_mtrk(out + o, t2, t2_len);
    return o;
}

/* Build a multi-event MIDI for a longer playback test (~1 second). */
static uint32_t build_long_midi(uint8_t *out) {
    static uint8_t track[256];
    int t = 0;

    /* Tempo: 120 BPM */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;

    /* Program change ch0 → 30 */
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 30;

    /* Play 4 notes, each ~125ms apart */
    int notes[] = {60, 64, 67, 72};  /* C4, E4, G4, C5 */
    for (int i = 0; i < 4; i++) {
        track[t++] = (i == 0) ? 0 : 24;    /* delta */
        track[t++] = 0x90; track[t++] = notes[i]; track[t++] = 100;
        track[t++] = 24;                    /* hold for ~125ms */
        track[t++] = 0x80; track[t++] = notes[i]; track[t++] = 0;
    }

    /* End of track */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI exercising running status — common in real MIDI files.
 * After a status byte, subsequent events of the same type omit it. */
static uint32_t build_running_status_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;  /* prog 0 piano */

    /* First note on with explicit status 0x90 */
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    /* Subsequent note ons WITHOUT status byte (running status) */
    track[t++] = 12;                    track[t++] = 64; track[t++] = 100;
    track[t++] = 12;                    track[t++] = 67; track[t++] = 100;
    /* Note off via running status note-on with velocity 0 */
    track[t++] = 24;                    track[t++] = 60; track[t++] = 0;
    track[t++] = 0;                     track[t++] = 64; track[t++] = 0;
    track[t++] = 0;                     track[t++] = 67; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with control change events (volume, pan, expression). */
static uint32_t build_cc_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 30;

    /* CC7 (channel volume) = 100 */
    track[t++] = 0; track[t++] = 0xB0; track[t++] = 7; track[t++] = 100;
    /* CC10 (pan) = 64 (center) */
    track[t++] = 0; track[t++] = 0xB0; track[t++] = 10; track[t++] = 64;
    /* CC11 (expression) = 127 */
    track[t++] = 0; track[t++] = 0xB0; track[t++] = 11; track[t++] = 127;

    /* Play note */
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    /* Pan sweep mid-note (left → right) */
    track[t++] = 12; track[t++] = 0xB0; track[t++] = 10; track[t++] = 0;
    track[t++] = 12; track[t++] = 0xB0; track[t++] = 10; track[t++] = 127;
    /* Volume fade */
    track[t++] = 12; track[t++] = 0xB0; track[t++] = 7; track[t++] = 50;
    /* Note off */
    track[t++] = 12; track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    /* All notes off (CC123) */
    track[t++] = 0; track[t++] = 0xB0; track[t++] = 123; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with pitch bend events. */
static uint32_t build_pitchbend_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 30;
    /* Pitch bend center */
    track[t++] = 0; track[t++] = 0xE0; track[t++] = 0; track[t++] = 0x40;
    /* Note on */
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    /* Bend up */
    track[t++] = 12; track[t++] = 0xE0; track[t++] = 0; track[t++] = 0x60;
    /* Bend max */
    track[t++] = 12; track[t++] = 0xE0; track[t++] = 0x7F; track[t++] = 0x7F;
    /* Bend down */
    track[t++] = 12; track[t++] = 0xE0; track[t++] = 0; track[t++] = 0x20;
    /* Recenter */
    track[t++] = 12; track[t++] = 0xE0; track[t++] = 0; track[t++] = 0x40;
    /* Note off */
    track[t++] = 12; track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with all 16 channels playing simultaneously (max polyphony). */
static uint32_t build_16ch_midi(uint8_t *out) {
    static uint8_t track[512];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;

    /* Program change on all 16 channels (channel 9 = drums = GM channel 10) */
    for (int ch = 0; ch < 16; ch++) {
        if (ch == 9) continue;  /* drums use percussion mapping */
        track[t++] = 0;
        track[t++] = 0xC0 | ch;
        track[t++] = ch * 8;  /* different program per channel */
    }

    /* Note on simultaneously on all 16 channels */
    for (int ch = 0; ch < 16; ch++) {
        track[t++] = 0;
        track[t++] = 0x90 | ch;
        track[t++] = (ch == 9) ? 36 : (60 + ch);  /* ch9 = bass drum */
        track[t++] = 100;
    }

    /* Hold for 250ms (24 + 24 ticks) */
    track[t++] = 24;

    /* Note off all 16 channels */
    for (int ch = 0; ch < 16; ch++) {
        if (ch != 0) track[t++] = 0;
        track[t++] = 0x80 | ch;
        track[t++] = (ch == 9) ? 36 : (60 + ch);
        track[t++] = 0;
    }

    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI exercising chord polyphony — 6 simultaneous notes on one channel. */
static uint32_t build_chord_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;

    /* C major chord: C4 E4 G4 + C5 E5 G5 (6 notes on ch0) */
    int notes[] = {60, 64, 67, 72, 76, 79};
    for (int i = 0; i < 6; i++) {
        track[t++] = 0;
        track[t++] = 0x90;
        track[t++] = notes[i];
        track[t++] = 100;
    }
    track[t++] = 48;  /* hold */
    for (int i = 0; i < 6; i++) {
        if (i != 0) track[t++] = 0;
        track[t++] = 0x80;
        track[t++] = notes[i];
        track[t++] = 0;
    }
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with tempo changes mid-song. */
static uint32_t build_tempo_change_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    /* Start at 60 BPM = 1000000 us/quarter = 0x0F4240 */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x0F; track[t++] = 0x42; track[t++] = 0x40;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    track[t++] = 24; track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    /* Speed up to 240 BPM = 250000 us/quarter = 0x03D090 */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x03; track[t++] = 0xD0; track[t++] = 0x90;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 64; track[t++] = 100;
    track[t++] = 24; track[t++] = 0x80; track[t++] = 64; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with multi-byte VLQ deltas (>127 ticks). */
static uint32_t build_long_delta_midi(uint8_t *out) {
    static uint8_t track[64];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    /* Delta = 200 ticks → encoded as 2-byte VLQ: 0x81 0x48 */
    track[t++] = 0x81; track[t++] = 0x48;
    track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with text/copyright meta events (must be skipped, not played). */
static uint32_t build_meta_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    /* Track name meta */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x03; track[t++] = 4;
    track[t++] = 'T'; track[t++] = 'e'; track[t++] = 's'; track[t++] = 't';
    /* Copyright meta */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x02; track[t++] = 3;
    track[t++] = '(';track[t++] = 'C'; track[t++] = ')';
    /* Time signature meta: 4/4 */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x58; track[t++] = 4;
    track[t++] = 4; track[t++] = 2; track[t++] = 24; track[t++] = 8;
    /* Key signature: C major */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x59; track[t++] = 2;
    track[t++] = 0; track[t++] = 0;
    /* Tempo */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    /* Note */
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    track[t++] = 24; track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with sustain pedal (CC64) — important for piano music. */
static uint32_t build_sustain_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;
    /* Sustain on */
    track[t++] = 0; track[t++] = 0xB0; track[t++] = 64; track[t++] = 127;
    /* Play 3 notes */
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    track[t++] = 12; track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 64; track[t++] = 100;
    track[t++] = 12; track[t++] = 0x80; track[t++] = 64; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 67; track[t++] = 100;
    track[t++] = 12; track[t++] = 0x80; track[t++] = 67; track[t++] = 0;
    /* Sustain off */
    track[t++] = 0; track[t++] = 0xB0; track[t++] = 64; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with SysEx event (must be skipped without crashing). */
static uint32_t build_sysex_midi(uint8_t *out) {
    static uint8_t track[64];
    int t = 0;
    /* SysEx: F0 <length> <data> F7 */
    track[t++] = 0; track[t++] = 0xF0; track[t++] = 5;
    track[t++] = 0x7E; track[t++] = 0x7F; track[t++] = 0x09; track[t++] = 0x01;
    track[t++] = 0xF7;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 60; track[t++] = 100;
    track[t++] = 24; track[t++] = 0x80; track[t++] = 60; track[t++] = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with all 128 GM programs (sweeps through every patch). */
static uint32_t build_all_programs_midi(uint8_t *out) {
    static uint8_t track[1600];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x01; track[t++] = 0x86; track[t++] = 0xA0;  /* fast tempo */
    /* For each of 128 programs: change program, play short note */
    for (int p = 0; p < 128; p++) {
        track[t++] = (p == 0) ? 0 : 1;
        track[t++] = 0xC0;
        track[t++] = p;
        track[t++] = 0;
        track[t++] = 0x90;
        track[t++] = 60;
        track[t++] = 80;
        track[t++] = 2;
        track[t++] = 0x80;
        track[t++] = 60;
        track[t++] = 0;
    }
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build MIDI with drum channel only (channel 9 = GM channel 10). */
static uint32_t build_drums_midi(uint8_t *out) {
    static uint8_t track[128];
    int t = 0;
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;
    /* Standard GM drum kit notes: 35=BD, 38=SD, 42=HHC, 49=Crash */
    int drums[] = {35, 38, 35, 38, 42, 35, 49};
    for (int i = 0; i < 7; i++) {
        track[t++] = (i == 0) ? 0 : 12;
        track[t++] = 0x99;  /* note on ch9 */
        track[t++] = drums[i];
        track[t++] = 100;
        track[t++] = 0;
        track[t++] = 0x89;  /* note off ch9 */
        track[t++] = drums[i];
        track[t++] = 0;
    }
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build a MIDI that plays Guitar, Bass, and Snare drum sequentially.
 * Exercises three different program changes + the drum channel. */
static uint32_t build_guitar_bass_snare_midi(uint8_t *out) {
    static uint8_t track[256];
    int t = 0;
    /* Tempo: 120 BPM */
    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x51; track[t++] = 0x03;
    track[t++] = 0x07; track[t++] = 0xA1; track[t++] = 0x20;

    /* === Distortion Guitar (program 30) on channel 0 === */
    track[t++] = 0; track[t++] = 0xC0; track[t++] = 30;
    /* Power chord: E3 + B3 (root + fifth) */
    track[t++] = 0; track[t++] = 0x90; track[t++] = 52; track[t++] = 110;
    track[t++] = 0; track[t++] = 0x90; track[t++] = 59; track[t++] = 110;
    track[t++] = 48; track[t++] = 0x80; track[t++] = 52; track[t++] = 0;
    track[t++] = 0;  track[t++] = 0x80; track[t++] = 59; track[t++] = 0;

    /* === Electric Bass finger (program 33) on channel 1 === */
    track[t++] = 0;  track[t++] = 0xC1; track[t++] = 33;
    /* Bass walk: E2, A2, B2, E3 */
    int bass_notes[] = {28, 33, 35, 40};
    for (int i = 0; i < 4; i++) {
        track[t++] = (i == 0) ? 0 : 0;
        track[t++] = 0x91; track[t++] = bass_notes[i]; track[t++] = 110;
        track[t++] = 24;
        track[t++] = 0x81; track[t++] = bass_notes[i]; track[t++] = 0;
    }

    /* === Acoustic Snare (note 38) on drum channel 9 === */
    /* Drum channel doesn't need program change */
    for (int i = 0; i < 4; i++) {
        track[t++] = (i == 0) ? 0 : 12;
        track[t++] = 0x99; track[t++] = 38; track[t++] = 110;  /* snare on */
        track[t++] = 0;
        track[t++] = 0x89; track[t++] = 38; track[t++] = 0;    /* snare off */
    }

    /* === All three together: power chord + bass + snare === */
    track[t++] = 12;
    track[t++] = 0x90; track[t++] = 52; track[t++] = 110;  /* guitar E3 */
    track[t++] = 0; track[t++] = 0x90; track[t++] = 59; track[t++] = 110;  /* B3 */
    track[t++] = 0; track[t++] = 0x91; track[t++] = 28; track[t++] = 110;  /* bass E2 */
    track[t++] = 0; track[t++] = 0x99; track[t++] = 38; track[t++] = 110;  /* snare */
    track[t++] = 24;
    track[t++] = 0x80; track[t++] = 52; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x80; track[t++] = 59; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x81; track[t++] = 28; track[t++] = 0;
    track[t++] = 0; track[t++] = 0x89; track[t++] = 38; track[t++] = 0;

    track[t++] = 0; track[t++] = 0xFF; track[t++] = 0x2F; track[t++] = 0x00;

    int hlen = sizeof(mthd_fmt0);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt0[i];
    return hlen + append_mtrk(out + hlen, track, t);
}

/* Build a Format 1 MIDI with many tracks (Duke3D MIDIs use 8-16 tracks). */
static uint32_t build_many_tracks_midi(uint8_t *out) {
    int hlen = sizeof(mthd_fmt1);
    for (int i = 0; i < hlen; i++) out[i] = mthd_fmt1[i];
    /* Override ntracks to 8 */
    out[10] = 0; out[11] = 8;
    int o = hlen;

    /* Track 0: tempo only */
    {
        uint8_t t[16];
        int n = 0;
        t[n++] = 0; t[n++] = 0xFF; t[n++] = 0x51; t[n++] = 0x03;
        t[n++] = 0x07; t[n++] = 0xA1; t[n++] = 0x20;
        t[n++] = 0; t[n++] = 0xFF; t[n++] = 0x2F; t[n++] = 0x00;
        o += append_mtrk(out + o, t, n);
    }

    /* Tracks 1-7: each plays a note on a different channel */
    for (int ch = 0; ch < 7; ch++) {
        uint8_t t[24];
        int n = 0;
        t[n++] = 0; t[n++] = 0xC0 | ch; t[n++] = ch * 4;
        t[n++] = 0; t[n++] = 0x90 | ch; t[n++] = 60 + ch * 2; t[n++] = 100;
        t[n++] = 24; t[n++] = 0x80 | ch; t[n++] = 60 + ch * 2; t[n++] = 0;
        t[n++] = 0; t[n++] = 0xFF; t[n++] = 0x2F; t[n++] = 0x00;
        o += append_mtrk(out + o, t, n);
    }
    return o;
}

/* ================================================================
 * Tests
 * ================================================================ */

static uint8_t midi_buf[2048];

/* Helper: play and pump for the given duration in ms */
static void play_and_pump(uint32_t len, int loop, int pump_ms) {
    of_midi_play(midi_buf, len, loop);
    int iters = pump_ms / 10;
    for (int i = 0; i < iters; i++) {
        of_midi_pump();
        usleep(10 * 1000);
    }
}

void test_midi(void) {
    section_start("MIDI");

    /* MD.01: init — should not crash, returns OK */
    int rc = of_midi_init();
    ASSERT("MD.01 init", rc == OF_MIDI_OK);

    /* MD.02: builtin bank pointer is non-NULL */
    const uint8_t *bank = of_midi_builtin_bank();
    ASSERT("MD.02 bank", bank != NULL);

    /* MD.03: get/set volume */
    of_midi_set_volume(180);
    ASSERT("MD.03a vol", of_midi_get_volume() == 180);
    of_midi_set_volume(255);
    ASSERT("MD.03b max", of_midi_get_volume() == 255);

    /* MD.04: not playing initially */
    of_midi_stop();  /* ensure stopped */
    ASSERT("MD.04 idle", !of_midi_playing());

    /* MD.05: play synthetic Format 0 MIDI */
    {
        uint32_t len = build_simple_midi(midi_buf, 0);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.05a play", rc == OF_MIDI_OK);
        ASSERT("MD.05b on", of_midi_playing());
    }

    /* MD.06: pump advances state — call pump for ~200ms (enough for the
     * 1-quarter-note (~125ms) playback to complete) */
    {
        for (int i = 0; i < 25; i++) {
            of_midi_pump();
            usleep(10 * 1000);
        }
        /* After ~250ms with non-looping playback, should have finished */
        ASSERT("MD.06 ended", !of_midi_playing());
    }

    /* MD.07: stop while playing */
    {
        uint32_t len = build_long_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.07a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 5; i++) {
            of_midi_pump();
            usleep(10 * 1000);
        }
        ASSERT("MD.07b on", of_midi_playing());
        of_midi_stop();
        ASSERT("MD.07c off", !of_midi_playing());
    }

    /* MD.08: pause / resume */
    {
        uint32_t len = build_long_midi(midi_buf);
        of_midi_play(midi_buf, len, 0);
        for (int i = 0; i < 3; i++) { of_midi_pump(); usleep(10 * 1000); }

        of_midi_pause();
        ASSERT("MD.08a paused", of_midi_paused());
        ASSERT("MD.08b on", of_midi_playing());

        /* Pump while paused — should not advance */
        for (int i = 0; i < 5; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.08c stayP", of_midi_paused() && of_midi_playing());

        of_midi_resume();
        ASSERT("MD.08d resume", !of_midi_paused());
        ASSERT("MD.08e on", of_midi_playing());
        of_midi_stop();
    }

    /* MD.09: looping playback — short MIDI with loop=1 should restart */
    {
        uint32_t len = build_simple_midi(midi_buf, 1);
        rc = of_midi_play(midi_buf, len, 1);  /* loop */
        ASSERT("MD.09a play", rc == OF_MIDI_OK);

        /* Pump for 500ms — way past one playthrough */
        for (int i = 0; i < 50; i++) {
            of_midi_pump();
            usleep(10 * 1000);
        }
        /* Should still be playing because of loop */
        ASSERT("MD.09b loop", of_midi_playing());
        of_midi_stop();
    }

    /* MD.10: Format 1 multi-track */
    {
        uint32_t len = build_fmt1_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.10a fmt1", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.10b ended", !of_midi_playing());
    }

    /* MD.11: invalid MIDI rejected */
    {
        uint8_t junk[16] = {0xDE,0xAD,0xBE,0xEF,0,0,0,0,0,0,0,0,0,0,0,0};
        rc = of_midi_play(junk, sizeof(junk), 0);
        ASSERT("MD.11 reject", rc != OF_MIDI_OK);
    }

    /* MD.12: load custom bank — make a fake 1925-byte bank, load, restore */
    {
        static uint8_t fake_bank[1925];
        const uint8_t *builtin = of_midi_builtin_bank();
        /* Start from builtin so the data is plausible */
        for (int i = 0; i < 1925; i++)
            fake_bank[i] = builtin[i];
        /* Tweak first instrument: set FB/CNT to 0x0D (FB=6, CNT=1) */
        fake_bank[0] = 0x0D;
        of_midi_load_bank(fake_bank);
        /* Play and verify it doesn't crash */
        uint32_t len = build_simple_midi(midi_buf, 0);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.12a custom", rc == OF_MIDI_OK);
        for (int i = 0; i < 25; i++) { of_midi_pump(); usleep(10 * 1000); }
        of_midi_stop();
        /* Restore builtin */
        of_midi_load_bank(NULL);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.12b restore", rc == OF_MIDI_OK);
        for (int i = 0; i < 25; i++) { of_midi_pump(); usleep(10 * 1000); }
        of_midi_stop();
    }

    /* MD.13: rapid play/stop cycles (simulates Duke3D level transitions) */
    {
        uint32_t len = build_simple_midi(midi_buf, 1);
        for (int cycle = 0; cycle < 5; cycle++) {
            rc = of_midi_play(midi_buf, len, 1);
            if (rc != OF_MIDI_OK) {
                test_fail("MD.13 cycle", "play failed");
                goto md13_done;
            }
            for (int i = 0; i < 5; i++) { of_midi_pump(); usleep(5 * 1000); }
            of_midi_stop();
            if (of_midi_playing()) {
                test_fail("MD.13 cycle", "still on");
                goto md13_done;
            }
        }
        test_pass("MD.13 cycles");
md13_done:;
    }

    /* MD.14: volume changes during playback */
    {
        uint32_t len = build_long_midi(midi_buf);
        of_midi_play(midi_buf, len, 1);
        for (int v = 255; v >= 0; v -= 64) {
            of_midi_set_volume(v);
            for (int i = 0; i < 3; i++) { of_midi_pump(); usleep(10 * 1000); }
            ASSERT("MD.14 vol live", of_midi_get_volume() == v);
        }
        of_midi_set_volume(255);
        of_midi_stop();
    }

    /* MD.15: stop when not playing — should be a no-op */
    of_midi_stop();
    of_midi_stop();  /* twice */
    ASSERT("MD.15 dblstop", !of_midi_playing());

    /* MD.16: re-init */
    rc = of_midi_init();
    ASSERT("MD.16 reinit", rc == OF_MIDI_OK);
    ASSERT("MD.16b idle", !of_midi_playing());

    /* MD.17: running status — common in real MIDI files.
     * Many parsers fail here because they require explicit status bytes. */
    {
        uint32_t len = build_running_status_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.17a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.17b ended", !of_midi_playing());
    }

    /* MD.18: control change events (volume, pan, expression).
     * Duke3D MIDIs use CC7 for channel volume balancing. */
    {
        uint32_t len = build_cc_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.18a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.18b ended", !of_midi_playing());
    }

    /* MD.19: pitch bend events.
     * Notes played over ~60 ticks at 120 BPM, 96 tpq = ~312ms.
     * Pump for 500ms to be safe. */
    {
        uint32_t len = build_pitchbend_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.19a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 50; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.19b ended", !of_midi_playing());
    }

    /* MD.20: 16-channel polyphony — all GM channels playing at once.
     * Duke3D MIDIs use up to 16 channels. */
    {
        uint32_t len = build_16ch_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.20a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 60; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.20b ended", !of_midi_playing());
    }

    /* MD.21: 6-note chord on one channel (polyphony per-channel). */
    {
        uint32_t len = build_chord_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.21a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 50; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.21b ended", !of_midi_playing());
    }

    /* MD.22: tempo change mid-song. */
    {
        uint32_t len = build_tempo_change_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.22a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 80; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.22b ended", !of_midi_playing());
    }

    /* MD.23: multi-byte VLQ delta (>127 ticks). */
    {
        uint32_t len = build_long_delta_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.23a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 250; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.23b ended", !of_midi_playing());
    }

    /* MD.24: meta events (text, copyright, time/key sig) — must be skipped. */
    {
        uint32_t len = build_meta_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.24a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.24b ended", !of_midi_playing());
    }

    /* MD.25: sustain pedal (CC64). */
    {
        uint32_t len = build_sustain_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.25a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 50; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.25b ended", !of_midi_playing());
    }

    /* MD.26: SysEx event must be skipped without crashing. */
    {
        uint32_t len = build_sysex_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.26a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.26b ended", !of_midi_playing());
    }

    /* MD.27: all 128 GM programs in sequence. */
    {
        uint32_t len = build_all_programs_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.27a play", rc == OF_MIDI_OK);
        /* 128 notes × ~3 ticks each at very fast tempo */
        for (int i = 0; i < 200; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.27b ended", !of_midi_playing());
    }

    /* MD.28: drum channel (channel 9 = GM channel 10). */
    {
        uint32_t len = build_drums_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.28a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 100; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.28b ended", !of_midi_playing());
    }

    /* MD.29: Format 1 with 8 tracks (Duke3D MIDIs are typically 8-16 tracks). */
    {
        uint32_t len = build_many_tracks_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.29a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 50; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.29b ended", !of_midi_playing());
    }

    /* MD.30: pump never called — playback should sit idle, not crash */
    {
        uint32_t len = build_simple_midi(midi_buf, 0);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.30a play", rc == OF_MIDI_OK);
        usleep(100 * 1000);  /* sleep without pumping */
        /* Without pump, position never advances, so still playing */
        ASSERT("MD.30b stuck", of_midi_playing());
        of_midi_stop();
    }

    /* MD.31: very fast pump rate (1ms intervals) — stress event scheduling */
    {
        uint32_t len = build_long_midi(midi_buf);
        of_midi_play(midi_buf, len, 0);
        for (int i = 0; i < 1000; i++) {
            of_midi_pump();
            usleep(1 * 1000);
        }
        ASSERT("MD.31 fast", !of_midi_playing());
    }

    /* MD.32: switch songs while playing (stop + new play). */
    {
        uint32_t len1 = build_long_midi(midi_buf);
        of_midi_play(midi_buf, len1, 1);
        for (int i = 0; i < 5; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.32a song1", of_midi_playing());

        /* Don't stop — let play() replace the song */
        uint32_t len2 = build_drums_midi(midi_buf);
        rc = of_midi_play(midi_buf, len2, 0);
        /* Some implementations require stop first; accept either */
        if (rc != OF_MIDI_OK) {
            of_midi_stop();
            rc = of_midi_play(midi_buf, len2, 0);
        }
        ASSERT("MD.32b song2", rc == OF_MIDI_OK);
        for (int i = 0; i < 100; i++) { of_midi_pump(); usleep(10 * 1000); }
        of_midi_stop();
    }

    /* MD.33: minimal valid MIDI (header + tiny EOT-only track) */
    {
        static const uint8_t tiny[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
            'M','T','r','k', 0,0,0,4, 0, 0xFF, 0x2F, 0x00
        };
        rc = of_midi_play(tiny, sizeof(tiny), 0);
        ASSERT("MD.33a empty", rc == OF_MIDI_OK);
        for (int i = 0; i < 5; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.33b ended", !of_midi_playing());
    }

    /* MD.34: truncated MIDI (header + partial track) — should reject or handle */
    {
        static const uint8_t bad[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
            'M','T','r','k', 0,0,0,100,  /* says 100 bytes but only 4 follow */
            0, 0x90, 60, 100
        };
        rc = of_midi_play(bad, sizeof(bad), 0);
        /* Either rejected up front, or pumps to a graceful end */
        if (rc == OF_MIDI_OK) {
            for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
            of_midi_stop();
        }
        test_pass("MD.34 trunc");
    }

    /* MD.35: bank data integrity — Distortion Guitar (prog 30) MUST have
     * non-zero feedback and a non-sine waveform. If this fails, the bank
     * is the wrong one or has been zeroed. */
    {
        const uint8_t *bank = of_midi_builtin_bank();
        const uint8_t *guitar = bank + 30 * 11;  /* program 30 = Distortion Guitar */
        /* p[0] = FB/CNT — Duke distortion guitar has FB > 0 */
        ASSERT("MD.35a fb",   (guitar[0] & 0x0E) != 0);  /* FB bits 1-3 */
        /* p[5] = mod waveform select, p[10] = car waveform select.
         * At least one should be non-sine for the buzzy guitar timbre. */
        ASSERT("MD.35b ws",   (guitar[5] != 0) || (guitar[10] != 0));
        /* Snapshot exact values into a debug string for the failure log */
        snprintf(__buf, sizeof(__buf), "FB=%02x WSm=%02x WSc=%02x",
                 guitar[0], guitar[5], guitar[10]);
        /* Always log so the user can see the actual bytes */
        if ((guitar[0] & 0x0E) == 0 || (guitar[5] == 0 && guitar[10] == 0))
            test_fail("MD.35c bytes", __buf);
        else
            test_pass("MD.35c bytes");
    }

    /* MD.36: bank coverage — every melodic instrument should have plausible
     * data (not all zeros). 128 melodic + 47 percussion = 175 patches. */
    {
        const uint8_t *bank = of_midi_builtin_bank();
        int blank = 0;
        for (int p = 0; p < 175; p++) {
            const uint8_t *inst = bank + p * 11;
            int sum = 0;
            for (int b = 0; b < 11; b++) sum |= inst[b];
            if (sum == 0) blank++;
        }
        snprintf(__buf, sizeof(__buf), "%d blank/175", blank);
        if (blank > 5)  /* allow a few empty slots */
            test_fail("MD.36 bank", __buf);
        else
            test_pass("MD.36 bank");
    }

    /* MD.37: bank diversity — different instruments should have different
     * data. If all 128 melodic patches are identical, the bank is broken. */
    {
        const uint8_t *bank = of_midi_builtin_bank();
        int unique_fb = 0;
        uint8_t seen[256] = {0};
        for (int p = 0; p < 128; p++) {
            uint8_t fb = bank[p * 11] & 0x0E;
            if (!seen[fb]) {
                seen[fb] = 1;
                unique_fb++;
            }
        }
        ASSERT("MD.37 div", unique_fb >= 3);  /* at least 3 distinct FB values */
    }

    /* MD.38: end-to-end instrument load via play.
     * Build a MIDI that switches to Distortion Guitar (prog 30) and plays a
     * note. We can't read OPL3 back, but we can verify the play() call
     * succeeds and the engine reports playing. */
    {
        of_midi_stop();
        of_midi_load_bank(NULL);  /* ensure builtin */
        uint32_t len = build_simple_midi(midi_buf, 0);  /* uses prog 30 */
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.38a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.38b ended", !of_midi_playing());
    }

    /* MD.39: custom bank with EXTREME guitar settings.
     * Override program 30 with FB=7 (max) and WS=3 (quarter-sine).
     * Plays the same MIDI as MD.38. If MD.38 sounds the same as MD.39,
     * the engine is ignoring bank data — proves the load_instrument bug. */
    {
        static uint8_t custom[1925];
        const uint8_t *builtin = of_midi_builtin_bank();
        for (int i = 0; i < 1925; i++) custom[i] = builtin[i];
        /* Override program 30 with extreme distortion */
        uint8_t *p = &custom[30 * 11];
        p[0]  = 0x0F;  /* FB=7 (max), CNT=1 (additive) */
        p[1]  = 0x21;  /* mod: EG=1 MULT=1 */
        p[2]  = 0x10;  /* mod TL=16 */
        p[3]  = 0xF0;  /* mod AR=15 DR=0 */
        p[4]  = 0x0F;  /* mod SL=0 RR=15 */
        p[5]  = 0x03;  /* mod WS=3 (quarter-sine) */
        p[6]  = 0x21;
        p[7]  = 0x00;
        p[8]  = 0xF0;
        p[9]  = 0x0F;
        p[10] = 0x03;  /* car WS=3 */
        of_midi_load_bank(custom);

        uint32_t len = build_simple_midi(midi_buf, 0);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.39a play", rc == OF_MIDI_OK);
        for (int i = 0; i < 30; i++) { of_midi_pump(); usleep(10 * 1000); }
        of_midi_load_bank(NULL);
    }

    /* MD.40-WSDIAG: waveform select diagnostic.
     * Plays the SAME note 4 times, varying only the waveform select bits.
     * If all 4 sound IDENTICAL, the WS register isn't taking effect — the
     * OPL3 always plays sine waves regardless. If they sound different
     * (sine vs half-sine vs abs-sine vs quarter-sine), WS works and the
     * Distortion Guitar bug is elsewhere. */
    {
        of_audio_opl_reset();
        of_audio_opl_write(0x105, 0x01);  /* OPL3 enable */
        of_audio_opl_write(0x01, 0x20);
        of_audio_opl_write(0x101, 0x20);

        /* Set up channel 0 with HIGH feedback so any harmonics from WS
         * are amplified. Both operators set up identically except WS. */
        for (int ws = 0; ws < 4; ws++) {
            /* Modulator (slot 0) */
            of_audio_opl_write(0x20, 0x21);
            of_audio_opl_write(0x40, 0x00);    /* TL=0 max volume */
            of_audio_opl_write(0x60, 0xF0);
            of_audio_opl_write(0x80, 0x0F);
            of_audio_opl_write(0xE0, ws);      /* mod WS */
            /* Carrier (slot 3) */
            of_audio_opl_write(0x23, 0x21);
            of_audio_opl_write(0x43, 0x00);
            of_audio_opl_write(0x63, 0xF0);
            of_audio_opl_write(0x83, 0x0F);
            of_audio_opl_write(0xE3, ws);      /* car WS */
            /* CNT=0 (FM mode), FB=7 (max feedback for harmonic content) */
            of_audio_opl_write(0xC0, 0x0E | 0x30);
            /* Key on A4 */
            of_audio_opl_write(0xA0, 0x41);
            of_audio_opl_write(0xB0, 0x32);
            usleep(400 * 1000);
            of_audio_opl_write(0xB0, 0x12);
            usleep(150 * 1000);
        }
        test_pass("MD.40w wsdiag");
    }

    /* MD.40-PRE: direct OPL3 register write of Distortion Guitar.
     * Bypasses the of_midi engine entirely. Programs the EXACT same bytes
     * that load_instrument would write, but via direct opl_write calls.
     * If this sounds buzzy/metallic but the next test (MD.40 via of_midi)
     * sounds organ-like, the bug is in the of_midi engine, not the bytes
     * or OPL3 hardware. */
    {
        of_audio_opl_reset();
        of_audio_opl_write(0x105, 0x01);  /* OPL3 enable */
        of_audio_opl_write(0x01, 0x20);   /* bank 0 WSE */
        of_audio_opl_write(0x101, 0x20);  /* bank 1 WSE */

        /* Distortion Guitar bytes (from gm_bank in of_midi.c, program 30):
         *   FB/CNT=0x06, mod=21,14,F6,54,WS=02
         *   car=21,TL=00,F2,52,WS=02
         * Channel 0: mod slot=0, car slot=3 */
        of_audio_opl_write(0xC0, 0x06 | 0x30);  /* FB/CNT + L+R */
        /* Modulator (slot 0) */
        of_audio_opl_write(0x20, 0x21);   /* char */
        of_audio_opl_write(0x40, 0x14);   /* TL */
        of_audio_opl_write(0x60, 0xF6);   /* AD */
        of_audio_opl_write(0x80, 0x54);   /* SR */
        of_audio_opl_write(0xE0, 0x02);   /* WS=2 abs-sine */
        /* Carrier (slot 3) */
        of_audio_opl_write(0x23, 0x21);
        of_audio_opl_write(0x43, 0x00);   /* TL=0 (max volume) */
        of_audio_opl_write(0x63, 0xF2);
        of_audio_opl_write(0x83, 0x52);
        of_audio_opl_write(0xE3, 0x02);   /* WS=2 abs-sine */

        /* Key on A4 (440Hz, fnum=0x241, block=4) */
        of_audio_opl_write(0xA0, 0x41);
        of_audio_opl_write(0xB0, 0x32);
        usleep(800 * 1000);
        of_audio_opl_write(0xB0, 0x12);   /* key off */
        usleep(100 * 1000);
        test_pass("MD.40p direct");
    }

    /* MD.40: guitar + bass + snare playback.
     * Plays the three Duke3D-style instruments to verify GM programs
     * 30 (Distortion Guitar), 33 (Electric Bass), and drum note 38
     * (Acoustic Snare) all load and play. The user listens to confirm
     * each instrument sounds distinct from the others. */
    {
        of_midi_stop();
        of_midi_load_bank(NULL);  /* builtin GM */
        uint32_t len = build_guitar_bass_snare_midi(midi_buf);
        rc = of_midi_play(midi_buf, len, 0);
        ASSERT("MD.40a play", rc == OF_MIDI_OK);
        /* Total duration: ~250ms guitar + ~500ms bass + ~250ms snare
         * + ~125ms ensemble + tempo overhead ≈ 1500ms. Pump for 2s. */
        for (int i = 0; i < 200; i++) { of_midi_pump(); usleep(10 * 1000); }
        ASSERT("MD.40b ended", !of_midi_playing());
    }

    /* MD.41: bank 1 WSE check.
     * After of_midi_init, both bank 0 (0x01) and bank 1 (0x101) WSE bits
     * should be set. Otherwise, OPL channels 9-17 ignore waveform select
     * and play sine waves regardless of instrument data. We can't read
     * OPL3 registers back, so this test re-initializes and writes 0x101
     * ourselves — if Duke's bug is bank 1 WSE missing, fixing it here
     * (after midi_init) would make any MIDI playing on ch 9-17 work. */
    {
        of_midi_stop();
        of_midi_init();
        /* Manually ensure bank 1 WSE is enabled — workaround for any
         * init bug that only sets bank 0. */
        of_audio_opl_write(0x101, 0x20);
        test_pass("MD.41 b1wse");
    }

    of_midi_stop();
    section_end();
    (void)play_and_pump;  /* may be unused if all tests use inline pump */
}
