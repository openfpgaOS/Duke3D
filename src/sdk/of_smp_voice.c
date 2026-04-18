/*
 * of_smp_voice.c -- Sample voice engine: 48-voice polyphony with
 *                   DAHDSR envelopes, dual LFOs, and pitch bend.
 */

#include "include/of_smp_voice.h"
#include "include/of_smp_bank.h"
#include "include/of_mixer.h"
#include "include/of_cache.h"
#include "include/of_timer.h"
#include "include/of_services.h"

/* From of_smp_tables.c */
extern const uint32_t smp_cents_to_mult[1200];
extern int32_t smp_timecents_to_ticks(int16_t tc);

/* ------------------------------------------------------------------ */
/* Static state                                                       */
/* ------------------------------------------------------------------ */

static smp_voice_t voices[SMP_MAX_VOICES];
static uint32_t tick_counter;

/* ------------------------------------------------------------------ */
/* Tick-cost probe (Task #10)                                         */
/* ------------------------------------------------------------------ */
/* NOTE: VexRiscv here does not expose rdcycle to user mode, so we use
 * OF_SVC->timer_get_us() (direct service-table call — NOT the ecall
 * of_time_us(), which would nest-trap when smp_voice_tick runs from
 * the MIDI timer ISR). Stats are in microseconds. */

static uint32_t tick_us_max;
static uint32_t tick_us_last;
static uint32_t tick_spike_count;
static uint32_t tick_stat_count;
static uint8_t  tick_active_peak;
static uint8_t  tick_stage_sustain;
static uint8_t  tick_stage_release;
static uint8_t  tick_stage_decay;
static uint8_t  tick_sustain_held;
static uint8_t  tick_ch_active[16];

/* A/B/C instrumentation counters — see smp_tick_stats_t for descriptions.
 * These are incremented at actual HW-write sites (post-cache) and from
 * smp_voice_tick_record_pump(), then snapshotted by get_stats and zeroed
 * by reset_stats. */
static uint32_t stat_filter_writes;
static uint32_t stat_rate_writes;
static uint32_t stat_vol_writes;
static uint32_t stat_pump_count;
static uint32_t stat_pump_interval_max_us;
static uint32_t stat_pump_interval_min_us = 0xFFFFFFFFu;
static uint32_t stat_pump_burst_count;
static uint32_t stat_pump_budget_exceeded;
static uint16_t stat_cutoff_delta_max;

/* 2 ms budget = 500 Hz tick rate. */
#define SMP_TICK_SPIKE_US  2000u

void smp_voice_tick_get_stats(smp_tick_stats_t *out)
{
    if (!out) return;
    /* Note: field is named cycles_* for ABI stability but holds microseconds. */
    out->cycles_max    = tick_us_max;
    out->cycles_last   = tick_us_last;
    out->spike_count   = tick_spike_count;
    out->tick_count    = tick_stat_count;
    out->active_peak   = tick_active_peak;
    out->stage_sustain = tick_stage_sustain;
    out->stage_release = tick_stage_release;
    out->stage_decay   = tick_stage_decay;
    out->sustain_held  = tick_sustain_held;
    for (int i = 0; i < 16; i++)
        out->ch_active[i] = tick_ch_active[i];

    out->filter_writes         = stat_filter_writes;
    out->rate_writes           = stat_rate_writes;
    out->vol_writes            = stat_vol_writes;
    out->pump_count            = stat_pump_count;
    out->pump_interval_max_us  = stat_pump_interval_max_us;
    out->pump_interval_min_us  = stat_pump_interval_min_us;
    out->pump_burst_count      = stat_pump_burst_count;
    out->pump_budget_exceeded  = stat_pump_budget_exceeded;
    out->cutoff_delta_max      = stat_cutoff_delta_max;
}

void smp_voice_tick_reset_stats(void)
{
    tick_us_max      = 0;
    tick_spike_count = 0;
    tick_stat_count  = 0;
    tick_active_peak = 0;

    stat_filter_writes        = 0;
    stat_rate_writes          = 0;
    stat_vol_writes           = 0;
    stat_pump_count           = 0;
    stat_pump_interval_max_us = 0;
    stat_pump_interval_min_us = 0xFFFFFFFFu;
    stat_pump_burst_count     = 0;
    stat_pump_budget_exceeded = 0;
    stat_cutoff_delta_max     = 0;
}

void smp_voice_tick_record_pump(uint32_t elapsed_us, int ticks_fired,
                                int budget_exceeded)
{
    stat_pump_count++;
    if (elapsed_us > stat_pump_interval_max_us)
        stat_pump_interval_max_us = elapsed_us;
    if (elapsed_us < stat_pump_interval_min_us)
        stat_pump_interval_min_us = elapsed_us;
    if (ticks_fired > 1) stat_pump_burst_count++;
    if (budget_exceeded) stat_pump_budget_exceeded++;
}

