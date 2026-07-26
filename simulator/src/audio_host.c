/**
 * Audio output, routed to the workstation's sound device through SDL.
 *
 * The device plays .snd files: headerless PCM, signed 16-bit little-endian,
 * mono, 44100 Hz, produced from the .wav sources by scripts/audio.py. There is
 * no header to parse and SDL takes exactly that format, so playing one is a
 * read and a queue.
 *
 * Mixing is what SDL_QueueAudio gives us, which is none: a second sound
 * started while one is playing is appended rather than mixed. The device's
 * codec does the same thing, so a UI that talks over itself sounds wrong here
 * in the same way it would there.
 *
 * The device is opened lazily and left open. Opening it costs a few hundred
 * milliseconds on macOS, which would be audible as a gap before the first
 * sound if it happened per file.
 */
#include "audio_host.h"

#include "storage_host.h"

#include <furi.h>

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>

#define TAG "AudioHost"

#define AUDIO_HOST_SAMPLE_RATE (44100)
#define AUDIO_HOST_CHANNELS    (1)

/* Roughly 12 ms at the sample rate above: small enough that stopping playback
 * is not audibly late, large enough not to starve. */
#define AUDIO_HOST_BUFFER_SAMPLES (512)

static struct {
    SDL_AudioDeviceID device;
    bool unavailable;
    float volume;
} audio_host = {
    .volume = 1.0f,
};

static bool audio_host_open(void) {
    if(audio_host.device) return true;
    if(audio_host.unavailable) return false;

    if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        FURI_LOG_W(TAG, "no audio subsystem: %s", SDL_GetError());
        audio_host.unavailable = true;
        return false;
    }

    SDL_AudioSpec want = {
        .freq = AUDIO_HOST_SAMPLE_RATE,
        .format = AUDIO_S16LSB,
        .channels = AUDIO_HOST_CHANNELS,
        .samples = AUDIO_HOST_BUFFER_SAMPLES,
    };
    SDL_AudioSpec have;

    audio_host.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if(!audio_host.device) {
        FURI_LOG_W(TAG, "no audio device: %s", SDL_GetError());
        audio_host.unavailable = true;
        return false;
    }

    SDL_PauseAudioDevice(audio_host.device, 0);
    return true;
}

void audio_host_set_volume(float volume) {
    audio_host.volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}

bool audio_host_play_file(const char* device_path) {
    furi_check(device_path);

    if(!audio_host_open()) return false;

    char resolved[1024];
    if(!storage_host_resolve_path(device_path, resolved, sizeof(resolved))) {
        FURI_LOG_W(TAG, "audio asset not found: %s", device_path);
        return false;
    }

    FILE* stream = fopen(resolved, "rb");
    if(!stream) {
        FURI_LOG_W(TAG, "no such sound: %s", resolved);
        return false;
    }

    fseek(stream, 0, SEEK_END);
    const long size = ftell(stream);
    fseek(stream, 0, SEEK_SET);

    if(size <= 0) {
        fclose(stream);
        FURI_LOG_W(TAG, "empty sound: %s", resolved);
        return false;
    }

    int16_t* samples = malloc((size_t)size);
    if(!samples) {
        fclose(stream);
        return false;
    }

    const size_t read = fread(samples, 1, (size_t)size, stream);
    fclose(stream);

    if(read != (size_t)size) {
        free(samples);
        FURI_LOG_W(TAG, "short read: %s", resolved);
        return false;
    }

    /* Scale in place rather than with SDL_MixAudioFormat, which mixes against
     * a silent buffer at a fixed volume and would clip on the way. */
    if(audio_host.volume < 1.0f) {
        const size_t count = read / sizeof(int16_t);
        for(size_t i = 0; i < count; i++) {
            samples[i] = (int16_t)((float)samples[i] * audio_host.volume);
        }
    }

    const bool queued = SDL_QueueAudio(audio_host.device, samples, (Uint32)read) == 0;
    if(!queued) FURI_LOG_W(TAG, "queue failed: %s", SDL_GetError());

    free(samples);

    FURI_LOG_D(TAG, "playing %s (%ld bytes)", device_path, size);
    return queued;
}

void audio_host_stop(void) {
    if(audio_host.device) SDL_ClearQueuedAudio(audio_host.device);
}
