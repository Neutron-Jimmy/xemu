/*
 * xemu-uwp.c -- xemu as a UWP app on the Xbox (Developer Mode): XEMU_UWP.
 *
 * What the console gives a Dev Mode app, measured (probe/jitprobe.c):
 *   - no RWX memory, but split-W^X through two views of one section works
 *     (xu_jit_map_views, used by tcg/region.c);
 *   - no opengl32.dll and no Win32 windows; D3D11/D3D12 at feature level 11_0;
 *   - Mesa's OSMesa with the d3d12 driver (shipped as opengl32.dll) gives
 *     GL 4.1 core on the GPU -- xemu needs 4.0;
 *   - 868 of xemu's 872 imports exist; the other 4 are stubbed below.
 *
 * So: the system starts the app in CoreApplication.Run; the view's Run calls
 * xemu's own main (renamed xemu_main by ui/xemu.c) on the window's thread.
 * SDL runs its dummy video driver; ui/xemu-uwp.h sends xemu's SDL GL calls
 * here, where a context is an OSMesa context rendering into a buffer of ours,
 * and SDL_GL_SwapWindow copies the window's buffer to a D3D11 swap chain on
 * the CoreWindow (and lets the window process its events).
 *
 * Files: settings, log and everything the user supplies (BIOS, MCPX ROM, HDD
 * image, discs) live in the app's LocalState, copied there through Device
 * Portal (tools/xdeploy.sh put).
 */
#ifdef XEMU_UWP

#undef WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <appmodel.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.storage.h>
#include <stdio.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include "xemu-uwp.h"
/* the real SDL calls, for the shims that wrap them */
#undef SDL_CreateWindow
#undef SDL_GetPrefPath

int xemu_main(int argc, char **argv);             /* ui/xemu.c's main */

typedef __x_ABI_CWindows_CUI_CCore_CICoreWindow CoreWindow;
typedef __x_ABI_CWindows_CUI_CCore_CICoreDispatcher CoreDispatcher;
typedef __x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewSource ViewSource;
typedef __x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkView View;
typedef __x_ABI_CWindows_CApplicationModel_CCore_CICoreApplicationView AppView;

static CoreWindow *window;
static CoreDispatcher *dispatcher;
static char local_dir[MAX_PATH];                 /* LocalState, no trailing slash */
static char package_dir[MAX_PATH];
static int g_argc;
static char **g_argv;

#define LOG(...) (fprintf(stderr, "uwp: " __VA_ARGS__), fputc('\n', stderr))

/* ------------------------------------------------------------------ imports the console lacks
 *
 * The loader refuses the whole image if one imported function is missing, so
 * these are defined here as the import pointers themselves: the linker then
 * takes ours and never asks the DLL. Measured missing on the console:
 * kernel32!GetDefaultCommConfigA (chardev serial), user32!CreateIconFromResourceEx
 * and user32!GetRawInputBuffer (SDL), imm32!ImmGetIMEFileNameA (SDL). */