/* Per-channel state (16 MIDI channels) */
static int ch_volume[16];       /* CC7  (0-127) */
static int ch_expression[16];   /* CC11 (0-127) */
static int ch_pan[16];          /* CC10 (0-127, 64=center) */
static int ch_bend[16];         /* -8192..+8191 */
static int ch_mod_depth[16];    /* CC1  (0-127) */
static int ch_sustain[16];      /* CC64 on/off */
static int ch_brightness[16];   /* CC74 (0-127) */
static int ch_resonance[16];    /* CC71 (0-127) */
static int master_vol = 255;

/* Cached mixer state to avoid redundant CDC writes */
static uint32_t prev_rate[SMP_MAX_VOICES];
static int      prev_vol_l[SMP_MAX_VOICES];
static int      prev_vol_r[SMP_MAX_VOICES];

/* Voices pending steal (waiting for hardware fade-out) */
#define STEAL_PENDING -2

/* Minimum envelope level before we consider it done */
#define ENV_FLOOR 0x100

/* Pitch bend range in cents (standard: +/-2 semitones) */
#define BEND_RANGE_CENTS 200

/* ------------------------------------------------------------------ */
/* Fixed-point helpers                                                */
/* ------------------------------------------------------------------ */

static uint32_t cents_to_rate_multiplier(int32_t cents)
{
    /* Handle octave + remainder decomposition.
     * smp_cents_to_mult covers 0..1199 cents (one octave).
     * Result is Q16.16: 0x10000 = multiply by 1.0 */
    int octaves = 0;
    while (cents >= 1200) { cents -= 1200; octaves++; }
    while (cents < 0)     { cents += 1200; octaves--; }

    uint32_t m = smp_cents_to_mult[cents];
    if (octaves > 0) m <<= octaves;
    else if (octaves < 0) m >>= (-octaves);
    return m;
}

static int32_t triangle_wave(int32_t phase)
{
    /* phase is Q16.16 wrapping at 0x10000.
     * Output: -0x10000 .. +0x10000 (Q16.16 signed) */
    phase &= 0xFFFF;
    if (phase < 0x4000)
        return (phase << 2);                    /* 0..0x10000 */
    else if (phase < 0xC000)
        return 0x20000 - (phase << 2);          /* 0x10000..-0x10000 */
    else
        return (phase << 2) - 0x40000;          /* -0x10000..0 */
}

static int32_t compute_sustain_level(int16_t centibels)
{
    /* SF2 sustainVolEnv is centibels of attenuation.
     * 0 cB = full volume (0x10000), 1000 cB = -100 dB ≈ silence.
     * level = 10^(-cB/200) = 2^(-cB * log2(10) / 200)
     *       = 2^(-cB * 0.01661)
     * Convert to negative cents: neg_cents = cB * 1661 / 100
     * Then use cents_to_mult for the fractional part. */
    if (centibels <= 0)    return 0x10000;
    if (centibels >= 960)  return 0;

    /* neg_cents = centibels * 3.3219 (log2(10) * 10) */
    int32_t neg_cents = (int32_t)centibels * 3322 / 1000;
    int octaves = neg_cents / 1200;
    int rem = neg_cents - octaves * 1200;
    if (rem < 0) { rem += 1200; octaves--; }

    /* level = 0x10000 / 2^octaves / cents_to_mult[rem] * 65536 */
    uint32_t mult = smp_cents_to_mult[rem];
    /* 0x10000 * 65536 / mult >> octaves, but avoid 64-bit overflow */
    uint64_t level = ((uint64_t)0x10000 << 16) / mult;
    int shift = octaves; /* positive = attenuate */
    if (shift >= 16) return 0;
    if (shift > 0) level >>= shift;
    if (level > 0x10000) level = 0x10000;
    return (int32_t)level;
}

/* ------------------------------------------------------------------ */
/* Envelope helpers                                                   */
/* ------------------------------------------------------------------ */

static void env_init(env_state_t *e, int16_t delay_tc, int16_t attack_tc,
                     int16_t hold_tc, int16_t decay_tc, int16_t sustain_cb,
                     int16_t release_tc)
{
    e->level = 0;
    e->target = 0;

    int32_t delay_ticks = smp_timecents_to_ticks(delay_tc);
    if (delay_ticks > 0) {
        e->stage = ENV_DELAY;
        e->timer = delay_ticks;
        e->rate = 0;
        return;
    }

    int32_t atk = smp_timecents_to_ticks(attack_tc);
    if (atk < 1) atk = 1;
    e->stage = ENV_ATTACK;
    e->rate = 0x10000 / atk;
    if (e->rate < 1) e->rate = 1;
    e->target = 0x10000;
    e->timer = 0;

    (void)hold_tc; (void)decay_tc; (void)sustain_cb; (void)release_tc;
}

