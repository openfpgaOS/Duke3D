/*
 * pocket_audio.h — Minimal sound mixer for Duke3D on openfpgaOS
 */

#ifndef POCKET_AUDIO_H
#define POCKET_AUDIO_H

void pocket_audio_init(void);
int  pocket_sound_play(int sound_num, int priority, int volume);
void pocket_sound_stop_all(void);
void pocket_audio_pump(void);

#endif
