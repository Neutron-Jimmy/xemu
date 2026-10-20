/* jitprobe.c -- what a Dev Mode UWP app on the Xbox may do with executable memory, before xemu's TCG is ported.
 *
 * xemu (QEMU TCG) allocates its code buffer with VirtualAlloc(PAGE_EXECUTE_READWRITE). A packaged app gets only
 * the ...FromApp memory calls, and execute protections only with the codeGeneration capability. This probe tries
 * each route a JIT could take and logs the result to LocalState\probe.log (fetched through Device Portal):
 *
 *   A  VirtualAllocFromApp(PAGE_EXECUTE_READWRITE)              -- QEMU's own way (expected to be refused)
 *   B  RW alloc, VirtualProtectFromApp -> PAGE_EXECUTE_READ     -- W^X by flipping (PPSSPP/Dolphin UWP)
 *   C  one section, an RW view and an RX view                   -- QEMU "split-wx" (rw/rx at a fixed distance)
 *   D  how much memory the app may commit
 *   E  Direct3D 11 device and feature level
 *
 * Every step is logged (and flushed) BEFORE it runs, so a crash still says where it happened.
 *
 * Build: tools/build_probe.sh. Package + install: tools/xpkg.sh probe, tools/xdeploy.sh install|launch|log.
 */
#undef WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <appmodel.h>
#include <d3d11.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef __x_ABI_CWindows_CUI_CCore_CICoreWindow CoreWindow;
typedef __x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkViewSource ViewSource;
typedef __x_ABI_CWindows_CApplicationModel_CCore_CIFrameworkView View;
typedef __x_ABI_CWindows_CApplicationModel_CCore_CICoreApplicationView AppView;

static CoreWindow *window;
static FILE *logf;
static char local_dir[MAX_PATH] = ".";

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (logf) {
        vfprintf(logf, fmt, ap);
        fputc('\n', logf);
        fflush(logf);
    }
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void *factory(const WCHAR *cls, REFIID iid)
{
    HSTRING h = NULL;
    void *f = NULL;
    if (FAILED(WindowsCreateString(cls, (UINT32)wcslen(cls), &h)))
        return NULL;
    RoGetActivationFactory(h, iid, &f);
    WindowsDeleteString(h);
    return f;
}

static void find_local_dir(void)
{
    WCHAR w[MAX_PATH];
    UINT32 len = MAX_PATH;
    if (GetCurrentPackagePath(&len, w) != ERROR_SUCCESS)
        return;                                 /* unpackaged: the current directory */
    __x_ABI_CWindows_CStorage_CIApplicationDataStatics *st;
    __x_ABI_CWindows_CStorage_CIApplicationData *ad;
    __x_ABI_CWindows_CStorage_CIStorageFolder *lf;
    __x_ABI_CWindows_CStorage_CIStorageItem *it;
    HSTRING p;
    if ((st = factory(RuntimeClass_Windows_Storage_ApplicationData,
                      &IID___x_ABI_CWindows_CStorage_CIApplicationDataStatics)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIApplicationDataStatics_get_Current(st, &ad)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIApplicationData_get_LocalFolder(ad, &lf)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIStorageFolder_QueryInterface(
            lf, &IID___x_ABI_CWindows_CStorage_CIStorageItem, (void **)&it)) &&
        SUCCEEDED(__x_ABI_CWindows_CStorage_CIStorageItem_get_Path(it, &p)))
        WideCharToMultiByte(CP_ACP, 0, WindowsGetStringRawBuffer(p, NULL), -1, local_dir, sizeof local_dir, NULL,
                            NULL);
}

/* mov eax, imm32 ; ret */
static void emit(unsigned char *p, int value)
{
    p[0] = 0xB8;
    memcpy(p + 1, &value, 4);
    p[5] = 0xC3;
}
typedef int (*fn_t)(void);

#define SZ 0x10000

static void probe_a(void)
{
    say("A  VirtualAllocFromApp(PAGE_EXECUTE_READWRITE)");
    void *p = VirtualAllocFromApp(NULL, SZ, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p) {
        say("   refused (error %lu)", GetLastError());
        return;
    }
    say("   got %p; writing and calling", p);
    emit(p, 42);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    say("   A RESULT: called, returned %d (want 42) -- RWX WORKS", ((fn_t)p)());
}