static void env_advance(env_state_t *e, const ofsf_zone_t *z, int is_vol)
{
    switch (e->stage) {
    case ENV_OFF:
    case ENV_DONE:
        return;

    case ENV_DELAY:
        if (--e->timer <= 0) {
            int16_t atk = is_vol ? z->vol_attack : z->mod_attack;
            int32_t atk_ticks = smp_timecents_to_ticks(atk);
            if (atk_ticks < 1) atk_ticks = 1;
            e->stage = ENV_ATTACK;
            e->rate = 0x10000 / atk_ticks;
            if (e->rate < 1) e->rate = 1;
            e->target = 0x10000;
        }
        return;

    case ENV_ATTACK:
        e->level += e->rate;
        if (e->level >= 0x10000) {
            e->level = 0x10000;
            int16_t hold = is_vol ? z->vol_hold : z->mod_hold;
            int32_t hold_ticks = smp_timecents_to_ticks(hold);
            if (hold_ticks > 0) {
                e->stage = ENV_HOLD;
                e->timer = hold_ticks;
                e->rate = 0;
            } else {
                goto start_decay;
            }
        }
        return;

    case ENV_HOLD:
        if (--e->timer <= 0) {
start_decay: ;
            int16_t decay = is_vol ? z->vol_decay : z->mod_decay;
            int16_t sus   = is_vol ? z->vol_sustain : z->mod_sustain;
            int32_t decay_ticks = smp_timecents_to_ticks(decay);
            if (decay_ticks < 1) decay_ticks = 1;
            int32_t sus_level = compute_sustain_level(sus);
            e->stage = ENV_DECAY;
            e->target = sus_level;
            e->rate = (e->level - sus_level) / decay_ticks;
            if (e->rate < 1) e->rate = 1;
        }
        return;

    case ENV_DECAY:
        e->level -= e->rate;
        if (e->level <= e->target) {
            e->level = e->target;
            e->stage = ENV_SUSTAIN;
            e->rate = 0;
        }
        return;

    case ENV_SUSTAIN:
        return;

    case ENV_RELEASE:
        e->level -= e->rate;
        if (e->level <= ENV_FLOOR) {
            e->level = 0;
            e->stage = ENV_DONE;
        }
        return;
    }
}

static void env_start_release(env_state_t *e, int16_t release_tc)
{
    if (e->stage == ENV_OFF || e->stage == ENV_DONE)
        return;

    int32_t rel_ticks = smp_timecents_to_ticks(release_tc);
    if (rel_ticks < 1) rel_ticks = 1;

    e->stage = ENV_RELEASE;
    e->target = 0;
    e->rate = e->level / rel_ticks;
    if (e->rate < 1) e->rate = 1;
}

/* ------------------------------------------------------------------ */
/* LFO helpers                                                        */
/* ------------------------------------------------------------------ */

static void lfo_init(lfo_state_t *l, int16_t delay_tc, int16_t freq_cents)
{
    l->phase = 0;
    l->delay_ticks = smp_timecents_to_ticks(delay_tc);
    if (l->delay_ticks < 0) l->delay_ticks = 0;

    /* freq_cents is absolute cents (SF2 spec):
     *   frequency (Hz) = 8.176 * 2^(freq_cents / 1200)
     *
     * LFO phase is Q16.16 wrapping at 0x10000 per cycle, advanced at 1 kHz
     * (effective — env_advance / lfo_advance runs twice per 500 Hz outer
     * tick).  So per-advance phase increment = freq * 65536 / 1000.
     *
     * 8.176 Hz in Q16.16 ≈ 535822 = 0x82D0E.  Historical bug: the
     * constant used to be 0x82D1 (33489 ≈ 8.176 * 4096, i.e. Q20.12)
     * which made every LFO run ~16x too slow — a 6 Hz vibrato wobbled
     * at 0.4 Hz and effect-heavy patches sounded like "timing is wrong". */
    uint32_t mult = cents_to_rate_multiplier(freq_cents);
    uint32_t freq_fp16 = (uint32_t)(((uint64_t)535822u * mult) >> 16);
    l->rate = (int32_t)(freq_fp16 / 1000);
    if (l->rate < 1) l->rate = 1;
}

static int32_t lfo_advance(lfo_state_t *l)
{
    if (l->delay_ticks > 0) {
        l->delay_ticks--;
        return 0;
    }
    l->phase += l->rate;
    l->phase &= 0xFFFF;
    return triangle_wave(l->phase);
}

/* ------------------------------------------------------------------ */
/* Voice allocation                                                   */
/* ------------------------------------------------------------------ */

static void voice_force_off(int idx);
static void voice_cleanup_stolen(void);

/* Reclaim a slot for immediate reuse: free the hardware mixer voice and
 * mark the slot inactive.  voice_alloc's steal passes call this so the
 * caller (smp_voice_note_on) can write fresh state without leaking the
 * previous hardware voice. */
static void voice_reclaim(int idx)
{
    smp_voice_t *v = &voices[idx];
    if (v->mixer_voice >= 0)
        of_mixer_stop(v->mixer_voice);
    v->mixer_voice = -1;
    v->active = 0;
}

