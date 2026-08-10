/*
 * midi_of.c -- MIDI music for Duke3D on openfpgaOS
 *
 * Bridges Duke3D's MUSIC_* interface to the openfpgaOS of_midi API.
 * MIDI files are loaded from the GRP via the BUILD engine file system
 * and rendered through the SoundFont sample-voice engine (the .ofsf
 * bank the kernel loaded at boot).
 */

#include <stdint.h>
#include <string.h>
#include "of_midi.h"
#include "of_smp_voice.h"
#include "../audiolib/music.h"

/* GRP file system functions (BUILD engine) */
extern short TCkopen4load(const char *fn, char searchfirst);
extern int32_t kfilelength(short handle);
extern void kread(short handle, void *buf, int32_t len);
extern void kclose(short handle);

/* ---- state --------------------------------------------------------- */

static int  midi_initialized = 0;
static int  midi_volume = 255;       /* 0-255, game-requested volume */
/* Duke3D's original DOS behaviour: per-level MIDI loops continuously
 * within the level; MUSIC_StopSong only fires on level transitions
 * (premap.c::enterlevel).  The audiolib-era default was to loop, but
 * this port's stubbed-out audiolib never calls MUSIC_SetLoopFlag, so
 * `midi_looping` stayed at 0 and tracks died at end-of-file.  Default
 * to 1 so the soundtrack is continuous; if anything ever wires up
 * MUSIC_SetLoopFlag(0) (e.g. for the menu jingle), that override
 * still takes effect. */
static int  midi_looping = 1;

/* Buffer for loaded MIDI data (largest Duke3D MIDI is ~60KB) */
#define MIDI_BUF_SIZE (64 * 1024)
static uint8_t midi_buffer[MIDI_BUF_SIZE];
static uint32_t midi_buffer_len = 0;

/* TMB override disabled: of_midi now uses a 4-op WOPL bank (25-byte
 * records) where Duke3D's 2-op TMB timbres don't fit. Keeping the
 * bank empty lets the engine fall back on the built-in 4-op GM patches,
 * which generally sound richer than the DMX-era TMB anyway. */

/* ---- error string -------------------------------------------------- */

char *MUSIC_ErrorString(int ErrorNumber)
{
    (void)ErrorNumber;
    return "of_midi";
}

/* ---- init / shutdown ----------------------------------------------- */

int MUSIC_Init(int SoundCard, int Address)
{
    (void)SoundCard;
    (void)Address;

    if (!midi_initialized) {
        of_midi_init();
        /* The SDK voice engine is software-only now (the AWE coprocessor
         * backend was removed), so the old smp_voice_enable_awe_backend(0)
         * opt-out is gone with it — the SW path this port required is the
         * only path.
         *
         * Reverb stays at the FPGA reset default (0 = bypass): dense
         * sustained music sums past the mixer accumulator headroom and
         * clips hard with non-zero wet.  Keep dry until the mixer learns
         * compression. */
        midi_initialized = 1;
    }
    return MUSIC_Ok;
}

int MUSIC_Shutdown(void)
{
    if (midi_initialized) {
        of_midi_stop();
        midi_initialized = 0;
    }
    return MUSIC_Ok;
}

/* ---- volume -------------------------------------------------------- */

void MUSIC_SetMaxFMMidiChannel(int channel)
{
    (void)channel;
}

void MUSIC_SetVolume(int volume)
{
    /* Duke3D volume is 0-255 — passed through unchanged.  With AWE
     * off and reverb at 0, the dry mix has enough headroom that the
     * engine's default ÷2 in master_volume shouldn't clip the mixer
     * accumulator on Duke3D's soundtrack. */
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    midi_volume = volume;
    if (midi_initialized)
        of_midi_set_volume(volume);
}

void MUSIC_SetMidiChannelVolume(int channel, int volume)
{
    (void)channel;
    (void)volume;
}

void MUSIC_ResetMidiChannelVolumes(void)
{
}

int MUSIC_GetVolume(void)
{
    return midi_volume;
}

/* ---- playback control ---------------------------------------------- */