static void probe_b(void)
{
    say("B  VirtualAllocFromApp(RW) then VirtualProtectFromApp(EXECUTE_READ)");
    unsigned char *p = VirtualAllocFromApp(NULL, SZ, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        say("   RW alloc refused (error %lu)", GetLastError());
        return;
    }
    emit(p, 42);
    ULONG old;
    if (!VirtualProtectFromApp(p, SZ, PAGE_EXECUTE_READ, &old)) {
        say("   protect -> EXECUTE_READ refused (error %lu)", GetLastError());
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    say("   flipped to RX; calling");
    int r1 = ((fn_t)p)();
    if (!VirtualProtectFromApp(p, SZ, PAGE_READWRITE, &old)) {
        say("   flip back -> RW refused (error %lu)", GetLastError());
        return;
    }
    emit(p, 43);
    VirtualProtectFromApp(p, SZ, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    int r2 = ((fn_t)p)();
    say("   B RESULT: %d then %d (want 42, 43) -- W^X FLIPPING %s", r1, r2, r1 == 42 && r2 == 43 ? "WORKS" : "WRONG");

    /* one flip costs what? TCG would flip for every translated block if it used this route */
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 1000; i++) {
        VirtualProtectFromApp(p, 0x1000, PAGE_READWRITE, &old);
        VirtualProtectFromApp(p, 0x1000, PAGE_EXECUTE_READ, &old);
    }
    QueryPerformanceCounter(&t1);
    say("   1000 RW<->RX flips: %.2f us each", (double)(t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 1000);
}

static void probe_c(DWORD section_prot, const char *name)
{
    say("C  section %s, RW view + RX view (split-wx)", name);
    HANDLE s = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, NULL, section_prot, SZ, NULL);
    if (!s) {
        say("   CreateFileMappingFromApp refused (error %lu)", GetLastError());
        return;
    }
    HANDLE me = GetCurrentProcess();
    unsigned char *rw = MapViewOfFile3FromApp(s, me, NULL, 0, SZ, 0, PAGE_READWRITE, NULL, 0);
    if (!rw) {
        say("   RW view refused (error %lu)", GetLastError());
        return;
    }
    unsigned char *rx = MapViewOfFile3FromApp(s, me, NULL, 0, SZ, 0, PAGE_EXECUTE_READ, NULL, 0);
    if (!rx) {
        say("   MapViewOfFile3FromApp(EXECUTE_READ) refused (error %lu)", GetLastError());
        rx = MapViewOfFileFromApp(s, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, SZ);
        if (!rx) {
            say("   MapViewOfFileFromApp(READ|EXECUTE) refused too (error %lu)", GetLastError());
            return;
        }
        say("   but MapViewOfFileFromApp(READ|EXECUTE) mapped it");
    }
    say("   rw %p rx %p (diff %lld); writing through rw, calling through rx", rw, rx, (long long)(rx - rw));
    emit(rw, 42);
    FlushInstructionCache(me, rx, 6);
    int r1 = ((fn_t)rx)();
    emit(rw, 43);                               /* patch in place, as TCG's goto_tb does */
    FlushInstructionCache(me, rx, 6);
    int r2 = ((fn_t)rx)();
    say("   C RESULT (%s): %d then %d (want 42, 43) -- SPLIT-WX %s", name, r1, r2,
        r1 == 42 && r2 == 43 ? "WORKS" : "WRONG");
}

static void probe_d(void)
{
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    GlobalMemoryStatusEx(&m);
    say("D  memory: phys %llu MB total, %llu MB avail; commit limit %llu MB, avail %llu MB",
        m.ullTotalPhys >> 20, m.ullAvailPhys >> 20, m.ullTotalPageFile >> 20, m.ullAvailPageFile >> 20);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    say("   cpus %lu, page %lu, granularity %lu", si.dwNumberOfProcessors, si.dwPageSize, si.dwAllocationGranularity);
    size_t mb;
    for (mb = 256; mb <= 8192; mb *= 2) {
        void *p = VirtualAllocFromApp(NULL, (size_t)mb << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            say("   commit %zu MB: refused (error %lu)", mb, GetLastError());
            break;
        }
        VirtualFree(p, 0, MEM_RELEASE);
        say("   commit %zu MB: ok", mb);
    }
}

static void probe_e(void)
{
    say("E  D3D11CreateDevice");
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                D3D_FEATURE_LEVEL_11_0};
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 4, D3D11_SDK_VERSION, &dev, &got,
                                   &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want + 2, 2, D3D11_SDK_VERSION, &dev, &got,
                               &ctx);
    say("   hr %08lX, feature level %X", (unsigned long)hr, (unsigned)got);
    if (dev) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS o = {0};
        ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o);
        say("   logic ops %d, map-no-overwrite on dynamic CB %d", o.OutputMergerLogicOp, o.MapNoOverwriteOnDynamicConstantBuffer);
        ID3D11Device_Release(dev);
    }
    if (ctx)
        ID3D11DeviceContext_Release(ctx);
}