static BOOL WINAPI stub_GetDefaultCommConfigA(LPCSTR n, LPCOMMCONFIG c, LPDWORD s)
{
    (void)n, (void)c, (void)s;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
static HICON WINAPI stub_CreateIconFromResourceEx(PBYTE b, DWORD n, BOOL i, DWORD v, int w, int h, UINT f)
{
    (void)b, (void)n, (void)i, (void)v, (void)w, (void)h, (void)f;
    return NULL;
}
static UINT WINAPI stub_GetRawInputBuffer(void *d, PUINT s, UINT h)
{
    (void)d, (void)s, (void)h;
    return (UINT)-1;
}
/* SDL also calls this one directly (through the import library's thunk, not
   __imp_), so the stub takes the thunk's name too, or the linker pulls in the
   import library member and finds two __imp_ImmGetIMEFileNameA */
UINT WINAPI stub_ImmGetIMEFileNameA(HKL k, LPSTR s, UINT n) __asm__("ImmGetIMEFileNameA");
UINT WINAPI stub_ImmGetIMEFileNameA(HKL k, LPSTR s, UINT n)
{
    (void)k, (void)s, (void)n;
    return 0;
}
void *__imp_GetDefaultCommConfigA = (void *)stub_GetDefaultCommConfigA;
void *__imp_CreateIconFromResourceEx = (void *)stub_CreateIconFromResourceEx;
void *__imp_GetRawInputBuffer = (void *)stub_GetRawInputBuffer;
void *__imp_ImmGetIMEFileNameA = (void *)stub_ImmGetIMEFileNameA;

/* ------------------------------------------------------------------ the JIT's memory */

bool xu_jit_map_views(size_t size, void **rw, void **rx)
{
    typedef HANDLE(WINAPI * CreateMapping_t)(HANDLE, PSECURITY_ATTRIBUTES, ULONG, ULONG64, PCWSTR);
    typedef PVOID(WINAPI * MapView3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                       void *, ULONG);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    CreateMapping_t create = (CreateMapping_t)(void *)GetProcAddress(kb, "CreateFileMappingFromApp");
    MapView3_t map = (MapView3_t)(void *)GetProcAddress(kb, "MapViewOfFile3FromApp");
    if (!create || !map) {
        LOG("jit: no CreateFileMappingFromApp/MapViewOfFile3FromApp");
        return false;
    }
    HANDLE s = create(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, size, NULL);
    if (!s) {
        LOG("jit: section of %zu bytes refused (error %lu)", size, GetLastError());
        return false;
    }
    *rw = map(s, GetCurrentProcess(), NULL, 0, size, 0, PAGE_READWRITE, NULL, 0);
    *rx = *rw ? map(s, GetCurrentProcess(), NULL, 0, size, 0, PAGE_EXECUTE_READ, NULL, 0) : NULL;
    DWORD err = GetLastError();
    CloseHandle(s);
    if (!*rx) {
        LOG("jit: views refused (error %lu)", err);
        if (*rw)
            UnmapViewOfFile(*rw);
        return false;
    }
    LOG("jit: %zu MB split-wx, rw %p rx %p", size >> 20, *rw, *rx);
    return true;
}

/* ------------------------------------------------------------------ WinRT plumbing */

static void *factory(const WCHAR *cls, REFIID iid)
{
    HSTRING h = NULL;
    void *f = NULL;
    if (FAILED(WindowsCreateString(cls, (UINT32)wcslen(cls), &h)))
        return NULL;
    HRESULT hr = RoGetActivationFactory(h, iid, &f);
    WindowsDeleteString(h);
    if (FAILED(hr))
        LOG("no activation factory for %ls (hr %08lX)", cls, (unsigned long)hr);
    return f;
}

static void find_folders(void)
{
    WCHAR w[MAX_PATH];
    UINT32 len = MAX_PATH;
    if (GetCurrentPackagePath(&len, w) == ERROR_SUCCESS)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, package_dir, sizeof package_dir, NULL, NULL);
    __x_ABI_CWindows_CStorage_CIApplicationDataStatics *st = NULL;
    __x_ABI_CWindows_CStorage_CIApplicationData *ad = NULL;
    __x_ABI_CWindows_CStorage_CIStorageFolder *lf = NULL;
    __x_ABI_CWindows_CStorage_CIStorageItem *it = NULL;
    HSTRING p = NULL;
    if (package_dir[0] &&
        (st = factory(RuntimeClass_Windows_Storage_ApplicationData,
                      &IID___x_ABI_CWindows_CStorage_CIApplicationDataStatics)) != NULL &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIApplicationDataStatics_get_Current(st, &ad)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIApplicationData_get_LocalFolder(ad, &lf)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIStorageFolder_QueryInterface(
            lf, &IID___x_ABI_CWindows_CStorage_CIStorageItem, (void **)&it)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIStorageItem_get_Path(it, &p)))
        WideCharToMultiByte(CP_UTF8, 0, WindowsGetStringRawBuffer(p, NULL), -1, local_dir,
                            sizeof local_dir, NULL, NULL);
    if (!local_dir[0]) {                         /* unpackaged (a PC test): the exe's folder */
        DWORD n = GetModuleFileNameA(NULL, local_dir, sizeof local_dir);
        while (n && local_dir[n - 1] != '\\')
            n--;
        local_dir[n ? n - 1 : 0] = 0;
        snprintf(package_dir, sizeof package_dir, "%s", local_dir);
    }
}

#define STATIC_REFCOUNT(prefix, T)                                                     \
    static ULONG STDMETHODCALLTYPE prefix##_addref(T *This) { (void)This; return 2; } \
    static ULONG STDMETHODCALLTYPE prefix##_release(T *This) { (void)This; return 1; }
