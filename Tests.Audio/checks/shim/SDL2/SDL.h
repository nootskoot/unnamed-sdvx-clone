#pragma once
// Minimal SDL2 stub: enough surface for the Audio DSP translation units.
#include <cstdint>
#include <cstddef>
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef float    Float32;
typedef uint32_t SDL_AudioDeviceID;
typedef uint16_t SDL_AudioFormat;
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_Thread SDL_Thread;
typedef struct SDL_RWops SDL_RWops;
typedef struct SDL_AudioCVT SDL_AudioCVT;
struct SDL_AudioSpec { int freq; SDL_AudioFormat format; Uint8 channels; Uint8 silence; Uint16 samples; Uint16 padding; Uint32 size; void (*callback)(void*, Uint8*, int); void* userdata; };
enum { AUDIO_F32SYS = 0x8120, AUDIO_S16SYS = 0x8010, AUDIO_S32SYS = 0x8020, AUDIO_U8 = 0x0008 };
inline const char* SDL_GetError() { return ""; }
inline int SDL_Init(Uint32) { return 0; }
inline void SDL_Quit() {}
inline SDL_mutex* SDL_CreateMutex() { return nullptr; }
inline int SDL_LockMutex(SDL_mutex*) { return 0; }
inline int SDL_UnlockMutex(SDL_mutex*) { return 0; }
inline void SDL_DestroyMutex(SDL_mutex*) {}
inline void SDL_Delay(Uint32) {}
inline Uint32 SDL_GetTicks() { return 0; }