/* ------------------------------------------------------------------ F: the platform census
 *
 * Which system DLLs and functions exist on the console, measured instead of guessed. LoadLibraryExW is not in
 * windowsapp, so it is found through kernelbase.dll, which every process has loaded: its address comes from the
 * PEB's module list (no import needed), then GetProcAddress. Each DLL is loaded by name (system32 search). */
#include <winternl.h>
typedef HMODULE(WINAPI *LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryExW_t load_ex;

static HMODULE loaded_module(const WCHAR *name)
{
    PEB *peb = (PEB *)__readgsqword(0x60);
    LIST_ENTRY *head = &peb->Ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
        LDR_DATA_TABLE_ENTRY *m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        UNICODE_STRING *full = &m->FullDllName;
        int n = full->Length / 2, k = (int)wcslen(name);
        if (n >= k && !_wcsnicmp(full->Buffer + n - k, name, k))
            return (HMODULE)m->DllBase;
    }
    return NULL;
}

static const char *census[] = {
    /* DLL, then its functions, "" ends a DLL; "*" ends the list */
    "kernel32.dll", "VirtualAlloc", "VirtualProtect", "LoadLibraryW", "LoadLibraryExW", "CreateFileW",
    "CreateFileMappingW", "MapViewOfFile", "GetModuleHandleW", "CreateProcessW", "GetConsoleMode", "",
    "kernelbase.dll", "VirtualAlloc", "VirtualProtect", "LoadLibraryExW", "MapViewOfFile3", "VirtualAlloc2", "",
    "user32.dll", "CreateWindowExW", "RegisterClassExW", "GetDC", "PeekMessageW", "GetSystemMetrics",
    "MessageBoxW", "SetWindowsHookExW", "GetRawInputDeviceList", "",
    "gdi32.dll", "ChoosePixelFormat", "SetPixelFormat", "DescribePixelFormat", "SwapBuffers", "StretchDIBits",
    "CreateDIBSection", "",
    "opengl32.dll", "wglCreateContext", "",
    "d3d12.dll", "D3D12CreateDevice", "D3D12GetDebugInterface", "D3D12EnableExperimentalFeatures", "",
    "dxgi.dll", "CreateDXGIFactory2", "", "d3dcompiler_47.dll", "D3DCompile", "", "dxcompiler.dll", "", "dxil.dll", "",
    "xinput1_4.dll", "XInputGetState", "", "xinputuap.dll", "XInputGetState", "", "winmm.dll", "timeGetTime", "",
    "ws2_32.dll", "WSAStartup", "", "iphlpapi.dll", "GetAdaptersAddresses", "", "crypt32.dll", "", "bcrypt.dll", "",
    "advapi32.dll", "RegOpenKeyExW", "CryptAcquireContextW", "", "shell32.dll", "SHGetKnownFolderPath", "",
    "ole32.dll", "CoInitializeEx", "", "setupapi.dll", "", "hid.dll", "", "dwmapi.dll", "", "imm32.dll", "",
    "version.dll", "", "shlwapi.dll", "", "ucrtbase.dll", "", "msvcrt.dll", "", "dbghelp.dll", "", "winhttp.dll", "",
    "xaudio2_9.dll", "XAudio2Create", "", "mfplat.dll", "", "dsound.dll", "", "uxtheme.dll", "", "comctl32.dll", "",
    "*"};

