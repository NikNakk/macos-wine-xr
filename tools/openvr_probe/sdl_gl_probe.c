// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Does SDL get an OpenGL context under Wine as SteamVR's web helper asks for
// one? vrwebhelper requests a hidden window and a 4.1 core profile context
// (no multisampling) through its bundled SDL2.dll; this repeats that request,
// then tries other versions and profiles.
//
//   x86_64-w64-mingw32-gcc -O1 -o sdl_gl_probe.exe sdl_gl_probe.c
//   sdl_gl_probe.exe <path to SDL2.dll>
#include <windows.h>

#include <stdio.h>

enum
{
	SDL_INIT_VIDEO = 0x20,
	SDL_GL_MULTISAMPLEBUFFERS = 13,
	SDL_GL_MULTISAMPLESAMPLES = 14,
	SDL_GL_CONTEXT_MAJOR_VERSION = 17,
	SDL_GL_CONTEXT_MINOR_VERSION = 18,
	SDL_GL_CONTEXT_PROFILE_MASK = 21,
	SDL_GL_CONTEXT_PROFILE_CORE = 1,
	SDL_GL_CONTEXT_PROFILE_COMPATIBILITY = 2,
	SDL_WINDOW_OPENGL = 2,
	SDL_WINDOW_HIDDEN = 8,
};

typedef int (*Init)(unsigned);
typedef int (*SetAttribute)(int, int);
typedef void *(*CreateWindowFn)(const char *, int, int, int, int, unsigned);
typedef void *(*CreateContext)(void *);
typedef void (*DeleteContext)(void *);
typedef void (*SdlDestroyWindow)(void *);
typedef const char *(*GetError)(void);
typedef void (*ResetAttributes)(void);
typedef const unsigned char *(WINAPI *GlGetString)(unsigned);

int
main(int argc, char **argv)
{
	HMODULE sdl = LoadLibraryA(argc > 1 ? argv[1] : "SDL2.dll");
	if (!sdl) {
		printf("LoadLibrary SDL2 failed: %lu\n", GetLastError());
		return 1;
	}
	Init init = (Init)GetProcAddress(sdl, "SDL_Init");
	SetAttribute set = (SetAttribute)GetProcAddress(sdl, "SDL_GL_SetAttribute");
	CreateWindowFn createWindow = (CreateWindowFn)GetProcAddress(sdl, "SDL_CreateWindow");
	CreateContext createContext = (CreateContext)GetProcAddress(sdl, "SDL_GL_CreateContext");
	DeleteContext deleteContext = (DeleteContext)GetProcAddress(sdl, "SDL_GL_DeleteContext");
	SdlDestroyWindow destroyWindow = (SdlDestroyWindow)GetProcAddress(sdl, "SDL_DestroyWindow");
	GetError getError = (GetError)GetProcAddress(sdl, "SDL_GetError");
	ResetAttributes reset = (ResetAttributes)GetProcAddress(sdl, "SDL_GL_ResetAttributes");
	GlGetString glGetString = (GlGetString)GetProcAddress(LoadLibraryA("opengl32.dll"), "glGetString");
	if (init(SDL_INIT_VIDEO) != 0) {
		printf("SDL_Init: %s\n", getError());
		return 1;
	}
	struct
	{
		const char *name;
		int major, minor, profile;
	} tries[] = {
	    {"4.1 core (vrwebhelper)", 4, 1, SDL_GL_CONTEXT_PROFILE_CORE},
	    {"3.2 core", 3, 2, SDL_GL_CONTEXT_PROFILE_CORE},
	    {"2.1 compatibility", 2, 1, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY},
	    {"default", 0, 0, 0},
	};
	for (int i = 0; i < (int)(sizeof(tries) / sizeof(tries[0])); ++i) {
		reset();
		if (tries[i].major) {
			set(SDL_GL_CONTEXT_MAJOR_VERSION, tries[i].major);
			set(SDL_GL_CONTEXT_MINOR_VERSION, tries[i].minor);
			set(SDL_GL_CONTEXT_PROFILE_MASK, tries[i].profile);
		}
		set(SDL_GL_MULTISAMPLEBUFFERS, 0);
		set(SDL_GL_MULTISAMPLESAMPLES, 0);
		void *window = createWindow("VRWebHelperOGL", 0, 0, 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
		if (!window) {
			printf("%s: window failed: %s\n", tries[i].name, getError());
			continue;
		}
		void *context = createContext(window);
		if (context) {
			printf("%s: context OK, GL_VERSION %s\n", tries[i].name, glGetString(0x1F02));
			deleteContext(context);
		} else {
			printf("%s: context failed: %s\n", tries[i].name, getError());
		}
		destroyWindow(window);
	}
	return 0;
}
