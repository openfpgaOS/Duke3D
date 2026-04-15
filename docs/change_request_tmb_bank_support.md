# Change Request: Support DMX 2-op Timbre Banks in of_midi.c

## Problem

Duke3D ships a custom OPL instrument bank (`d3dtimbr.tmb`) in DMX 2-op format.
The current `of_midi_load_bank()` only accepts the 26-byte WOPL/4-op record
format. Duke3D calls `MUSIC_RegisterTimbreBank(tmb_bank)` at startup, but the
SDK bridge (`midi_of.c`) discards it because the formats are incompatible.
The MIDI engine falls back on the built-in GM bank, losing the signature
Duke3D OPL sound.

## Current 4-op Bank Format (OF_MIDI_INST4_SIZE = 26 bytes per instrument)

```
Offset  Size  Description
  0       1   flags  (bit0 = pseudo-4-op, bit1 = blank, bit6 = fixed-pitch)
  1       1   note_offset1  (int8, semitones for voice 1)
  2       1   C0 byte voice 1  (FB[3:1] | CNT1[0])
  3       1   C0 byte voice 2  (FB | CNT2; pseudo-4-op only)
  4-8     5   op1 (voice-1 modulator): AVEK_MULT, KSL_TL, AR_DR, SL_RR, WS
  9-13    5   op2 (voice-1 carrier):   same 5 registers
 14-18    5   op3 (voice-2 modulator): pseudo-4-op only
 19-23    5   op4 (voice-2 carrier):   pseudo-4-op only
 24       1   percussion_key_number  (0 = melodic)
 25       1   note_offset2  (int8, semitones for voice 2)
```

128 melodic entries followed by 47 drum entries (GM notes 35-81).

## DMX TMB Format (13 bytes per entry, Duke3D's d3dtimbr.tmb)

The TMB file is a flat array of (program_number, register_data) pairs.
Each entry overrides a single GM program. The file typically contains
only the instruments the game actually uses (sparse, not all 128).

```
Offset  Size  Description
  0       1   GM program number (0-127 melodic, 128+ = drum offset)
  1       1   0x20 modulator  (AM | VIB | EGT | KSR | MULT)
  2       1   0x20 carrier
  3       1   0x40 modulator  (KSL | TL)
  4       1   0x40 carrier
  5       1   0x60 modulator  (AR | DR)
  6       1   0x60 carrier
  7       1   0x80 modulator  (SL | RR)
  8       1   0x80 carrier
  9       1   0xE0 modulator  (waveform select)
 10       1   0xE0 carrier
 11       1   0xC0  (FB[3:1] | CNT[0])
 12       1   note offset (int8, signed semitones)
```

Total file size = N * 13, where N = number of overridden patches.

## Proposed Change

Add a new API function to of_midi:

```c
void of_midi_load_bank_dmx(const uint8_t *tmb_data, uint32_t tmb_size);
```

This function:

1. Allocates a static 26-byte-per-instrument bank buffer (128 melodic + 47 drum
   = 175 * 26 = 4550 bytes).

2. Initializes it as a copy of the built-in GM bank (so unpatched instruments
   keep their default sound).

3. Iterates the TMB entries (tmb_size / 13 entries). For each:
   - Read the GM program number from byte 0.
   - If program < 128: patch the melodic instrument.
   - If program >= 128: patch the drum instrument at index (program - 128 + 35).
   - Convert the 13-byte TMB record to the 26-byte WOPL layout:

```c
/* Conversion: TMB 2-op → WOPL 26-byte record */
static void tmb_to_inst4(const uint8_t *tmb, uint8_t *out) {
    memset(out, 0, 26);
    out[0]  = 0x00;          /* flags: no pseudo-4-op, not blank */
    out[1]  = (int8_t)tmb[12]; /* note_offset1 */
    out[2]  = tmb[11];       /* C0 (FB | CNT) */
    out[3]  = 0;             /* C0 voice 2 (unused for 2-op) */
    /* op1 = modulator: AVEK_MULT, KSL_TL, AR_DR, SL_RR, WS */
    out[4]  = tmb[1];        /* 0x20 mod */
    out[5]  = tmb[3];        /* 0x40 mod */
    out[6]  = tmb[5];        /* 0x60 mod */
    out[7]  = tmb[7];        /* 0x80 mod */
    out[8]  = tmb[9];        /* 0xE0 mod */
    /* op2 = carrier */
    out[9]  = tmb[2];        /* 0x20 car */
    out[10] = tmb[4];        /* 0x40 car */
    out[11] = tmb[6];        /* 0x60 car */
    out[12] = tmb[8];        /* 0x80 car */
    out[13] = tmb[10];       /* 0xE0 car */
    /* ops 3-4 zeroed (no pseudo-4-op) */
    out[24] = 0;             /* percussion_key_number */
    out[25] = 0;             /* note_offset2 */
}
```

4. Calls `of_midi_load_bank(converted_buffer)` to install the patched bank.

## SDK-side Integration

Once the OS exposes `of_midi_load_bank_dmx`, the SDK bridge in
`midi_of.c` changes `MUSIC_RegisterTimbreBank` from a no-op to:

```c
void MUSIC_RegisterTimbreBank(uint8_t *timbres)
{
    /* Duke3D d3dtimbr.tmb is a sparse DMX 2-op bank.
     * Convert to the 26-byte format and overlay on the built-in GM bank. */
    if (timbres && midi_initialized)
        of_midi_load_bank_dmx(timbres, tmb_size);
}
```

(The caller would need to pass the size, or `loadtmb()` in game.c would
store it alongside the buffer.)

## Alternative: SDK-Side Conversion Only

If adding `of_midi_load_bank_dmx` to the OS is too invasive, the entire
conversion can happen in `midi_of.c` using the existing
`of_midi_load_bank()` API — just do the TMB→WOPL conversion in the SDK
and call `of_midi_load_bank(converted)`. The OS needs no changes. The
downside is 4550 bytes of static RAM in the app binary for the
converted bank buffer.

## Scope

- OS: add `of_midi_load_bank_dmx()` (or do it SDK-side only)
- SDK: wire `MUSIC_RegisterTimbreBank` to the converter
- SDK: store TMB file size in `loadtmb()` for the size parameter
- No changes to the MIDI playback engine — the converted bank is
  indistinguishable from a native 4-op bank (just with ops 3-4 zeroed
  and pseudo-4-op flag clear)
