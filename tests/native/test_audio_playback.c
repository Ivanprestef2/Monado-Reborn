#include <assert.h>
#include <stdio.h>
#include <SDL3/SDL_audio.h>

struct SDL_AudioStream { bool destroyed; };

static SDL_AudioStream streams[8];
static unsigned next_stream;
static bool fail_open, fail_resume;

static SDL_AudioStream *open_stream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
                                    SDL_AudioStreamCallback callback, void *user) {
    (void)device; (void)spec; (void)callback; (void)user;
    return fail_open ? NULL : &streams[next_stream++];
}

static bool resume_stream(SDL_AudioStream *stream) { (void)stream; return !fail_resume; }
static void destroy_stream(SDL_AudioStream *stream) { stream->destroyed = true; }

static bool set_format(SDL_AudioStream *stream, const SDL_AudioSpec *source, const SDL_AudioSpec *dest) {
    (void)source; (void)dest;
    return !stream->destroyed;
}

#define SDL_OpenAudioDeviceStream open_stream
#define SDL_ResumeAudioStreamDevice resume_stream
#define SDL_DestroyAudioStream destroy_stream
#define SDL_SetAudioStreamFormat set_format
#include "services/audio_playback.h"

int main(void) {
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
    SDL_AudioStream *music = NULL, *movie = NULL;
    SDL_AudioDeviceID music_device = 0, movie_device = 0;
    assert(xcde_audio_reconfigure(&music, &music_device, 1, &spec));
    SDL_AudioStream *original = music;

    fail_open = true;
    assert(!xcde_audio_reconfigure(&music, &music_device, 2, &spec));
    assert(music == original && !original->destroyed && music_device == 1);
    assert(!xcde_audio_reconfigure(&movie, &movie_device, 1, &spec));
    assert(!movie && !movie_device);

    fail_open = false;
    assert(xcde_audio_reconfigure(&movie, &movie_device, 1, &spec));
    fail_resume = true;
    assert(!xcde_audio_reconfigure(&music, &music_device, 2, &spec));
    assert(music == original && !original->destroyed && streams[2].destroyed);
    fail_resume = false;
    assert(xcde_audio_reconfigure(&music, &music_device, 2, &spec));
    assert(original->destroyed && music_device == 2 && movie_device == 1 && !movie->destroyed);
    assert(xcde_audio_reconfigure(&movie, &movie_device, 2, &spec));
    assert(movie_device == 2 && movie != music);
    puts("audio output replacement: OK");
    return 0;
}
