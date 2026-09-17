#pragma once
#include "SDL.h"

struct SDL_DialogFileFilter { const char* name; const char* pattern; };
using SDL_DialogFileCallback = void (SDLCALL *)(void* userdata, const char* const* filelist, int filter);
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters,
                            const char* default_location, bool allow_many);
void SDL_ShowSaveFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters,
                            const char* default_location);
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                              const char* default_location, bool allow_many);
