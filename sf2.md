# SF2 SoundFont Music for Duke Nukem 3D on Analogue Pocket

## Problem

The OPL3 FM synthesis path has unresolved issues with instrument configuration that make the music sound organ-like instead of the intended metallic/guitar tones. SF2 SoundFont playback through the hardware PCM mixer is a higher-quality alternative that bypasses OPL3 entirely.

## Architecture

```
Current (OPL3 FM):
  MIDI file → of_midi parser → OPL3 register writes → YMF262 FPGA core → audio mix

Proposed (SF2 PCM):
  MIDI file → SF2 MIDI engine → PCM mixer voices → hardware mixer → audio mix
```

Both paths mix at the FPGA audio output stage. SF2 replaces OPL3 synthesis with pre-recorded instrument samples played through the 32-voice hardware PCM mixer.

## Hardware Resources

| Resource | Available | Used by SFX | Available for Music |
|----------|-----------|-------------|---------------------|
| PCM mixer voices | 32 | ~8 peak | 18-24 |
| CRAM1 (sample RAM) | 16 MB | ~1 MB (SFX cache) | 8-12 MB |
| CPU | 100 MHz RV32 | minimal | MIDI parsing only |

The mixer handles resampling, volume, panning, and looping in hardware — zero CPU cost per voice.

## SF2 File Selection

| SoundFont | Size | Quality | Fits in CRAM? |
|-----------|------|---------|---------------|
| Creative GM35REVC | 4 MB | Basic | Yes |
| Creative 8MBGMSFX | 8 MB | Good | Yes |
| GeneralUser GS | 30 MB | Excellent | No (needs streaming) |
| SC-55 Emulation | 15-25 MB | Authentic | Partial |

**Recommended**: An 8 MB GM SoundFont provides good quality and fits entirely in CRAM1. Duke3D's MIDI files use standard GM program changes, so any GM-compliant SF2 works.

## Implementation Plan

### Phase 1: SF2 Loader (load SF2 from data slot)

**Goal**: Parse SF2 file, extract sample data and preset/instrument metadata.

**Steps**:
1. Load SF2 file from Pocket data slot (slot:4 or similar)
2. Parse RIFF container → extract `pdta` (metadata) and `sdta` (samples)
3. Build lookup tables:
   - `preset[128]` → melodic instruments (GM programs 0-127)
   - `drum[47]` → percussion (GM drum map notes 35-81)
   - Each entry: sample pointer, loop start/end, base note, fine tune, attenuation
4. Upload raw PCM sample data to CRAM1 via `of_mixer_alloc_samples()`

**Files to create**:
- `src/duke3d/sf2_loader.c` — SF2 parser
- `src/duke3d/sf2_loader.h` — data structures

**Key data structures**:
```c
typedef struct {
    uint8_t  *pcm;          /* CRAM1 pointer to 16-bit PCM */
    uint32_t  sample_count;
    uint32_t  sample_rate;
    uint32_t  loop_start;
    uint32_t  loop_end;
    int8_t    base_note;    /* MIDI note at which sample plays at original pitch */
    int8_t    fine_tune;    /* cents offset */
    uint8_t   attenuation;  /* base volume (0=loud, 127=quiet) */
} sf2_sample_t;

typedef struct {
    sf2_sample_t *samples;  /* array of key/velocity-split samples */
    int           num_samples;
    int           key_lo, key_hi;  /* MIDI note range */
} sf2_preset_t;

static sf2_preset_t presets[128];  /* melodic */
static sf2_preset_t drums[47];    /* percussion (notes 35-81) */
```

### Phase 2: SF2 MIDI Engine (replace of_midi for music)

**Goal**: Parse Duke3D MIDI files and drive the PCM mixer with SF2 samples.

