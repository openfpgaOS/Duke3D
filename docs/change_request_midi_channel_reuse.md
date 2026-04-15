# Change Request: MIDI Note-Off Should Not Immediately Reuse OPL3 Channels

## Problem

When `of_midi` processes a MIDI note-off, it immediately kills the OPL3
channel (`opl_ch_write(ch, 0xB0, 0x00)` — KEY-OFF) and marks it as free
for the next note-on. This cuts the release phase of the ADSR envelope
short — the sound stops abruptly instead of fading out naturally.

OPL3 has hardware release envelopes (the R in ADSR, configured via the
0x80 register's low 4 bits). After KEY-OFF, the hardware ramps the
operator levels down at the rate set by the Release Rate. This produces
a natural decay that's essential for realistic instrument sounds (piano
sustain, pad tails, reverb trails).

## Current Behavior (of_midi.c)

```
note-off → kill_channel(ch):
    opl_ch_write(ch, 0xB0, 0x00);   // KEY-OFF
    M.opl[ch].note = -1;             // mark free
    M.opl[ch].midi_ch = -1;          // immediately available for reuse
```

The channel is available for the next `alloc_2op()` call immediately.
If a new note arrives in the same MIDI tick, it reuses the channel,
cutting the release tail of the previous note.

## Desired Behavior

After KEY-OFF, the channel should remain "occupied" for a short time
to let the OPL3 hardware envelope complete its release phase. Only
after the release time has elapsed should the channel be returned to
the free pool.

### Approach A: Release-Time Hold (Recommended)

1. On note-off: send KEY-OFF to OPL3 (starts hardware release), but
   mark the channel as "releasing" rather than "free."

2. `alloc_2op()` skips "releasing" channels when free channels are
   available. Only steals a "releasing" channel as a last resort
   (when all 18 channels are busy).

3. A timer (checked in `of_midi_pump`) transitions "releasing"
   channels to "free" after a configurable holdoff:
   - Compute from the patch's Release Rate register (0x80 low nibble):
     `release_ms = opl3_release_time_ms[RR]` (lookup table)
   - Or use a fixed holdoff (e.g., 200ms) for simplicity.

### Approach B: Fixed Holdoff (Simpler)

Same as Approach A but with a fixed 150-200ms holdoff instead of
computing from the Release Rate. Simpler to implement, slightly
less optimal (holds channels longer than needed for fast-release
patches, not long enough for very slow releases).

## OPL3 Release Time Reference

The OPL3 release rate (0x80 register, bits 3:0) maps to these
approximate release times at typical KSR/block settings:

| RR value | Release time (approx) |
|----------|----------------------|
| 0        | infinite (no release)|
| 1        | ~1600 ms             |
| 2        | ~800 ms              |
| 3        | ~400 ms              |
| 4-6      | ~200-100 ms          |
| 7-9      | ~80-40 ms            |
| 10-12    | ~20-10 ms            |
| 13-15    | ~5-2 ms              |

Most GM instruments use RR 4-8, so a 200ms fixed holdoff covers
the majority of cases.

## Channel States

```
FREE       → available for alloc_2op()
PLAYING    → note is active (KEY-ON set)
RELEASING  → KEY-OFF sent, hardware envelope decaying
               → transitions to FREE after holdoff
               → can be stolen by alloc_2op() if no FREE channels
```

## Allocation Priority (alloc_2op)

```
1. FREE channels           (best — no sound impact)
2. RELEASING channels      (acceptable — tail gets cut, barely audible)
3. PLAYING channels        (last resort — steals active note, audible)
```

Within each category, prefer the oldest channel (existing age-based
voice stealing).

## Scope

- OS only: changes to `of_midi.c` (kill_channel, alloc_2op, pump loop)
- No SDK or game-side changes needed
- Backward compatible: existing `of_midi_*` API unchanged
