/*
 * of_midi.h -- MIDI playback for openfpgaOS
 *
 * Plays Standard MIDI Files (Format 0 and 1) through the hardware
 * OPL3 (YMF262) FM synthesizer.  Non-blocking: call of_midi_pump()
 * from your game loop or idle hook.
 *
 * Usage:
 *   of_midi_init();
 *   of_midi_play(midi_data, midi_len, 1);  // loop
 *   while (1) {
 *       of_midi_pump();
 *       // ... game logic ...
 *   }
 */

#ifndef OF_MIDI_H
#define OF_MIDI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Error codes */
#define OF_MIDI_OK            0
#define OF_MIDI_ERR_NOT_INIT  (-1)
#define OF_MIDI_ERR_BAD_HDR   (-2)
#define OF_MIDI_ERR_FORMAT    (-3)
#define OF_MIDI_ERR_NO_TRACKS (-4)
#define OF_MIDI_ERR_PLAYING   (-5)

/* Instrument patch size in the built-in WOPL-derived bank:
 *   [0]     flags   (bit0 = pseudo-4-op, bit1 = blank, bit6 = fixed-pitch)
 *   [1]     note_offset1 (int8, semitones for voice 1)
 *   [2]     C0 register, voice 1 (FB | CNT1)
 *   [3]     C0 register, voice 2 (FB | CNT2)
 *   [4..8]  op1 = modulator of voice 1  (AVEK_MULT, KSL_TL, AR_DR, SL_RR, WS)
 *   [9..13] op2 = carrier  of voice 1
 *   [14..18] op3 = modulator of voice 2 (pseudo-4-op stack)
 *   [19..23] op4 = carrier  of voice 2
 *   [24]    percussion_key_number (drum fixed OPL3 play note; 0 melodic)
 *   [25]    note_offset2 (int8, semitones for voice 2 stack)
 *
 * Pseudo-4-op patches are loaded as TWO independent 2-op voices on
 * separate OPL3 channels (voice 1 ops on one channel, voice 2 ops on
 * another), with their own note offsets — this matches the DMXOPL3
 * style where most "GM" instruments are stacked 2-op pairs rather than
 * true OPL3 4-op. The bank is 128 melodic + 47 GM drums × 26 bytes. */
#define OF_MIDI_INST_SIZE 26

/* Initialize MIDI subsystem.  Resets OPL3 and enables OPL3 mode. */
int of_midi_init(void);

/* Start playback of a Standard MIDI File in memory.
 * data/len: raw .mid file bytes.
 * loop: 1 = restart on end-of-track, 0 = play once.
 * Returns OF_MIDI_OK or an error code. */
int of_midi_play(const uint8_t *data, uint32_t len, int loop);

/* Stop playback, silence all channels. */
void of_midi_stop(void);

/* Pause / resume playback (notes sustain during pause). */
void of_midi_pause(void);
void of_midi_resume(void);

/* Process pending MIDI events.  Call this every frame.
 * Uses of_time_us() internally for timing. */
void of_midi_pump(void);

/* Query state */
int of_midi_playing(void);
int of_midi_paused(void);

/* Master volume (0 = silent, 255 = full).  Default: 255. */
void of_midi_set_volume(int volume);
int  of_midi_get_volume(void);

/* Load a custom instrument bank: 128 melodic + 47 drum records in the
 * WOPL-derived 4-op format described at OF_MIDI_INST_SIZE above
 * (175 × 26 = 4550 bytes total). Pass NULL to restore the built-in bank. */
void of_midi_load_bank(const uint8_t *bank);

/* Pointer to the built-in GM instrument bank (4550 bytes, read-only).
 * First 128 records are melodic programs 0..127, next 47 are GM drum
 * notes 35..81 in order. Useful for merging custom patches over the
 * default bank. */
const uint8_t *of_midi_builtin_bank(void);

#ifdef __cplusplus
}
#endif

#endif /* OF_MIDI_H */