#define INSPECTABLE_STUBS(prefix, T)                                                   \
    STATIC_REFCOUNT(prefix, T)                                                         \
    static HRESULT STDMETHODCALLTYPE prefix##_iids(T *This, ULONG *n, IID **iids)      \
    {                                                                                  \
        (void)This;                                                                    \
        *n = 0;                                                                        \
        *iids = NULL;                                                                  \
        return S_OK;                                                                   \
    }                                                                                  \
    static HRESULT STDMETHODCALLTYPE prefix##_name(T *This, HSTRING *s)                \
    {                                                                                  \
        (void)This;                                                                    \
        *s = NULL;                                                                     \
        return S_OK;                                                                   \
    }                                                                                  \
    static HRESULT STDMETHODCALLTYPE prefix##_trust(T *This, TrustLevel *t)            \
    {                                                                                  \
        (void)This;                                                                    \
        *t = BaseTrust;                                                                \
        return S_OK;                                                                   \
    }

/* ------------------------------------------------------------------ B is the pad's B, not "go home"
 * Windows.UI.Core.SystemNavigationManager is not in mingw's headers; IIDs from
 * the Windows SDK (from the NC2 port's front_uwp.c). */
DEFINE_GUID(IID_SysNavStatics, 0xdc52b5ce, 0xbee0, 0x4305, 0x8c, 0x54, 0x68, 0x22, 0x8e, 0xd6, 0x83, 0xb5);
DEFINE_GUID(IID_BackHandler, 0xca821060, 0x002b, 0x526d, 0x81, 0x22, 0x98, 0x26, 0x30, 0xd7, 0xcd, 0xbe);
DEFINE_GUID(IID_AgileObject, 0x94ea2b94, 0xe9cc, 0x49e0, 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90);

typedef struct BackArgs BackArgs;
struct BackArgsVtbl {
    void *iunknown[3], *iinspectable[3];
    HRESULT(STDMETHODCALLTYPE *get_Handled)(BackArgs *, boolean *);
    HRESULT(STDMETHODCALLTYPE *put_Handled)(BackArgs *, boolean);
};
struct BackArgs { const struct BackArgsVtbl *lpVtbl; };

typedef struct BackHandler BackHandler;
struct BackHandlerVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(BackHandler *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(BackHandler *);
    ULONG(STDMETHODCALLTYPE *Release)(BackHandler *);
    HRESULT(STDMETHODCALLTYPE *Invoke)(BackHandler *, IInspectable *, BackArgs *);
};
struct BackHandler { const struct BackHandlerVtbl *lpVtbl; };

typedef struct SysNav SysNav;
struct SysNavVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(SysNav *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(SysNav *);
    ULONG(STDMETHODCALLTYPE *Release)(SysNav *);
    void *iinspectable[3];
    HRESULT(STDMETHODCALLTYPE *add_BackRequested)(SysNav *, BackHandler *, EventRegistrationToken *);
    HRESULT(STDMETHODCALLTYPE *remove_BackRequested)(SysNav *, EventRegistrationToken);
};
struct SysNav { const struct SysNavVtbl *lpVtbl; };

typedef struct SysNavStatics SysNavStatics;
struct SysNavStaticsVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(SysNavStatics *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(SysNavStatics *);
    ULONG(STDMETHODCALLTYPE *Release)(SysNavStatics *);
    void *iinspectable[3];
    HRESULT(STDMETHODCALLTYPE *GetForCurrentView)(SysNavStatics *, SysNav **);
};
struct SysNavStatics { const struct SysNavStaticsVtbl *lpVtbl; };

static HRESULT STDMETHODCALLTYPE back_qi(BackHandler *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_BackHandler) ||
        IsEqualIID(riid, &IID_AgileObject)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
STATIC_REFCOUNT(back, BackHandler)
static HRESULT STDMETHODCALLTYPE back_invoke(BackHandler *This, IInspectable *sender, BackArgs *e)
{
    (void)This, (void)sender;
    e->lpVtbl->put_Handled(e, TRUE);
    return S_OK;
}
static const struct BackHandlerVtbl back_vtbl = {back_qi, back_addref, back_release, back_invoke};
static BackHandler back_handler = {&back_vtbl};

