/*
 * midi_of.c -- MIDI stub for openfpgaOS
 *
 * openfpgaOS has no general-purpose MIDI synthesizer, so all
 * MUSIC_* entry points are stubbed to no-ops.  The OPM (YM2151) could
 * theoretically be driven for music, but that is out of scope here.
 *
 * This file replaces sdl_midi.c for the openfpgaOS target.
 */

#include <stdint.h>
#include "../audiolib/music.h"

/* ---- error string -------------------------------------------------- */

char *MUSIC_ErrorString(int ErrorNumber)
{
    (void)ErrorNumber;
    return "openfpgaOS: no MIDI support.";
}

/* ---- init / shutdown ----------------------------------------------- */

int MUSIC_Init(int SoundCard, int Address)
{
    (void)SoundCard;
    (void)Address;
    return MUSIC_Ok;
}

int MUSIC_Shutdown(void)
{
    return MUSIC_Ok;
}

/* ---- volume -------------------------------------------------------- */

void MUSIC_SetMaxFMMidiChannel(int channel)
{
    (void)channel;
}

void MUSIC_SetVolume(int volume)
{
    (void)volume;
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
    return 0;
}

/* ---- playback control ---------------------------------------------- */

void MUSIC_SetLoopFlag(int loopflag)
{
    (void)loopflag;
}

int MUSIC_SongPlaying(void)
{
    return 0;
}

void MUSIC_Continue(void)
{
}

void MUSIC_Pause(void)
{
}

int MUSIC_StopSong(void)
{
    return MUSIC_Ok;
}

int MUSIC_PlaySong(char *songData, int loopflag)
{
    (void)songData;
    (void)loopflag;
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
    (void)tovolume;
    (void)milliseconds;
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

void MUSIC_RegisterTimbreBank(uint8_t *timbres)
{
    (void)timbres;
}

/* ---- convenience wrapper called from the Game module --------------- */

void PlayMusic(char *fileName)
{
    (void)fileName;
    /* no-op on openfpgaOS */
}