static int voice_alloc(void)
{
    /* Pass 1: find a free slot */
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (!voices[i].active)
            return i;
    }

    /* Pass 2: steal ENV_DONE (oldest first).  Skip STEAL_PENDING slots —
     * voice_cleanup_stolen owns those and will free them next tick. */
    int best = -1;
    uint32_t best_age = UINT32_MAX;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active != STEAL_PENDING &&
            voices[i].vol_env.stage == ENV_DONE && voices[i].age < best_age) {
            best = i;
            best_age = voices[i].age;
        }
    }
    if (best >= 0) {
        voice_reclaim(best);
        return best;
    }

    /* Pass 3: steal the quietest ENV_RELEASE voice (lowest envelope
     * level).  Stealing by audible-level minimizes the click from the
     * hard stop in voice_reclaim — a voice already near-silent fades
     * to zero with no perceptible discontinuity.  "Oldest by age" was
     * a poor proxy because per-zone release rates differ widely (a
     * drum with 50 ms release started 2 s ago is silent; a piano with
     * 3 s release started 2 s ago is still loud). */
    int32_t best_level = INT32_MAX;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active != STEAL_PENDING &&
            voices[i].vol_env.stage == ENV_RELEASE &&
            voices[i].vol_env.level < best_level) {
            best = i;
            best_level = voices[i].vol_env.level;
        }
    }
    if (best >= 0) {
        voice_reclaim(best);
        return best;
    }

    /* Pass 4: steal the quietest voice of any stage (last resort).
     * Same rationale as pass 3 — steal whatever is least audible. */
    best_level = INT32_MAX;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active != STEAL_PENDING &&
            voices[i].vol_env.level < best_level) {
            best = i;
            best_level = voices[i].vol_env.level;
        }
    }
    if (best >= 0)
        voice_reclaim(best);

    return best;
}

/* Schedule a voice for shutdown without reusing its slot.  Used by
 * kill_exclusive_class — the new note allocates a fresh slot and the
 * old one fades out via voice_cleanup_stolen on the next tick. */
static void voice_force_off(int idx)
{
    smp_voice_t *v = &voices[idx];
    if (v->mixer_voice >= 0) {
        of_mixer_set_vol_lr(v->mixer_voice, 0, 0);
        of_mixer_set_volume_ramp(v->mixer_voice, 4);
    }
    v->active = STEAL_PENDING;
}

