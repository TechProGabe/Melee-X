/* sdl3_audio.c - the SDL3 audio stream src/pc/audio.c (melee-pc's AX mixer)
 * opens, backed by xhw_audio (AC97 / xemu APU).
 *
 * The mixer's pull callback runs on a mixer thread here, as it does on SDL's
 * audio thread elsewhere. It keeps about 50 ms of 32 kHz audio queued ahead
 * of the output: enough for the game thread's worst frames, short enough
 * that sound effects stay in sync with the picture. */
#include <SDL3/SDL.h>
#include <string.h>

#include "xhw.h"

#define RATE 32000
#define RING_FRAMES 16384   /* xhw_audio.c's ring, in stereo frames */
#define LEAD_FRAMES 1600    /* ~50 ms */

struct SDL_AudioStream {
    SDL_AudioStreamCallback cb;
    void* user;
    float gain;
    volatile int run;
};

static struct SDL_AudioStream s_stream;

bool SDL_InitSubSystem(uint32_t flags) {
    (void)flags;
    return true;
}

const char* SDL_GetError(void) { return "Xbox audio output did not start"; }

static uint32_t queued_frames(void) { return RING_FRAMES - xhw_audio_space(); }

static void mixer(void* arg) {
    struct SDL_AudioStream* s = (struct SDL_AudioStream*)arg;
    while (__atomic_load_n(&s->run, __ATOMIC_ACQUIRE)) {
        uint32_t q = queued_frames();
        if (q < LEAD_FRAMES) {
            int want = (int)((LEAD_FRAMES - q) * 2 * sizeof(float));
            uint64_t t0 = xhw_perf_now();
            s->cb(s->user, s, want, want);
            xhw_perf_audio(xhw_perf_now() - t0);
        } else {
            xhw_sleep_ms(2);
        }
    }
}

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback cb, void* userdata) {
    (void)dev;
    if (!spec || spec->channels != 2 || spec->freq != RATE || spec->format != SDL_AUDIO_F32) return NULL;
    if (!xhw_audio_init(RATE)) return NULL;
    s_stream.cb = cb;
    s_stream.user = userdata;
    s_stream.gain = 1.0f;
    return &s_stream;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* s) {
    if (!s->cb || s->run) return true;
    s->run = 1;
    return xhw_thread_start(mixer, s, 1, 64 * 1024) != 0;
}

bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len) {
    const float* in = (const float*)buf;
    int16_t out[512];
    int n = len / (int)sizeof(float), i = 0;
    while (i < n) {
        int k, chunk = n - i < (int)(sizeof out / sizeof out[0]) ? n - i : (int)(sizeof out / sizeof out[0]);
        for (k = 0; k < chunk; k++) {
            float v = in[i + k] * s->gain * 32767.0f;
            out[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        xhw_audio_write(out, (uint32_t)chunk / 2);
        i += chunk;
    }
    return true;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream* s) {
    (void)s;
    return (int)(queued_frames() * 2 * sizeof(float));
}

bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain) {
    s->gain = gain;
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream* s) {
    __atomic_store_n(&s->run, 0, __ATOMIC_RELEASE);
    xhw_audio_stop();
}
