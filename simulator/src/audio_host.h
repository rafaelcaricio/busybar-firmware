#pragma once

#include <stdbool.h>

/** Play a device-path .snd file on the workstation's audio device. */
bool audio_host_play_file(const char* device_path);

/** Scale subsequent playback, 0.0 to 1.0. */
void audio_host_set_volume(float volume);

/** Drop anything still queued. */
void audio_host_stop(void);