static void voice_cleanup_stolen(void)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        if (voices[i].active == STEAL_PENDING) {
            if (voices[i].mixer_voice >= 0)
                of_mixer_stop(voices[i].mixer_voice);
            voices[i].active = 0;
            voices[i].mixer_voice = -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Exclusive class                                                    */
/* ------------------------------------------------------------------ */

static void kill_exclusive_class(int midi_ch, uint8_t excl_class)
{
    if (excl_class == 0) return;
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active && v->active != STEAL_PENDING &&
            v->midi_ch == midi_ch && v->zone &&
            v->zone->exclusive_class == excl_class) {
            voice_force_off(i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Volume / pan / pitch computation                                   */
/* ------------------------------------------------------------------ */

static void compute_vol_lr(smp_voice_t *v, int *out_l, int *out_r)
{
    /* env_vol: Q16.16 -> 0..256 */
    int32_t env_vol = v->vol_env.level >> 8;
    if (env_vol > 255) env_vol = 255;
    if (env_vol < 0)   env_vol = 0;

    /* velocity: 0..127 -> 0..255 (simple scale) */
    int vel_scale = (v->velocity * 2) + 1;
    if (vel_scale > 255) vel_scale = 255;

    /* channel volume: (CC7 * CC11) / 127 -> 0..127 */
    int ch = v->midi_ch;
    int ch_vol_combined = (ch_volume[ch] * ch_expression[ch]) / 127;

    /* initial attenuation: 0..960 centibels -> linear scale 0..255 */
    int attn_scale = 255;
    if (v->zone && v->zone->initial_attn > 0) {
        int cb = v->zone->initial_attn;
        if (cb >= 960) cb = 960;
        attn_scale = (255 * (960 - cb)) / 960;
    }

    /* combined: (env * vel * attn * ch * master) >> shifts -> 0..255 */
    int32_t vol = env_vol;
    vol = (vol * vel_scale) >> 8;
    vol = (vol * attn_scale) >> 8;
    vol = (vol * ch_vol_combined) >> 7;
    vol = (vol * master_vol) >> 8;
    if (vol > 255) vol = 255;
    if (vol < 0)   vol = 0;

    /* Pan: zone pan + channel pan.
     * Zone pan: -500..+500 (SF2 units, -500=full left, +500=full right)
     * Channel CC10: 0..127 (64=center)
     * Combined pan: -500..+500 range */
    int zone_pan = v->zone ? v->zone->pan : 0;
    int midi_pan = ((ch_pan[ch] - 64) * 500) / 63;
    int pan = zone_pan + midi_pan;
    if (pan < -500) pan = -500;
    if (pan > 500)  pan = 500;

    /* Convert pan to L/R scaling.
     * pan -500: L=vol, R=0
     * pan    0: L=vol, R=vol
     * pan +500: L=0,   R=vol */
    if (pan <= 0) {
        *out_l = vol;
        *out_r = (vol * (500 + pan)) / 500;
    } else {
        *out_l = (vol * (500 - pan)) / 500;
        *out_r = vol;
    }
}

/* Recompute and (if changed) write the filter state for a voice.
 *
 * Called at 1 kHz (mid-tick + end-tick) rather than 500 Hz because the HW
 * filter cutoff snaps instantly — each cents-level change produces a small
 * SVF state-variable transient.  Doubling the update rate halves the jump
 * size per write and reduces audible "breakup" on effect-heavy patches
 * (synth leads, pads, brass) where mod_lfo_to_filter or mod_env_to_filter
 * sweep the cutoff continuously.
 *
 * Skip the HW write when the integer-rounded HW cutoff value is unchanged:
 * cents-level jitter often collapses to the same Q0.16 HW number, and a
 * redundant write still perturbs the filter on the HW side. */
static void filter_update(smp_voice_t *v)
{
    const ofsf_zone_t *z = v->zone;
    if (!z) return;
    if (v->mixer_voice < 0) return;
    if (z->initial_fc >= 13500 &&
        z->mod_lfo_to_filter == 0 &&
        z->mod_env_to_filter == 0)
        return;  /* filter bypassed at note-on */

    int32_t fc = z->initial_fc;

    if (z->mod_lfo_to_filter != 0) {
        int32_t lfo_out = triangle_wave(v->mod_lfo.phase);
        fc += (lfo_out * z->mod_lfo_to_filter) >> 16;
    }
    if (z->mod_env_to_filter != 0) {
        fc += ((int64_t)v->mod_env.level * z->mod_env_to_filter) >> 16;
    }
    fc += ((int32_t)ch_brightness[v->midi_ch] - 64) * 75;

    if (fc < 1500)  fc = 1500;
    if (fc > 13500) fc = 13500;

    int16_t fc16 = (int16_t)fc;
    int16_t q16  = z->initial_q + (int16_t)(ch_resonance[v->midi_ch] * 8);
    if (q16 > 960) q16 = 960;

    if (fc16 == v->cur_filter_fc && q16 == v->cur_filter_q)
        return;
    v->cur_filter_fc = fc16;
    v->cur_filter_q  = q16;

    /* Convert cents to Q0.16 normalized frequency for hardware.
     * fc_hz = 8.176 * 2^(fc_cents/1200)
     * cutoff_hw = (fc_hz / 24000) * 65535  (Nyquist = 24 kHz)
     * 8.176 Hz * 65536 / 24000 ≈ 22.36 → use 22 as integer approx */
    uint32_t fc_mult = cents_to_rate_multiplier(fc16);
    uint32_t cutoff_hw = (uint32_t)(((uint64_t)fc_mult * 22) >> 16);
    if (cutoff_hw > 65535) cutoff_hw = 65535;

    /* SVF q is damping (higher = less resonance), SF2 Q is resonance
     * gain (higher = more resonance). Invert. */
    int q_hw = 255 - (q16 * 255 / 960);
    if (q_hw < 8) q_hw = 8;  /* prevent self-oscillation */

    if ((uint16_t)cutoff_hw == v->cur_cutoff_hw)
        return;  /* HW cutoff unchanged — avoid redundant write */

    /* Track the largest single-tick cutoff jump across all voices since
     * the last stats reset.  Big jumps correlate with audible SVF
     * state-variable transients. */
    uint16_t new_hw = (uint16_t)cutoff_hw;
    uint16_t delta  = (new_hw > v->cur_cutoff_hw)
                        ? (uint16_t)(new_hw - v->cur_cutoff_hw)
                        : (uint16_t)(v->cur_cutoff_hw - new_hw);
    if (delta > stat_cutoff_delta_max) stat_cutoff_delta_max = delta;
    v->cur_cutoff_hw = new_hw;

    of_mixer_set_filter(v->mixer_voice, (int)cutoff_hw, q_hw, 1);
    stat_filter_writes++;
}

static uint32_t compute_pitch(smp_voice_t *v)
{
    int ch = v->midi_ch;
    int32_t cents_offset = 0;

    /* Pitch bend */
    cents_offset += ((int32_t)ch_bend[ch] * BEND_RANGE_CENTS) / 8192;

    /* Vibrato LFO */
    if (v->zone && v->zone->vib_lfo_to_pitch != 0) {
        int32_t lfo_out = triangle_wave(v->vib_lfo.phase);
        cents_offset += (lfo_out * v->zone->vib_lfo_to_pitch) >> 16;
    }

    /* Mod LFO to pitch */
    if (v->zone && v->zone->mod_lfo_to_pitch != 0) {
        int32_t lfo_out = triangle_wave(v->mod_lfo.phase);
        int32_t depth = v->zone->mod_lfo_to_pitch;
        /* Scale by CC1 mod wheel */
        depth = (depth * ch_mod_depth[ch]) / 127;
        cents_offset += (lfo_out * depth) >> 16;
    }

    /* Mod envelope to pitch */
    if (v->zone && v->zone->mod_env_to_pitch != 0) {
        cents_offset += ((int64_t)v->mod_env.level * v->zone->mod_env_to_pitch) >> 16;
    }

    if (cents_offset == 0)
        return v->base_rate_fp16;
    if (cents_offset > 12000) cents_offset = 12000;
    if (cents_offset < -12000) cents_offset = -12000;

    uint32_t mult = cents_to_rate_multiplier(cents_offset);
    return (uint32_t)(((uint64_t)v->base_rate_fp16 * mult) >> 16);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void smp_voice_init(void)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        voices[i].active = 0;
        voices[i].mixer_voice = -1;
    }

    for (int i = 0; i < 16; i++) {
        ch_volume[i]     = 100;
        ch_expression[i] = 127;
        ch_pan[i]        = 64;
        ch_bend[i]       = 0;
        ch_mod_depth[i]  = 0;
        ch_sustain[i]    = 0;
        ch_brightness[i] = 64;
        ch_resonance[i]  = 0;
    }

    master_vol = 255;
    tick_counter = 0;
}

int smp_voice_note_on(const ofsf_zone_t *zone, int midi_ch, int note,
                      int velocity, const void *sample_base)
{
    if (!zone || midi_ch < 0 || midi_ch > 15 || velocity <= 0)
        return -1;

    kill_exclusive_class(midi_ch, zone->exclusive_class);

    int idx = voice_alloc();
    if (idx < 0)
        return -1;

    smp_voice_t *v = &voices[idx];

    v->active = 1;
    v->zone = zone;
    v->midi_ch = (uint8_t)midi_ch;
    v->note = (uint8_t)note;
    v->velocity = (uint8_t)velocity;
    v->sustain_held = 0;
    v->mixer_voice = -1;
    v->age = tick_counter;

    /* Compute base playback rate:
     * rate = (sample_rate / 48000) * 2^((note - root + coarse)*100 + fine) / 1200)
     * We split into the sample_rate ratio and the pitch offset. */
    const ofsf_header_t *hdr = of_smp_bank_get();
    uint32_t sr = hdr ? hdr->sample_rate : 44100;
    uint32_t base_fp16 = OF_MIXER_RATE_FP16(sr);

    int32_t total_cents = ((int32_t)note - (int32_t)zone->root_key) * 100
                        + (int32_t)zone->coarse_tune * 100
                        + (int32_t)zone->fine_tune;
    uint32_t pitch_mult = cents_to_rate_multiplier(total_cents);
    v->base_rate_fp16 = (uint32_t)(((uint64_t)base_fp16 * pitch_mult) >> 16);

    /* Compute sample address.
     * sample_base points to start of sample blob in CRAM1.
     * sample_offset is bytes from blob start.
     * CRAM1 uses word addressing but samples are 16-bit, so
     * the word address = base + offset/2. */
    const uint8_t *sample_ptr = (const uint8_t *)sample_base
                              + zone->sample_offset;

    int mhv = of_mixer_play(sample_ptr, zone->sample_length, sr, 0, 200);
    if (mhv < 0) { v->active = 0; return -1; }
    v->mixer_voice = mhv;
    of_mixer_set_rate_raw(mhv, v->base_rate_fp16);
    stat_rate_writes++;

    /* Loop setup */
    if (zone->loop_mode == OFSF_LOOP_FORWARD || zone->loop_mode == OFSF_LOOP_BIDI) {
        of_mixer_set_loop(mhv, zone->loop_start, zone->loop_end);
        if (zone->loop_mode == OFSF_LOOP_BIDI)
            of_mixer_set_bidi(mhv, 1);
        /* Looping voice: let the envelope decide when it ends. */
        v->sample_ticks_remaining = 0;
    } else {
        /* One-shot: compute how many software ticks (500 Hz) until the
         * sample walks off its natural end.
         *
         *   samples consumed per 48 kHz output tick = base_rate_fp16 / 65536
         *   seconds to finish = sample_length * 65536 / (base_rate_fp16 * 48000)
         *   SW ticks (500 Hz) = sample_length * 65536 / (base_rate_fp16 * 96)
         *
         * Adds 20 % headroom so modest pitch bends (drums: typically none)
         * don't truncate audible content. */
        if (v->base_rate_fp16 > 0 && zone->sample_length > 0) {
            uint64_t num = (uint64_t)zone->sample_length * 65536u * 6u;
            uint64_t den = (uint64_t)v->base_rate_fp16 * 96u * 5u;
            uint64_t ticks = num / (den ? den : 1);
            if (ticks < 1) ticks = 1;
            if (ticks > 0x7FFFFFFFu) ticks = 0x7FFFFFFFu;
            v->sample_ticks_remaining = (int32_t)ticks;
        } else {
            v->sample_ticks_remaining = 0;
        }
    }

    of_mixer_set_group(mhv, OF_MIXER_GROUP_MUSIC);

    /* Initialize envelopes */
    env_init(&v->vol_env,
             zone->vol_delay, zone->vol_attack, zone->vol_hold,
             zone->vol_decay, zone->vol_sustain, zone->vol_release);

    env_init(&v->mod_env,
             zone->mod_delay, zone->mod_attack, zone->mod_hold,
             zone->mod_decay, zone->mod_sustain, zone->mod_release);

    /* Initialize LFOs */
    lfo_init(&v->mod_lfo, zone->mod_lfo_delay, zone->mod_lfo_freq);
    lfo_init(&v->vib_lfo, zone->vib_lfo_delay, zone->vib_lfo_freq);

    /* Initial filter state.
     *
     * CRITICAL: of_mixer_play does NOT reset the per-voice filter
     * registers (FILTER_FC / FILTER_Q), so a reused voice slot inherits
     * whatever cutoff/Q/enable the previous note left behind.  Without
     * an explicit write here, a new zone with no filter requirement
     * would play through a stale low-pass filter from an unrelated
     * sample, producing audibly wrong timbres on stolen voices.
     *
     * Program the filter explicitly:
     *   - If the zone needs filtering (cutoff below wide-open, or any
     *     modulation source), enable with the zone's initial cutoff/Q
     *     converted to the hardware Q0.16 / 0..255 representation —
     *     same math as the per-tick filter update below.
     *   - Otherwise disable the filter so the voice runs unfiltered. */
    v->cur_filter_fc = zone->initial_fc;
    v->cur_filter_q  = zone->initial_q;
    {
        int need_filter = (zone->initial_fc < 13500) ||
                          (zone->mod_lfo_to_filter != 0) ||
                          (zone->mod_env_to_filter != 0);
        if (need_filter) {
            int32_t fc = zone->initial_fc;
            if (fc < 1500)  fc = 1500;
            if (fc > 13500) fc = 13500;
            int16_t q16 = zone->initial_q + (int16_t)(ch_resonance[midi_ch] * 8);
            if (q16 > 960) q16 = 960;
            uint32_t fc_mult = cents_to_rate_multiplier(fc);
            uint32_t cutoff_hw = (uint32_t)(((uint64_t)fc_mult * 22) >> 16);
            if (cutoff_hw > 65535) cutoff_hw = 65535;
            int q_hw = 255 - (q16 * 255 / 960);
            if (q_hw < 8) q_hw = 8;
            of_mixer_set_filter(mhv, (int)cutoff_hw, q_hw, 1);
            stat_filter_writes++;
            /* Cache the fc we actually wrote so tick() only rewrites on
             * a real change. */
            v->cur_filter_fc = (int16_t)fc;
            v->cur_filter_q  = q16;
            v->cur_cutoff_hw = (uint16_t)cutoff_hw;
        } else {
            /* Bypass the filter — enable=0 ensures a prior voice's
             * cutoff doesn't bleed into this one. */
            of_mixer_set_filter(mhv, 65535, 8, 0);
            stat_filter_writes++;
            v->cur_cutoff_hw = 65535;
        }
    }

    /* Advance the envelope one tick so the level is non-zero before
     * the ISR runs — otherwise the ISR writes volume 0 immediately. */
    env_advance(&v->vol_env, zone, 1);

    int vl, vr;
    compute_vol_lr(v, &vl, &vr);
    of_mixer_set_vol_lr(mhv, vl, vr);
    stat_vol_writes++;
    prev_vol_l[idx] = vl;
    prev_vol_r[idx] = vr;
    prev_rate[idx]  = v->base_rate_fp16;

    return idx;
}

void smp_voice_note_off(int midi_ch, int note)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (!v->active || v->active == STEAL_PENDING)
            continue;
        if (v->midi_ch != midi_ch || v->note != note)
            continue;

        if (ch_sustain[midi_ch]) {
            v->sustain_held = 1;
        } else {
            env_start_release(&v->vol_env, v->zone->vol_release);
            env_start_release(&v->mod_env, v->zone->mod_release);
        }
    }
}

