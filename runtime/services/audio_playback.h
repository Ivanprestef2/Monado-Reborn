// [XCDE] Change SDL playback streams without dropping a working device on failure.
#pragma once
#include <SDL3/SDL_audio.h>

static inline bool xcde_audio_reconfigure(SDL_AudioStream **stream, SDL_AudioDeviceID *active_device,
                                          SDL_AudioDeviceID device, const SDL_AudioSpec *spec) {
    if (*stream && *active_device == device) return SDL_SetAudioStreamFormat(*stream, spec, NULL);

    SDL_AudioStream *replacement = SDL_OpenAudioDeviceStream(device, spec, NULL, NULL);
    if (!replacement) return false;
    if (!SDL_ResumeAudioStreamDevice(replacement)) {
        SDL_DestroyAudioStream(replacement);
        return false;
    }
    if (*stream) SDL_DestroyAudioStream(*stream);
    *stream = replacement;
    *active_device = device;
    return true;
}