static void claim_back_button(void)
{
    SysNavStatics *s = factory(L"Windows.UI.Core.SystemNavigationManager", &IID_SysNavStatics);
    SysNav *nav = NULL;
    EventRegistrationToken tok;
    if (s && SUCCEEDED(s->lpVtbl->GetForCurrentView(s, &nav)) && nav &&
        SUCCEEDED(nav->lpVtbl->add_BackRequested(nav, &back_handler, &tok)))
        LOG("B is the pad's B (BackRequested handled)");
    else
        LOG("could not claim BackRequested: B may leave the app");
    if (nav)
        nav->lpVtbl->Release(nav);
    if (s)
        s->lpVtbl->Release(s);
}

/* ------------------------------------------------------------------ OSMesa (opengl32.dll in the package) */

#define OSMESA_RGBA 0x1908
#define OSMESA_FORMAT 0x22
#define OSMESA_DEPTH_BITS 0x30
#define OSMESA_STENCIL_BITS 0x31
#define OSMESA_ACCUM_BITS 0x32
#define OSMESA_PROFILE 0x33
#define OSMESA_CORE_PROFILE 0x34
#define OSMESA_COMPAT_PROFILE 0x35
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37
#define OSMESA_Y_UP 0x11

typedef void *(WINAPI *OSMesaCreateContextAttribs_t)(const int *, void *);
typedef int(WINAPI *OSMesaMakeCurrent_t)(void *, void *, unsigned, int, int);
typedef void(WINAPI *OSMesaDestroyContext_t)(void *);
typedef void *(WINAPI *OSMesaGetProcAddress_t)(const char *);
typedef void(WINAPI *OSMesaPixelStore_t)(int, int);
static OSMesaCreateContextAttribs_t osm_create;
static OSMesaMakeCurrent_t osm_make;
static OSMesaDestroyContext_t osm_destroy;
static OSMesaGetProcAddress_t osm_proc;
static OSMesaPixelStore_t osm_pixelstore;
static void(WINAPI *gl_finish)(void);

/* A context of ours: the OSMesa context and the buffer that is its default framebuffer */
typedef struct XuContext {
    void *osm;
    uint8_t *buf;
    int w, h;
    bool window;                                 /* the visible window's: presented on swap */
} XuContext;

static __thread XuContext *current;
static int attr_share, attr_major = 4, attr_minor = 0, attr_core = 1;
static int swap_interval = 1;

static void load_osmesa(void)
{
    if (osm_create)
        return;
    /* by full path, before anyone loads it by name: libepoxy and ImGui's GL loader do
       LoadLibrary("opengl32"), which then finds this one already loaded */
    char p[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s\\opengl32.dll", package_dir);
    HMODULE m = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        LOG("FATAL: cannot load %s (error %lu)", p, GetLastError());
        exit(1);
    }
    osm_create = (OSMesaCreateContextAttribs_t)(void *)GetProcAddress(m, "OSMesaCreateContextAttribs");
    osm_make = (OSMesaMakeCurrent_t)(void *)GetProcAddress(m, "OSMesaMakeCurrent");
    osm_destroy = (OSMesaDestroyContext_t)(void *)GetProcAddress(m, "OSMesaDestroyContext");
    osm_proc = (OSMesaGetProcAddress_t)(void *)GetProcAddress(m, "OSMesaGetProcAddress");
    osm_pixelstore = (OSMesaPixelStore_t)(void *)GetProcAddress(m, "OSMesaPixelStore");
    if (!osm_create || !osm_make || !osm_proc) {
        LOG("FATAL: %s is not OSMesa", p);
        exit(1);
    }
    gl_finish = (void(WINAPI *)(void))osm_proc("glFinish");
    LOG("OSMesa from %s (GALLIUM_DRIVER=%s)", p, getenv("GALLIUM_DRIVER") ? getenv("GALLIUM_DRIVER") : "default");
}

bool xu_SDL_GL_SetAttribute(SDL_GLAttr attr, int value)
{
    switch (attr) {
    case SDL_GL_SHARE_WITH_CURRENT_CONTEXT: attr_share = value; break;
    case SDL_GL_CONTEXT_MAJOR_VERSION: attr_major = value; break;
    case SDL_GL_CONTEXT_MINOR_VERSION: attr_minor = value; break;
    case SDL_GL_CONTEXT_PROFILE_MASK: attr_core = (value & SDL_GL_CONTEXT_PROFILE_CORE) != 0; break;
    default: break;                              /* RGBA8, depth 24, stencil 8: always */
    }
    return true;
}

