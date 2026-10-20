/* xu_wgl.c -- the wgl entry points libepoxy and ImGui's GL loader look up in "opengl32.dll", answered by OSMesa.
 *
 * The Xbox has no opengl32.dll (measured: tools/../probe census, error 126). The UWP package ships Mesa's
 * osmesa.dll under that name instead: OSMesa renders off screen into a buffer we own (d3d12 on the GPU, softpipe
 * as the fallback), and xemu's UWP front end presents that buffer through D3D11. epoxy and imgl3w resolve every
 * GL function past 1.1 through wglGetProcAddress and check the current context with wglGetCurrentContext; those
 * are the only wgl calls they make once a context is current. Added to Mesa's osmesa target by
 * tools/mesa_build.sh (sources + osmesa.def). */
#include <windows.h>
#include "GL/osmesa.h"

PROC WINAPI wglGetProcAddress(LPCSTR name)
{
    return (PROC)OSMesaGetProcAddress(name);
}

HGLRC WINAPI wglGetCurrentContext(void)
{
    return (HGLRC)OSMesaGetCurrentContext();
}

HDC WINAPI wglGetCurrentDC(void)
{
    return OSMesaGetCurrentContext() ? (HDC)(UINT_PTR)1 : NULL;
}