static void probe_f(void)
{
    say("F  platform census");
    HMODULE kb = loaded_module(L"kernelbase.dll");
    load_ex = kb ? (LoadLibraryExW_t)GetProcAddress(kb, "LoadLibraryExW") : NULL;
    say("   kernelbase %p, LoadLibraryExW %p", kb, load_ex);
    if (!load_ex)
        return;
    char line[2048];
    for (int i = 0; strcmp(census[i], "*"); i++) {
        WCHAR w[64];
        MultiByteToWideChar(CP_ACP, 0, census[i], -1, w, 64);
        HMODULE m = load_ex(w, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        DWORD err = m ? 0 : GetLastError();
        int n = snprintf(line, sizeof line, "   %-18s %s", census[i], m ? "LOADS" : "missing");
        if (!m)
            n += snprintf(line + n, sizeof line - n, " (error %lu)", err);
        for (i++; census[i][0]; i++)
            if (m)
                n += snprintf(line + n, sizeof line - n, "  %s%s", GetProcAddress(m, census[i]) ? "+" : "-", census[i]);
        say("%s", line);
    }

    /* can a Win32 window be made at all? (Mesa's pbuffers and SDL's video driver both make one) */
    HMODULE u = load_ex(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (u) {
        typedef ATOM(WINAPI * Reg_t)(const WNDCLASSEXW *);
        typedef HWND(WINAPI * Create_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
        typedef LRESULT(WINAPI * Def_t)(HWND, UINT, WPARAM, LPARAM);
        Reg_t reg = (Reg_t)GetProcAddress(u, "RegisterClassExW");
        Create_t create = (Create_t)GetProcAddress(u, "CreateWindowExW");
        Def_t def = (Def_t)GetProcAddress(u, "DefWindowProcW");
        if (reg && create && def) {
            WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = (WNDPROC)def, .lpszClassName = L"probe"};
            ATOM a = reg(&wc);
            DWORD e1 = a ? 0 : GetLastError();
            HWND h = create(0, L"probe", L"probe", 0, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
            say("   Win32 window: RegisterClassExW %s (error %lu), CreateWindowExW %p (error %lu)", a ? "ok" : "FAILED",
                e1, h, h ? 0 : GetLastError());
        }
    }

    /* D3D12: a device, its feature level and shader model */
    HMODULE d12 = load_ex(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (d12) {
        typedef HRESULT(WINAPI * Create12_t)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
        Create12_t c12 = (Create12_t)GetProcAddress(d12, "D3D12CreateDevice");
        static const GUID IID_D3D12Device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
        IUnknown *dev = NULL;
        D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                   D3D_FEATURE_LEVEL_11_0};
        for (int k = 0; c12 && k < 4; k++) {
            HRESULT hr = c12(NULL, fls[k], &IID_D3D12Device, (void **)&dev);
            say("   D3D12CreateDevice(FL %X): hr %08lX", (unsigned)fls[k], (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                /* ID3D12Device::CheckFeatureSupport is vtable slot 13; D3D12_FEATURE_SHADER_MODEL = 7 */
                struct { int highest; } sm = {0x66};
                typedef HRESULT(STDMETHODCALLTYPE * Check_t)(IUnknown *, int, void *, UINT);
                Check_t check = (Check_t)(*(void ***)dev)[13];
                HRESULT h2 = check(dev, 7, &sm, sizeof sm);
                say("   D3D12 highest shader model %X (hr %08lX)", sm.highest, (unsigned long)h2);
                dev->lpVtbl->Release(dev);
                break;
            }
        }
    }
}

/* G: every import of the exe being ported (probe/imports.inc, tools/imports_inc.py), checked one by one: a
   function the console does not export fails the whole launch, silently. Only the misses are printed. */
static const char *imports[] = {
#include "imports.inc"
    "dxcore.dll", "DXCoreCreateAdapterFactory", "",
    "*"};

static void probe_g(void)
{
    say("G  import census (only misses listed)");
    if (!load_ex)
        return;
    int dlls = 0, fns = 0, miss = 0;
    for (int i = 0; strcmp(imports[i], "*"); i++) {
        WCHAR w[96];
        MultiByteToWideChar(CP_ACP, 0, imports[i], -1, w, 96);
        const char *dll = imports[i];
        HMODULE m = load_ex(w, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        dlls++;
        if (!m)
            say("   %s: DLL MISSING (error %lu)", dll, GetLastError());
        for (i++; imports[i][0]; i++) {
            fns++;
            if (m && !GetProcAddress(m, imports[i])) {
                say("   %s!%s MISSING", dll, imports[i]);
                miss++;
            }
        }
    }
    say("   %d DLLs, %d functions, %d functions missing", dlls, fns, miss);
}

/* H: Mesa's OSMesa (packaged as opengl32.dll) -- a GL 4.x core context, as xemu asks for, on d3d12 and on
   softpipe. It draws a clear and reads the buffer back, so the result is a pixel, not just a version string. */
#define OSMESA_RGBA 0x1908
#define OSMESA_FORMAT 0x22
#define OSMESA_DEPTH_BITS 0x30
#define OSMESA_STENCIL_BITS 0x31
#define OSMESA_PROFILE 0x33
#define OSMESA_CORE_PROFILE 0x34
#define OSMESA_CONTEXT_MAJOR_VERSION 0x36
#define OSMESA_CONTEXT_MINOR_VERSION 0x37
static void probe_h_one(HMODULE gl, const char *driver)
{
    typedef void *(WINAPI * Create_t)(const int *, void *);
    typedef int(WINAPI * Make_t)(void *, void *, unsigned, int, int);
    typedef void *(WINAPI * Proc_t)(const char *);
    typedef void(WINAPI * Destroy_t)(void *);
    Create_t create = (Create_t)GetProcAddress(gl, "OSMesaCreateContextAttribs");
    Make_t make = (Make_t)GetProcAddress(gl, "OSMesaMakeCurrent");
    Proc_t proc = (Proc_t)GetProcAddress(gl, "OSMesaGetProcAddress");
    Destroy_t destroy = (Destroy_t)GetProcAddress(gl, "OSMesaDestroyContext");
    if (!create || !make || !proc)
        return;
    SetEnvironmentVariableA("GALLIUM_DRIVER", driver);
    int attrs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
                   OSMESA_PROFILE, OSMESA_CORE_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 4,
                   OSMESA_CONTEXT_MINOR_VERSION, 0, 0};
    say("   [%s] creating a GL 4.0 core context", driver);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    void *ctx = create(attrs, NULL);
    if (!ctx) {
        say("   [%s] OSMesaCreateContextAttribs FAILED", driver);
        return;
    }
    static unsigned px[64 * 64];
    if (!make(ctx, px, 0x1401 /* GL_UNSIGNED_BYTE */, 64, 64)) {
        say("   [%s] OSMesaMakeCurrent FAILED", driver);
        return;
    }
    typedef const char *(WINAPI * Str_t)(unsigned);
    typedef void(WINAPI * Clr_t)(float, float, float, float);
    typedef void(WINAPI * U_t)(unsigned);
    typedef void(WINAPI * V_t)(void);
    Str_t gs = (Str_t)proc("glGetString");
    Clr_t cc = (Clr_t)proc("glClearColor");
    U_t clr = (U_t)proc("glClear");
    V_t fin = (V_t)proc("glFinish");
    say("   [%s] GL_RENDERER %s | GL_VERSION %s | GLSL %s", driver, gs(0x1F01), gs(0x1F02), gs(0x8B8C));
    cc(0.0f, 1.0f, 0.0f, 1.0f);
    clr(0x4000);
    fin();
    QueryPerformanceCounter(&t1);
    say("   [%s] clear -> pixel %08X (want FF00FF00), %.1f ms; wglGetProcAddress(glGenVertexArrays) %p", driver,
        px[0], (double)(t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart,
        ((Proc_t)GetProcAddress(gl, "wglGetProcAddress"))("glGenVertexArrays"));
    make(NULL, NULL, 0, 0, 0);
    destroy(ctx);
}

static void probe_h(void)
{
    say("H  Mesa OSMesa (opengl32.dll from the package)");
    if (!load_ex)
        return;
    /* by full path: on a PC the census above has already loaded the system's opengl32.dll, and a load by name
       would hand that back */
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\')
        n--;
    wcscpy(path + n, L"opengl32.dll");
    HMODULE gl = load_ex(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!gl) {
        say("   opengl32.dll from the package: FAILED (error %lu)", GetLastError());
        return;
    }
    probe_h_one(gl, "d3d12");
    probe_h_one(gl, "softpipe");
}

/* I: files outside the app's folders (removableStorage + broadFileSystemAccess in the manifest). For each drive
   letter: list it with plain FindFirstFileExW and with FindFirstFileExFromApp, then open the first file found
   with plain CreateFileW and with CreateFile2FromApp -- which of the two the console honours decides how xemu's
   file I/O has to be done. */
typedef HANDLE(WINAPI *FindFromApp_t)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
typedef HANDLE(WINAPI *CreateFromApp_t)(LPCWSTR, DWORD, DWORD, DWORD, void *);
static void probe_i(void)
{
    say("I  file access outside the app (broadFileSystemAccess, removableStorage)");
    if (!load_ex)
        return;
    HMODULE fa = load_ex(L"api-ms-win-core-file-fromapp-l1-1-0.dll", NULL, 0);
    FindFromApp_t find_fa = fa ? (FindFromApp_t)GetProcAddress(fa, "FindFirstFileExFromAppW") : NULL;
    CreateFromApp_t create_fa = fa ? (CreateFromApp_t)GetProcAddress(fa, "CreateFile2FromAppW") : NULL;
    say("   file-fromapp api set %p: FindFirstFileExFromApp %p, CreateFile2FromApp %p", fa, find_fa, create_fa);
    DWORD drives = GetLogicalDrives();
    say("   GetLogicalDrives %08lX", drives);
    for (int d = 0; d < 26; d++) {
        WCHAR pat[8] = {(WCHAR)(L'A' + d), L':', L'\\', L'*', 0};
        WIN32_FIND_DATAW fd;
        char line[1024];
        int n = snprintf(line, sizeof line, "   %c: %s", 'A' + d, (drives >> d) & 1 ? "present" : "-");
        HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        n += snprintf(line + n, sizeof line - n, " | plain list %s", h != INVALID_HANDLE_VALUE ? "OK" : "no");
        if (h == INVALID_HANDLE_VALUE)
            n += snprintf(line + n, sizeof line - n, " (%lu)", GetLastError());
        else
            FindClose(h);
        WCHAR first[MAX_PATH] = {0};
        char names[400] = "";
        int nn = 0, count = 0;
        h = find_fa ? find_fa(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
        n += snprintf(line + n, sizeof line - n, " | FromApp list %s", h != INVALID_HANDLE_VALUE ? "OK" : "no");
        if (h == INVALID_HANDLE_VALUE) {
            n += snprintf(line + n, sizeof line - n, " (%lu)", GetLastError());
        } else {
            do {
                if (count < 8 && nn < (int)sizeof names - 80)
                    nn += snprintf(names + nn, sizeof names - nn, " %ls%s", fd.cFileName,
                                   fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "\\" : "");
                if (!first[0] && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    swprintf(first, MAX_PATH, L"%c:\\%ls", L'A' + d, fd.cFileName);
                count++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            n += snprintf(line + n, sizeof line - n, " [%d]%s", count, names);
        }
        if (first[0]) {
            HANDLE f = CreateFileW(first, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            n += snprintf(line + n, sizeof line - n, " | open %ls: plain %s", first,
                          f != INVALID_HANDLE_VALUE ? "OK" : "no");
            if (f != INVALID_HANDLE_VALUE)
                CloseHandle(f);
            f = create_fa ? create_fa(first, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL) : INVALID_HANDLE_VALUE;
            n += snprintf(line + n, sizeof line - n, ", FromApp %s", f != INVALID_HANDLE_VALUE ? "OK" : "no");
            if (f != INVALID_HANDLE_VALUE)
                CloseHandle(f);
        }
        if (((drives >> d) & 1) || strstr(line, "OK"))
            say("%s", line);
    }

    /* D:\DevelopmentFiles, two levels: what is there, and can a real file be opened (plain vs FromApp)? */
    static const WCHAR *roots[] = {L"D:\\DevelopmentFiles", L"D:\\Drives"};
    for (int r = 0; r < 2 && find_fa; r++) {
        WCHAR pat[MAX_PATH];
        WIN32_FIND_DATAW fd;
        swprintf(pat, MAX_PATH, L"%ls\\*", roots[r]);
        HANDLE h = find_fa(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        if (h == INVALID_HANDLE_VALUE) {
            say("   %ls: FromApp list no (%lu)", roots[r], GetLastError());
            continue;
        }
        do {
            if (fd.cFileName[0] == L'.')
                continue;
            int dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            if (!dir) {
                say("   %ls\\%ls", roots[r], fd.cFileName);
                continue;
            }
            WCHAR sub[MAX_PATH], subpat[MAX_PATH];
            swprintf(sub, MAX_PATH, L"%ls\\%ls", roots[r], fd.cFileName);
            swprintf(subpat, MAX_PATH, L"%ls\\*", sub);
            WIN32_FIND_DATAW fd2;
            HANDLE h2 = find_fa(subpat, FindExInfoBasic, &fd2, FindExSearchNameMatch, NULL, 0);
            char names[600] = "";
            int nn = 0, count = 0;
            WCHAR file[MAX_PATH] = {0};
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (fd2.cFileName[0] == L'.')
                        continue;
                    if (count < 10 && nn < (int)sizeof names - 80)
                        nn += snprintf(names + nn, sizeof names - nn, " %ls%s", fd2.cFileName,
                                       fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "\\" : "");
                    if (!file[0] && !(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        swprintf(file, MAX_PATH, L"%ls\\%ls", sub, fd2.cFileName);
                    count++;
                } while (FindNextFileW(h2, &fd2));
                FindClose(h2);
            }
            char open[300] = "";
            if (file[0]) {
                HANDLE f1 = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                DWORD e1 = f1 == INVALID_HANDLE_VALUE ? GetLastError() : 0;
                HANDLE f2 = create_fa(file, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NULL);
                DWORD e2 = f2 == INVALID_HANDLE_VALUE ? GetLastError() : 0;
                snprintf(open, sizeof open, " | open %ls: plain %s(%lu) FromApp %s(%lu)", file,
                         f1 != INVALID_HANDLE_VALUE ? "OK" : "no", e1, f2 != INVALID_HANDLE_VALUE ? "OK" : "no", e2);
                if (f1 != INVALID_HANDLE_VALUE)
                    CloseHandle(f1);
                if (f2 != INVALID_HANDLE_VALUE)
                    CloseHandle(f2);
            }
            say("   %ls\\ %s [%d]%s%s", sub, h2 != INVALID_HANDLE_VALUE ? "lists" : "NO LIST", count, names, open);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void run_probes(void)
{
    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s\\probe.log", local_dir);
    logf = fopen(path, "w");
    say("jitprobe: log %s", path);
    probe_d();
    probe_e();
    probe_b();
    probe_c(PAGE_EXECUTE_READWRITE, "PAGE_EXECUTE_READWRITE");
    probe_c(PAGE_READWRITE, "PAGE_READWRITE");
    probe_f();
    probe_g();
    probe_h();
    probe_i();
    probe_a();                                  /* last: the likeliest to crash outright */
    say("jitprobe: done");
}

/* ------------------------------------------------------------------ the minimal CoreApplication */

#define STUBS(prefix, T)                                                                                          \
    static ULONG STDMETHODCALLTYPE prefix##_addref(T *This) { (void)This; return 2; }                            \
    static ULONG STDMETHODCALLTYPE prefix##_release(T *This) { (void)This; return 1; }                           \
    static HRESULT STDMETHODCALLTYPE prefix##_iids(T *This, ULONG *n, IID **iids)                                  \
    {                                                                                                             \
        (void)This;                                                                                               \
        *n = 0;                                                                                                   \
        *iids = NULL;                                                                                             \
        return S_OK;                                                                                              \
    }                                                                                                             \
    static HRESULT STDMETHODCALLTYPE prefix##_name(T *This, HSTRING *s) { (void)This; *s = NULL; return S_OK; }  \
    static HRESULT STDMETHODCALLTYPE prefix##_trust(T *This, TrustLevel *t) { (void)This; *t = BaseTrust; return S_OK; }

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
STUBS(view, View)
static HRESULT STDMETHODCALLTYPE view_initialize(View *This, AppView *app) { (void)This, (void)app; return S_OK; }
static HRESULT STDMETHODCALLTYPE view_set_window(View *This, CoreWindow *w)
{
    (void)This;
    window = w;
    __x_ABI_CWindows_CUI_CCore_CICoreWindow_AddRef(w);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_load(View *This, HSTRING e) { (void)This, (void)e; return S_OK; }
static HRESULT STDMETHODCALLTYPE view_run(View *This)
{
    (void)This;
    __x_ABI_CWindows_CUI_CCore_CICoreWindow_Activate(window);
    run_probes();
    Sleep(1500);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_uninitialize(View *This) { (void)This; return S_OK; }
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
STUBS(source, ViewSource)
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
    find_local_dir();
    if (argc > 1 && !strcmp(argv[1], "--direct")) {   /* a PC run, unpackaged: no window */
        run_probes();
        return 0;
    }
    __x_ABI_CWindows_CApplicationModel_CCore_CICoreApplication *app =
        factory(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication,
                &IID___x_ABI_CWindows_CApplicationModel_CCore_CICoreApplication);
    if (!app)
        return 3;
    __x_ABI_CWindows_CApplicationModel_CCore_CICoreApplication_Run(app, &the_source);
    return 0;
}