void smp_voice_tick(void)
{
    uint32_t _probe_t0 = OF_SVC->timer_get_us();
    uint8_t  _probe_active = 0;
    uint8_t  _probe_sustain = 0;
    uint8_t  _probe_release = 0;
    uint8_t  _probe_decay = 0;
    uint8_t  _probe_held = 0;
    uint8_t  _probe_ch[16] = {0};

    tick_counter++;
    voice_cleanup_stolen();

    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (!v->active || v->active == STEAL_PENDING)
            continue;
        _probe_active++;
        if (v->vol_env.stage == ENV_SUSTAIN) _probe_sustain++;
        else if (v->vol_env.stage == ENV_RELEASE) _probe_release++;
        else if (v->vol_env.stage == ENV_DECAY) _probe_decay++;
        if (v->sustain_held) _probe_held++;
        if (v->midi_ch < 16) _probe_ch[v->midi_ch]++;

        const ofsf_zone_t *z = v->zone;

        /* Natural sample-end check for one-shots.  When the sample has
         * played to its end the mixer is already emitting silence, so we
         * can force-DONE without any audible click and reclaim the slot
         * immediately — otherwise the envelope's long SUSTAIN parks the
         * voice (especially SF2 drum zones with very long vol_sustain)
         * and fills all 28 soft voices during dense drum tracks. */
        if (v->sample_ticks_remaining > 0) {
            if (--v->sample_ticks_remaining == 0) {
                v->vol_env.stage = ENV_DONE;
                v->vol_env.level = 0;
            }
        }

        /* Envelopes and LFOs advance twice per outer tick (effective
         * 1 kHz), but we must also sample pitch at 1 kHz — writing RATE
         * only once per outer tick produces an audible 2 ms staircase
         * on fast pitch modulation (short mod_env_to_pitch sweeps, fast
         * vibratos, snappy pitch bends).  HW RATE snaps (no ramp), so
         * we interleave a mid-tick compute_pitch between the two halves
         * of the advance sequence.  Volume is handled only at the end
         * because the mixer already ramps between VOL writes. */
        env_advance(&v->vol_env, z, 1);
        env_advance(&v->mod_env, z, 0);
        lfo_advance(&v->mod_lfo);
        lfo_advance(&v->vib_lfo);

        /* Mid-tick pitch update (1 kHz sampling) */
        if (v->mixer_voice >= 0 && v->vol_env.stage != ENV_DONE) {
            uint32_t rate_mid = compute_pitch(v);
            if (rate_mid != prev_rate[i]) {
                of_mixer_set_rate_raw(v->mixer_voice, rate_mid);
                prev_rate[i] = rate_mid;
                stat_rate_writes++;
            }
            /* Mid-tick filter update — run at 1 kHz (matches pitch) so
             * cents-level cutoff sweeps from mod_lfo_to_filter /
             * mod_env_to_filter change in smaller steps and produce
             * smaller SVF state-variable transients. */
            filter_update(v);
        }

        env_advance(&v->vol_env, z, 1);
        env_advance(&v->mod_env, z, 0);
        lfo_advance(&v->mod_lfo);
        lfo_advance(&v->vib_lfo);

        if (v->vol_env.stage == ENV_DONE) {
            if (v->mixer_voice >= 0)
                of_mixer_stop(v->mixer_voice);
            v->active = 0;
            v->mixer_voice = -1;
            continue;
        }

        int vl, vr;
        compute_vol_lr(v, &vl, &vr);
        uint32_t rate = compute_pitch(v);
        if (vl != prev_vol_l[i] || vr != prev_vol_r[i] ||
            rate != prev_rate[i]) {
            of_mixer_set_voice_raw(v->mixer_voice, rate, vl, vr);
            /* set_voice_raw coalesces rate + vol; count each independently
             * changed field so the stats reflect the underlying load. */
            if (rate != prev_rate[i]) stat_rate_writes++;
            if (vl != prev_vol_l[i] || vr != prev_vol_r[i]) stat_vol_writes++;
            prev_vol_l[i] = vl;
            prev_vol_r[i] = vr;
            prev_rate[i] = rate;
        }

        /* End-tick filter update (second half of the 1 kHz sampling). */
        filter_update(v);
    }

    uint32_t _probe_dt = OF_SVC->timer_get_us() - _probe_t0;
    tick_us_last = _probe_dt;
    if (_probe_dt > tick_us_max) tick_us_max = _probe_dt;
    if (_probe_dt > SMP_TICK_SPIKE_US) tick_spike_count++;
    if (_probe_active > tick_active_peak) tick_active_peak = _probe_active;
    tick_stage_sustain = _probe_sustain;
    tick_stage_release = _probe_release;
    tick_stage_decay   = _probe_decay;
    tick_sustain_held  = _probe_held;
    for (int i = 0; i < 16; i++)
        tick_ch_active[i] = _probe_ch[i];
    tick_stat_count++;
}