void MUSIC_SetLoopFlag(int loopflag)
{
    midi_looping = loopflag;
}

int MUSIC_SongPlaying(void)
{
    return midi_initialized ? of_midi_playing() : 0;
}

void MUSIC_Continue(void)
{
    if (midi_initialized)
        of_midi_resume();
}

void MUSIC_Pause(void)
{
    if (midi_initialized)
        of_midi_pause();
}

int MUSIC_StopSong(void)
{
    if (midi_initialized)
        of_midi_stop();
    return MUSIC_Ok;
}

int MUSIC_PlaySong(char *songData, int loopflag)
{
    if (!midi_initialized)
        return MUSIC_Error;

    of_midi_stop();
    of_midi_set_volume(midi_volume);

    int rc = of_midi_play((const uint8_t *)songData, midi_buffer_len, loopflag);
    if (rc != OF_MIDI_OK)
        return MUSIC_Error;

    return MUSIC_Ok;
}

/* ---- position / context -------------------------------------------- */

void MUSIC_SetContext(int context)
{
    (void)context;
}

int MUSIC_GetContext(void)
{
    return 0;
}

void MUSIC_SetSongTick(uint32_t PositionInTicks)
{
    (void)PositionInTicks;
}

void MUSIC_SetSongTime(uint32_t milliseconds)
{
    (void)milliseconds;
}

void MUSIC_SetSongPosition(int measure, int beat, int tick)
{
    (void)measure;
    (void)beat;
    (void)tick;
}

void MUSIC_GetSongPosition(songposition *pos)
{
    (void)pos;
}

void MUSIC_GetSongLength(songposition *pos)
{
    (void)pos;
}

/* ---- fade ---------------------------------------------------------- */

int MUSIC_FadeVolume(int tovolume, int milliseconds)
{
    (void)milliseconds;
    MUSIC_SetVolume(tovolume);
    return MUSIC_Ok;
}

int MUSIC_FadeActive(void)
{
    return 0;
}

void MUSIC_StopFade(void)
{
}

/* ---- misc ---------------------------------------------------------- */

void MUSIC_RerouteMidiChannel(int channel,
    int (*function)(int event, int c1, int c2))
{
    (void)channel;
    (void)function;
}

/* DMX/TMB timbre banks were an OPL3-era hook: d3dtimbr.tmb supplied
 * 2-op FM patches that of_midi overlaid onto the built-in GM set.  The
 * SDK's MIDI engine is now SoundFont-based — it renders notes through
 * the sample-voice path against the .ofsf loaded by the kernel at boot —
 * so there is no longer a concept of "override one program with an FM
 * timbre".  The game still calls this during startup; silently ignore
 * it.  Change your .ofsf if you want different instruments. */
void MUSIC_RegisterTimbreBank(uint8_t *timbres, unsigned int size)
{
    (void)timbres;
    (void)size;
}

/* ---- PlayMusic: load MIDI from GRP and play ------------------------ */

void PlayMusic(char *fileName)
{
    if (!midi_initialized)
        return;

    /* Stop any currently playing song. The TMB override path used to
     * re-apply a custom bank here, but of_midi now uses the 4-op GM
     * bank by default (see MUSIC_RegisterTimbreBank). */
    of_midi_stop();

    /* Load MIDI file from GRP */
    short fp = TCkopen4load(fileName, 0);
    if (fp == -1)
        return;

    int32_t len = kfilelength(fp);
    if (len <= 0 || (uint32_t)len > MIDI_BUF_SIZE) {
        kclose(fp);
        return;
    }

    kread(fp, midi_buffer, len);
    kclose(fp);
    midi_buffer_len = (uint32_t)len;

    /* Validate MThd header */
    if (len < 14 || midi_buffer[0] != 'M' || midi_buffer[1] != 'T' ||
        midi_buffer[2] != 'h' || midi_buffer[3] != 'd') {
        midi_buffer_len = 0;
        return;
    }

    of_midi_set_volume(midi_volume);
    of_midi_play(midi_buffer, midi_buffer_len, midi_looping);
}
