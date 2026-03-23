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

#include <stdint.h>

/* Error codes */
#define OF_MIDI_OK            0
#define OF_MIDI_ERR_NOT_INIT  (-1)
#define OF_MIDI_ERR_BAD_HDR   (-2)
#define OF_MIDI_ERR_FORMAT    (-3)
#define OF_MIDI_ERR_NO_TRACKS (-4)
#define OF_MIDI_ERR_PLAYING   (-5)

/* Instrument patch size (11 bytes: FB/CNT + 5 mod + 5 car) */
#define OF_MIDI_INST_SIZE 11

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

/* Load a custom instrument bank (128 melodic + 47 percussion instruments,
 * 11 bytes each = 1925 bytes total).  Pass NULL to restore built-in bank. */
void of_midi_load_bank(const uint8_t *bank);

#endif /* OF_MIDI_H */