**Steps**:
1. Reuse `of_midi`'s MIDI parser (Format 0/1, track parsing, delta time)
2. Replace OPL3 note-on with PCM mixer voice allocation:
   - Look up preset from program change
   - Find sample matching note + velocity
   - Calculate playback rate: `rate = sample_rate * 2^((note - base_note) / 12.0)`
   - Use `of_mixer_play()` with calculated rate
   - Set loop via `of_mixer_set_loop()` if sample has loop points
3. Note-off: stop voice or let it release naturally
4. Volume (CC7): `of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, vol)`
5. Pan (CC10): `of_mixer_set_vol_lr(voice, left, right)`
6. Pitch bend: `of_mixer_set_rate(voice, adjusted_rate)`

**Files to create**:
- `src/duke3d/sf2_midi.c` — MIDI engine using PCM mixer
- `src/duke3d/sf2_midi.h` — API (drop-in replacement for midi_of.c)

**Playback rate calculation**:
```c
/* Convert MIDI note to playback rate (16.16 fixed-point) */
static uint32_t note_to_rate(int note, int base_note, uint32_t sample_rate) {
    /* semitone_ratio = 2^(1/12) ≈ 1.05946 */
    /* Use lookup table for 2^(n/12) in 16.16 fixed-point */
    int diff = note - base_note;
    uint32_t base_rate = OF_MIXER_RATE_FP16(sample_rate);
    /* Apply pitch shift via pre-computed table */
    return (base_rate * pitch_table[diff + 128]) >> 16;
}
```

### Phase 3: Integration with Duke3D

**Goal**: Wire SF2 engine into Duke3D's MUSIC_* interface.

**Steps**:
1. In `midi_of.c`, add SF2 path alongside OPL3:
   - If SF2 is loaded → use `sf2_midi_play()` / `sf2_midi_pump()`
   - If no SF2 → fall back to `of_midi` OPL3 path
2. Load SF2 from data slot during `MUSIC_Init()`
3. `PlayMusic()` passes MIDI data to SF2 engine instead of of_midi
4. `d3d_audio_pump()` calls `sf2_midi_pump()` for MIDI timing

**Modified files**:
- `src/duke3d/Game/src/midi/midi_of.c` — add SF2 fallback
- `src/duke3d/d3d_audio.c` — pump SF2 engine
- `src/duke3d/Makefile` — add new source files

### Phase 4: Packaging

**Goal**: Distribute SF2 file with the core.

**Steps**:
1. Add SF2 to data slot 4 in `data.json`:
   ```json
   { "id": 4, "name": "Music SoundFont", "required": false,
     "parameters": { "extensions": ["sf2"] } }
   ```
2. Ship a default 8MB GM SF2 with the core package
3. Allow user to replace with their preferred SF2

## Effort Estimate

| Phase | Complexity | Dependencies |
|-------|-----------|-------------|
| Phase 1: SF2 Loader | Medium | RIFF parsing, memory management |
| Phase 2: MIDI Engine | Medium | Reuse of_midi parser, pitch math |
| Phase 3: Integration | Low | Wire into existing MUSIC_* API |
| Phase 4: Packaging | Low | data.json + file distribution |

## Risks

- **Memory**: 8MB SF2 leaves ~4MB for SFX in CRAM1. May need to reduce SFX cache or use streaming.
- **Voice count**: Complex MIDI passages use 15+ simultaneous notes. With SFX using some voices, may need voice stealing.
- **Pitch accuracy**: Fixed-point pitch calculation needs a good 2^(n/12) lookup table.
- **Loop quality**: Short loops can produce audible clicks at loop points — may need crossfade.

## Alternative: Fix OPL3 Path

If the OPL3 organ-like sound issue is resolved, the TMB bank approach is simpler:
- Zero additional memory (TMB is 2KB, GM bank is 2KB)
- No SF2 file needed
- Authentic 1996 Sound Blaster sound

The OPL3 issue appears to be in the `of_midi` engine's register write path — direct MMIO writes produce correct sounds, but the same writes through `of_midi`'s note-on flow sound organ-like. Disabling the instrument cache and always reloading registers is the next test.
