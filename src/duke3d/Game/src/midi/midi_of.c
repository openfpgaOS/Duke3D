/*
 * midi_of.c -- MIDI music for Duke3D on openfpgaOS
 *
 * Bridges Duke3D's MUSIC_* interface to the openfpgaOS of_midi API.
 * MIDI files are loaded from the GRP via the BUILD engine file system
 * and played through the hardware OPL3 (YMF262) FM synthesizer.
 */

#include <stdint.h>
#include <string.h>
#include "of_midi.h"
#include "../audiolib/music.h"

/* GRP file system functions (BUILD engine) */
extern short TCkopen4load(const char *fn, char searchfirst);
extern int32_t kfilelength(short handle);
extern void kread(short handle, void *buf, int32_t len);
extern void kclose(short handle);

/* ---- state --------------------------------------------------------- */

static int  midi_initialized = 0;
static int  midi_volume = 255;       /* 0-255, maps to of_midi 0-255 */
static int  midi_looping = 0;

/* Buffer for loaded MIDI data (largest Duke3D MIDI is ~60KB) */
#define MIDI_BUF_SIZE (64 * 1024)
static uint8_t midi_buffer[MIDI_BUF_SIZE];
static uint32_t midi_buffer_len = 0;

/* Converted instrument bank: 175 instruments x 11 bytes = 1925 bytes.
 * Built from Duke3D's TMB format (13-byte sparse records) by
 * scattering patches into the sequential layout of_midi expects. */
static uint8_t converted_bank[175 * OF_MIDI_INST_SIZE];

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
    /* Duke3D volume is 0-255 */
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

/* Parse Duke3D TMB format and convert to of_midi bank format.
 *
 * Duke3D TMB: variable number of 13-byte records:
 *   [instrument_number (1)] [OPL data (11)] [note_offset (1)]
 * The instrument number selects which slot to overwrite (0-174).
 *
 * of_midi bank: 175 sequential instruments, 11 bytes each:
 *   128 melodic (GM program 0-127) + 47 percussion (notes 35-81).
 */
void MUSIC_RegisterTimbreBank(uint8_t *timbres)
{
    if (!timbres || !midi_initialized)
        return;

    /* Start from built-in GM bank, overlay Duke3D TMB patches */
    memcpy(converted_bank, of_midi_builtin_bank(), 175 * OF_MIDI_INST_SIZE);

    /* TMB format (bytes 1-11 of each 13-byte record):
     *   [0] Mod Char, [1] Car Char, [2] Mod KSL, [3] Car KSL,
     *   [4] Mod AD,   [5] Car AD,   [6] Mod SR,  [7] Car SR,
     *   [8] Mod WS,   [9] Car WS,   [10] FB/CNT
     *
     * of_midi bank format (11 bytes per instrument):
     *   [0] FB/CNT,
     *   [1] Mod Char, [2] Mod KSL, [3] Mod AD, [4] Mod SR, [5] Mod WS,
     *   [6] Car Char, [7] Car KSL, [8] Car AD, [9] Car SR, [10] Car WS */
    int tmb_count = 0;
    uint8_t *p = timbres;
    uint8_t *end = timbres + 8000;
    while (p + 13 <= end) {
        int inst = p[0];
        if (inst >= 175) break;
        const uint8_t *t = &p[1];
        uint8_t *d = &converted_bank[inst * OF_MIDI_INST_SIZE];
        /* TMB per-operator: Char(0x20), AD(0x60), SR(0x80), WS(0xE0), KSL_TL(0x40)
         * of_midi per-operator: Char(0x20), KSL_TL(0x40), AD(0x60), SR(0x80), WS(0xE0)
         * TMB: [ModChar ModAD ModSR ModWS ModKSL] [CarChar CarAD CarSR CarWS CarKSL] FB
         * Mask WS to 3 bits — TMB has dirty upper bits real OPL ignores. */
        d[0]  = t[10];          /* FB/CNT */
        d[1]  = t[0];           /* Mod Char */
        d[2]  = t[4];           /* Mod KSL_TL */
        d[3]  = t[1];           /* Mod AD */
        d[4]  = t[2];           /* Mod SR */
        d[5]  = t[3] & 0x07;   /* Mod WS */
        d[6]  = t[5];           /* Car Char */
        d[7]  = t[9];           /* Car KSL_TL */
        d[8]  = t[6];           /* Car AD */
        d[9]  = t[7];           /* Car SR */
        d[10] = t[8] & 0x07;   /* Car WS */
        if (tmb_count < 5)
            printf("[bank] #%d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   inst, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10]);
        tmb_count++;
        p += 13;
    }

    of_midi_load_bank(converted_bank);
}

/* ---- PlayMusic: load MIDI from GRP and play ------------------------ */

void PlayMusic(char *fileName)
{
    if (!midi_initialized)
        return;

    /* Stop any currently playing song and re-apply TMB bank
     * (of_midi_stop/play may reset to built-in bank) */
    of_midi_stop();
    if (converted_bank[0] != 0)
        of_midi_load_bank(converted_bank);

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