SDL_GLContext xu_SDL_GL_CreateContext(SDL_Window *win)
{
    load_osmesa();
    XuContext *c = calloc(1, sizeof *c);
    c->window = win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_HIDDEN);
    c->w = c->h = 16;                            /* off-screen contexts draw only into their own FBOs */
    if (c->window)
        SDL_GetWindowSizeInPixels(win, &c->w, &c->h);
    c->buf = calloc((size_t)c->w * c->h, 4);
    int attrs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
                   OSMESA_PROFILE, attr_core ? OSMESA_CORE_PROFILE : OSMESA_COMPAT_PROFILE,
                   OSMESA_CONTEXT_MAJOR_VERSION, attr_major, OSMESA_CONTEXT_MINOR_VERSION, attr_minor, 0};
    void *share = attr_share && current ? current->osm : NULL;
    c->osm = osm_create(attrs, share);
    if (!c->osm) {
        LOG("OSMesaCreateContextAttribs(GL %d.%d %s) failed", attr_major, attr_minor, attr_core ? "core" : "compat");
        SDL_SetError("OSMesa: no GL %d.%d context", attr_major, attr_minor);
        free(c->buf);
        free(c);
        return NULL;
    }
    LOG("GL context %p: %dx%d%s%s", (void *)c, c->w, c->h, c->window ? ", the window's" : "",
        share ? ", shared" : "");
    xu_SDL_GL_MakeCurrent(win, (SDL_GLContext)c);  /* SDL makes a new context current too */
    return (SDL_GLContext)c;
}

bool xu_SDL_GL_MakeCurrent(SDL_Window *win, SDL_GLContext context)
{
    (void)win;
    XuContext *c = (XuContext *)context;
    if (!c) {
        if (current && osm_make)
            osm_make(NULL, NULL, 0, 0, 0);
        current = NULL;
        return true;
    }
    if (!osm_make(c->osm, c->buf, 0x1401 /* GL_UNSIGNED_BYTE */, c->w, c->h)) {
        SDL_SetError("OSMesaMakeCurrent failed");
        return false;
    }
    if (osm_pixelstore)
        osm_pixelstore(OSMESA_Y_UP, 0);           /* row 0 is the top: the swap chain's order */
    current = c;
    return true;
}

SDL_GLContext xu_SDL_GL_GetCurrentContext(void)
{
    return (SDL_GLContext)current;
}

SDL_FunctionPointer xu_SDL_GL_GetProcAddress(const char *proc)
{
    load_osmesa();
    return (SDL_FunctionPointer)osm_proc(proc);
}

bool xu_SDL_GL_SetSwapInterval(int interval)
{
    swap_interval = interval;
    return true;
}

bool xu_SDL_GL_DestroyContext(SDL_GLContext context)
{
    XuContext *c = (XuContext *)context;
    if (!c)
        return true;
    if (current == c)
        xu_SDL_GL_MakeCurrent(NULL, NULL);
    osm_destroy(c->osm);
    free(c->buf);
    free(c);
    return true;
}

/* ------------------------------------------------------------------ the picture: D3D11 on the CoreWindow */

static ID3D11Device *dev;
static ID3D11DeviceContext *dctx;
static IDXGISwapChain1 *swap;
static int swap_w, swap_h;

static void process_window_events(void)
{
    if (dispatcher)
        __x_ABI_CWindows_CUI_CCore_CICoreDispatcher_ProcessEvents(
            dispatcher, CoreProcessEventsOption_ProcessAllIfPresent);
}

