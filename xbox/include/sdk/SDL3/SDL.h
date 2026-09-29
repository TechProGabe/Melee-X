/* SDL3/SDL.h - the SDL3 audio-stream subset src/pc/audio.c uses, backed by
 * the Xbox audio output (xbox/src/sdk/sdl3_audio.c). Only on the SDK/src-pc
 * include path; the Xbox layer itself uses nxdk's SDL2. */
#ifndef XSDK_SDL3_SHIM_H
#define XSDK_SDL3_SHIM_H
#include <stdbool.h>
#include <stdint.h>

/* renamed: nxdk's SDL2 (the controllers) defines the SDL2 functions of the same name */
#define SDL_InitSubSystem xsdk_SDL_InitSubSystem
#define SDL_GetError xsdk_SDL_GetError
#define SDL_OpenAudioDeviceStream xsdk_SDL_OpenAudioDeviceStream
#define SDL_PutAudioStreamData xsdk_SDL_PutAudioStreamData
#define SDL_GetAudioStreamQueued xsdk_SDL_GetAudioStreamQueued
#define SDL_SetAudioStreamGain xsdk_SDL_SetAudioStreamGain
#define SDL_ResumeAudioStreamDevice xsdk_SDL_ResumeAudioStreamDevice
#define SDL_DestroyAudioStream xsdk_SDL_DestroyAudioStream

#define SDLCALL
#define SDL_INIT_AUDIO 0x10u
#define SDL_AUDIO_F32 0x8120u
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu

typedef uint32_t SDL_AudioDeviceID;
typedef uint32_t SDL_AudioFormat;
typedef struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
} SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (SDLCALL* SDL_AudioStreamCallback)(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                                int total_amount);

bool SDL_InitSubSystem(uint32_t flags);
const char* SDL_GetError(void);
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID dev, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback cb, void* userdata);
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len);
int SDL_GetAudioStreamQueued(SDL_AudioStream* stream);
bool SDL_SetAudioStreamGain(SDL_AudioStream* stream, float gain);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream);
void SDL_DestroyAudioStream(SDL_AudioStream* stream);

#endif