void smp_voice_update_volume(int midi_ch, int volume, int expression)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_volume[midi_ch]     = volume;
    ch_expression[midi_ch] = expression;
}

void smp_voice_update_pan(int midi_ch, int pan)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_pan[midi_ch] = pan;
}

void smp_voice_update_bend(int midi_ch, int bend)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_bend[midi_ch] = bend;
}

void smp_voice_update_mod(int midi_ch, int mod_depth)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_mod_depth[midi_ch] = mod_depth;
}

void smp_voice_update_sustain(int midi_ch, int sustain_on)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_sustain[midi_ch] = sustain_on;

    if (!sustain_on) {
        for (int i = 0; i < SMP_MAX_VOICES; i++) {
            smp_voice_t *v = &voices[i];
            if (v->active && v->active != STEAL_PENDING &&
                v->midi_ch == midi_ch && v->sustain_held) {
                v->sustain_held = 0;
                env_start_release(&v->vol_env, v->zone->vol_release);
                env_start_release(&v->mod_env, v->zone->mod_release);
            }
        }
    }
}

void smp_voice_update_filter(int midi_ch, int brightness, int resonance)
{
    if (midi_ch < 0 || midi_ch > 15) return;
    ch_brightness[midi_ch] = brightness;
    ch_resonance[midi_ch]  = resonance;
}

void smp_voice_all_off(int midi_ch)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active && v->active != STEAL_PENDING && v->midi_ch == midi_ch) {
            if (v->mixer_voice >= 0)
                of_mixer_stop(v->mixer_voice);
            v->active = 0;
            v->mixer_voice = -1;
        }
    }
}

void smp_voice_all_off_global(void)
{
    for (int i = 0; i < SMP_MAX_VOICES; i++) {
        smp_voice_t *v = &voices[i];
        if (v->active) {
            if (v->mixer_voice >= 0)
                of_mixer_stop(v->mixer_voice);
            v->active = 0;
            v->mixer_voice = -1;
        }
    }
}

void smp_voice_set_master_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 255) vol = 255;
    master_vol = vol;
}