static bool open_swap_chain(int w, int h)
{
    HRESULT hr;
    if (!dev) {
        D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2, D3D11_SDK_VERSION, &dev, NULL,
                               &dctx);
        if (FAILED(hr)) {
            LOG("D3D11CreateDevice failed (hr %08lX)", (unsigned long)hr);
            return false;
        }
    }
    if (swap) {
        IDXGISwapChain1_Release(swap);
        swap = NULL;
    }
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dd)) ||
        FAILED(IDXGIDevice_GetAdapter(dd, &ad)) ||
        FAILED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac))) {
        LOG("no DXGI factory");
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)w, sd.Height = (UINT)h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;      /* OSMesa's RGBA bytes, as they are */
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = IDXGIFactory2_CreateSwapChainForCoreWindow(fac, (IUnknown *)dev, (IUnknown *)window, &sd, NULL, &swap);
    if (FAILED(hr)) {
        sd.Scaling = DXGI_SCALING_STRETCH;
        hr = IDXGIFactory2_CreateSwapChainForCoreWindow(fac, (IUnknown *)dev, (IUnknown *)window, &sd, NULL, &swap);
    }
    IDXGIFactory2_Release(fac);
    IDXGIAdapter_Release(ad);
    IDXGIDevice_Release(dd);
    if (FAILED(hr)) {
        LOG("CreateSwapChainForCoreWindow %dx%d failed (hr %08lX)", w, h, (unsigned long)hr);
        return false;
    }
    swap_w = w, swap_h = h;
    LOG("swap chain %dx%d", w, h);
    return true;
}

bool xu_SDL_GL_SwapWindow(SDL_Window *win)
{
    (void)win;
    XuContext *c = current;
    if (c && c->window && window) {
        if (gl_finish)
            gl_finish();                         /* OSMesa writes the buffer at the flush */
        if ((!swap || swap_w != c->w || swap_h != c->h) && !open_swap_chain(c->w, c->h))
            window = NULL;                       /* no picture, but keep running (the log says why) */
        if (swap) {
            ID3D11Texture2D *back = NULL;
            if (SUCCEEDED(IDXGISwapChain1_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back))) {
                ID3D11DeviceContext_UpdateSubresource(dctx, (ID3D11Resource *)back, 0, NULL, c->buf,
                                                      (UINT)c->w * 4, 0);
                ID3D11Texture2D_Release(back);
            }
            IDXGISwapChain1_Present(swap, swap_interval ? 1 : 0, 0);
        }
    }
    process_window_events();
    return true;
}

/* ------------------------------------------------------------------ SDL shims */

SDL_Window *xu_SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags)
{
    /* the dummy video driver has no GL: we are the GL */
    flags &= ~(SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!(flags & SDL_WINDOW_HIDDEN)) {
        w = 1280, h = 720;                       /* the picture; the system scales it to the screen */
    }
    return SDL_CreateWindow(title, w, h, flags);
}

char *xu_SDL_GetPrefPath(const char *org, const char *app)
{
    (void)org, (void)app;
    char p[MAX_PATH + 2];
    snprintf(p, sizeof p, "%s\\", local_dir);
    return SDL_strdup(p);
}

/* ------------------------------------------------------------------ the app */

static HRESULT STDMETHODCALLTYPE view_qi(View *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IInspectable) ||
        IsEqualIID(riid, &IID___x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkView)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
