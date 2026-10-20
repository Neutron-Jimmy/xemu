/*
 * xemu-uwp.h -- the Xbox (UWP, Developer Mode) build's redirections (XEMU_UWP).
 *
 * The console has no OpenGL and no Win32 windows (RegisterClassExW fails with
 * ERROR_CALL_NOT_IMPLEMENTED; measured with probe/jitprobe.c). So SDL runs its
 * "dummy" video driver, and every SDL GL call xemu makes goes to ui/xemu-uwp.c
 * instead: a context is an OSMesa context (Mesa's d3d12 driver, GL 4.1 core on
 * the console's GPU), a window's default framebuffer is a buffer we own, and
 * SDL_GL_SwapWindow copies that buffer to a D3D11 swap chain on the app's
 * CoreWindow. Included after <SDL3/SDL.h> by the files that make those calls.
 */
#ifndef XEMU_UWP_H
#define XEMU_UWP_H

#ifdef XEMU_UWP
#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

bool xu_SDL_GL_SetAttribute(SDL_GLAttr attr, int value);
SDL_GLContext xu_SDL_GL_CreateContext(SDL_Window *window);
bool xu_SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);
bool xu_SDL_GL_SwapWindow(SDL_Window *window);
bool xu_SDL_GL_SetSwapInterval(int interval);
bool xu_SDL_GL_DestroyContext(SDL_GLContext context);
SDL_GLContext xu_SDL_GL_GetCurrentContext(void);
SDL_FunctionPointer xu_SDL_GL_GetProcAddress(const char *proc);
SDL_Window *xu_SDL_CreateWindow(const char *title, int w, int h,
                                SDL_WindowFlags flags);
char *xu_SDL_GetPrefPath(const char *org, const char *app);

/* The JIT's two views of one section (tcg/region.c): false if refused */
bool xu_jit_map_views(size_t size, void **rw, void **rx);

#ifdef __cplusplus
}
#endif

#define SDL_GL_SetAttribute xu_SDL_GL_SetAttribute
#define SDL_GL_CreateContext xu_SDL_GL_CreateContext
#define SDL_GL_MakeCurrent xu_SDL_GL_MakeCurrent
#define SDL_GL_SwapWindow xu_SDL_GL_SwapWindow
#define SDL_GL_SetSwapInterval xu_SDL_GL_SetSwapInterval
#define SDL_GL_DestroyContext xu_SDL_GL_DestroyContext
#define SDL_GL_GetCurrentContext xu_SDL_GL_GetCurrentContext
#define SDL_GL_GetProcAddress xu_SDL_GL_GetProcAddress
#define SDL_CreateWindow xu_SDL_CreateWindow
#define SDL_GetPrefPath xu_SDL_GetPrefPath

#endif /* XEMU_UWP */
#endif /* XEMU_UWP_H */
