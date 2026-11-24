#pragma once

#include <stdbool.h>

#include "therapy_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Voice guidance through the board speaker (ES8311 -> NS4150B). The PCM clips are generated offline by
 * tools/make_voice_clips.py and embedded in flash. Volume and mute persist in NVS and are exposed in the
 * Settings page. If the speaker does not initialise, every play call returns false and the caller falls
 * back to the vibration motor.
 */

void therapy_audio_init(void);
bool therapy_audio_ready(void);

int  therapy_audio_volume(void);
void therapy_audio_set_volume(int percent);
bool therapy_audio_muted(void);
void therapy_audio_set_muted(bool mute);
void therapy_audio_save(void);

/* Queue one clip. Returns false if the speaker is unavailable or the queue is full. */
bool therapy_audio_play(therapy_clip_t clip);

/* "Rep N of M" clip (N 1..20). */
bool therapy_audio_play_rep(int rep);

#ifdef __cplusplus
}
#endif