INSPECTABLE_STUBS(view, View)
static HRESULT STDMETHODCALLTYPE view_initialize(View *This, AppView *app)
{
    (void)This, (void)app;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_set_window(View *This, CoreWindow *w)
{
    (void)This;
    window = w;
    __x_ABI_CWindows_CUI_CCore_CICoreWindow_AddRef(w);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_load(View *This, HSTRING entry)
{
    (void)This, (void)entry;
    return S_OK;
}

static LONG CALLBACK last_word(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == 0x406D1388 || er->ExceptionCode == DBG_PRINTEXCEPTION_C ||
        er->ExceptionCode == 0x4001000A /* DBG_PRINTEXCEPTION_WIDE_C */)
        return EXCEPTION_CONTINUE_SEARCH;        /* thread names, debug strings */
    if (er->ExceptionCode >= 0xC0000000u) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &m);
        if (m)
            GetModuleFileNameA(m, name, sizeof name);
        fprintf(stderr, "uwp: exception %08lX at %p (%s+%llx), address %p, thread %lu\n",
                (unsigned long)er->ExceptionCode, er->ExceptionAddress, name,
                (unsigned long long)((uintptr_t)er->ExceptionAddress - (uintptr_t)m),
                er->NumberParameters > 1 ? (void *)er->ExceptionInformation[1] : NULL, GetCurrentThreadId());
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static HRESULT STDMETHODCALLTYPE view_run(View *This)
{
    (void)This;
    __x_ABI_CWindows_CUI_CCore_CICoreWindow_Activate(window);   /* without this the splash screen stays */
    __x_ABI_CWindows_CUI_CCore_CICoreWindow_get_Dispatcher(window, &dispatcher);
    claim_back_button();
    process_window_events();
    int code = xemu_main(g_argc, g_argv);
    LOG("xemu_main returned %d", code);
    fflush(stderr);
    exit(code);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_uninitialize(View *This)
{
    (void)This;
    return S_OK;
}
static const __x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewVtbl view_vtbl = {
    view_qi, view_addref, view_release, view_iids, view_name, view_trust,
    view_initialize, view_set_window, view_load, view_run, view_uninitialize,
};
static View the_view = {(__x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewVtbl *)&view_vtbl};

static HRESULT STDMETHODCALLTYPE source_qi(ViewSource *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IInspectable) ||
        IsEqualIID(riid, &IID___x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewSource)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
INSPECTABLE_STUBS(source, ViewSource)
static HRESULT STDMETHODCALLTYPE source_create_view(ViewSource *This, View **out)
{
    (void)This;
    *out = &the_view;
    return S_OK;
}
static const __x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewSourceVtbl source_vtbl = {
    source_qi, source_addref, source_release, source_iids, source_name, source_trust, source_create_view,
};
static ViewSource the_source = {(__x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewSourceVtbl *)&source_vtbl};

int main(int argc, char **argv)
{
    RoInitialize(RO_INIT_MULTITHREADED);
    find_folders();

    char p[MAX_PATH + 32], prev[MAX_PATH + 32];
    snprintf(p, sizeof p, "%s\\xemu.log", local_dir);
    snprintf(prev, sizeof prev, "%s\\xemu.prev.log", local_dir);
    MoveFileExA(p, prev, MOVEFILE_REPLACE_EXISTING);  /* keep the last session's log: it may be the one that went wrong */
    /* One file, one descriptor: stdout and stderr each freopen'd on the log kept separate file positions and wrote
       over each other's lines (the launch command line came out cut in half). */
    freopen(p, "w", stderr);
    freopen(p, "a", stdout);                     /* gives stdout a descriptor (a GUI app starts without one) */
    _dup2(_fileno(stderr), _fileno(stdout));     /* ...then makes it stderr's: one shared file position */
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(0, last_word);
    LOG("package %s, local state %s", package_dir, local_dir);
    SetCurrentDirectoryA(local_dir);             /* relative paths land somewhere writable */

    /* SDL: no Win32 windows on the console -- the dummy video driver, pads through Windows.Gaming.Input, and
       audio that falls back to silence rather than failing (XAudio2 is the right one; WASAPI may not open in an
       app container). XInput loads on the console but a UWP app's pads never show up through it (measured: no
       input at all); WGI is how a UWP app gets them. */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "wasapi,dummy");
    if (!getenv("GALLIUM_DRIVER"))
        _putenv("GALLIUM_DRIVER=d3d12");
    _putenv("MESA_GL_VERSION_OVERRIDE=4.0");    /* what xemu asks for; d3d12 says 4.1 */
    load_osmesa();

    /* the system's activation argument (-ServerName:...) is not xemu's */
    int n = 1;
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "-ServerName:", 12))
            argv[n++] = argv[i];
        else
            LOG("activation argument %s dropped", argv[i]);
    argv[n] = NULL;
    g_argc = n, g_argv = argv;

    if (!package_dir[0] || GetEnvironmentVariableA("XEMU_UWP_DIRECT", NULL, 0)) {
        LOG("not packaged: running without a CoreWindow (no picture)");
        return xemu_main(g_argc, g_argv);
    }
    __x_ABI_CWindows_CApplicationModel_CCore_CICoreApplication *app =
        factory(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication,
                &IID___x_ABI_CWindows_CApplicationModel_CCore_CICoreApplication);
    if (!app)
        return 3;
    HRESULT hr = __x_ABI_CWindows_CApplicationModel_CCore_CICoreApplication_Run(app, &the_source);
    LOG("CoreApplication.Run returned (hr %08lX)", (unsigned long)hr);
    return FAILED(hr) ? 3 : 0;
}

#endif /* XEMU_UWP */
