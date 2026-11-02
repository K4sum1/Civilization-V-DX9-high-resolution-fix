#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>
#include <cstdarg>

using Create9Fn = IDirect3D9* (WINAPI*)(UINT);
using Create9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
using CreateDeviceFn = HRESULT (WINAPI*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using ResetFn = HRESULT (WINAPI*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateDepthFn = HRESULT (WINAPI*)(
    IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DMULTISAMPLE_TYPE, DWORD, BOOL,
    IDirect3DSurface9**, HANDLE*);

using PerfBeginEventFn = int (WINAPI*)(D3DCOLOR, LPCWSTR);
using PerfEndEventFn = int (WINAPI*)();
using PerfSetMarkerFn = void (WINAPI*)(D3DCOLOR, LPCWSTR);
using PerfSetRegionFn = void (WINAPI*)(D3DCOLOR, LPCWSTR);
using PerfSetOptionsFn = void (WINAPI*)(DWORD);
using PerfGetStatusFn = DWORD (WINAPI*)();
using PerfQueryRepeatFrameFn = BOOL (WINAPI*)();

static HMODULE gSystemD3D9 = nullptr;
static std::once_flag gLoadOnce;

static std::atomic<UINT> gBackBufferWidth{ 0 };
static std::atomic<UINT> gBackBufferHeight{ 0 };

static CreateDeviceFn gRealCreateDevice = nullptr;
static ResetFn gRealReset = nullptr;
static CreateDepthFn gRealCreateDepth = nullptr;

static void Log(const char* fmt, ...)
{
#ifdef _DEBUG
    FILE* f = nullptr;
    fopen_s(&f, "Civ5DepthFix.log", "a");
    if (!f)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fclose(f);
#else
    (void)fmt;
#endif
}

static void LoadSystemD3D9()
{
    wchar_t path[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 12)
        return;

    wcscat_s(path, L"\\d3d9.dll");
    gSystemD3D9 = LoadLibraryW(path);
}

static HMODULE GetSystemD3D9()
{
    std::call_once(gLoadOnce, LoadSystemD3D9);
    return gSystemD3D9;
}

static bool PatchSlot(void* object, int index, void* replacement, void** original)
{
    if (!object)
        return false;

    void** vtable = *reinterpret_cast<void***>(object);
    DWORD oldProtect = 0;

    if (!VirtualProtect(&vtable[index], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    if (vtable[index] != replacement) {
        if (original && !*original)
            *original = vtable[index];

        vtable[index] = replacement;
    }

    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    return true;
}

static void RememberBackBufferSize(UINT w, UINT h)
{
    gBackBufferWidth.store(w, std::memory_order_relaxed);
    gBackBufferHeight.store(h, std::memory_order_relaxed);
}

static void RememberBackBuffer(IDirect3DDevice9* device)
{
    if (!device) {
        RememberBackBufferSize(0, 0);
        return;
    }

    IDirect3DSurface9* surface = nullptr;

    if (SUCCEEDED(device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface)
    {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(surface->GetDesc(&desc)))
            RememberBackBufferSize(desc.Width, desc.Height);

        surface->Release();
    }
}

static UINT RoundUp2048(UINT value)
{
    constexpr UINT64 bucket = 2048;
    const UINT64 rounded =
        ((static_cast<UINT64>(value) + bucket - 1) / bucket) * bucket;

    if (rounded > (std::numeric_limits<UINT>::max)())
        return value;

    return static_cast<UINT>(rounded);
}

static HRESULT WINAPI HookReset(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
{
    if (pp) {
        RememberBackBufferSize(pp->BackBufferWidth, pp->BackBufferHeight);
    }

    if (!gRealReset)
        return D3DERR_INVALIDCALL;

    HRESULT hr = gRealReset(device, pp);

    if (SUCCEEDED(hr))
        RememberBackBuffer(device);

    return hr;
}

static HRESULT WINAPI HookCreateDepth(
    IDirect3DDevice9* device,
    UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL discard,
    IDirect3DSurface9** surface, HANDLE* sharedHandle)
{
    if (!gRealCreateDepth)
        return D3DERR_INVALIDCALL;

    // Only special-case the known problematic 2048x2048 depth surface.
    if (width == 2048 && height == 2048) {
        const UINT bbWidth = gBackBufferWidth.load(std::memory_order_relaxed);
        const UINT bbHeight = gBackBufferHeight.load(std::memory_order_relaxed);

        const UINT requiredWidth = (std::max)(width, bbWidth);
        const UINT requiredHeight = (std::max)(height, bbHeight);

        const UINT roundedWidth = RoundUp2048(requiredWidth);
        const UINT roundedHeight = RoundUp2048(requiredHeight);

        if (roundedWidth > width || roundedHeight > height) {
            Log("Depth surface %ux%u -> %ux%u\n",
                width, height, roundedWidth, roundedHeight);

            HRESULT hr = gRealCreateDepth(
                device, roundedWidth, roundedHeight, format,
                multisample, quality, discard, surface, sharedHandle);

            if (SUCCEEDED(hr))
                return hr;

            Log("Rounded allocation failed: 0x%08X\n", static_cast<UINT>(hr));

            if (requiredWidth != roundedWidth || requiredHeight != roundedHeight) {
                hr = gRealCreateDepth(
                    device, requiredWidth, requiredHeight, format,
                    multisample, quality, discard, surface, sharedHandle);

                if (SUCCEEDED(hr))
                    return hr;

                Log("Exact allocation failed: 0x%08X\n", static_cast<UINT>(hr));
            }

            Log("Falling back to original depth size\n");
        }
    }

    return gRealCreateDepth(
        device, width, height, format,
        multisample, quality, discard, surface, sharedHandle);
}

static HRESULT WINAPI HookCreateDevice(
    IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focusWindow,
    DWORD behavior, D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** device)
{
    if (pp) {
        RememberBackBufferSize(pp->BackBufferWidth, pp->BackBufferHeight);
    }

    if (!gRealCreateDevice)
        return D3DERR_INVALIDCALL;

    HRESULT hr = gRealCreateDevice(
        d3d, adapter, type, focusWindow, behavior, pp, device);

    if (SUCCEEDED(hr) && device && *device) {
        RememberBackBuffer(*device);

        // IDirect3DDevice9 vtable:
        // Reset = 16
        // CreateDepthStencilSurface = 29
        PatchSlot(*device, 16, reinterpret_cast<void*>(HookReset),
                  reinterpret_cast<void**>(&gRealReset));
        PatchSlot(*device, 29, reinterpret_cast<void*>(HookCreateDepth),
                  reinterpret_cast<void**>(&gRealCreateDepth));
    }

    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion)
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return nullptr;

    auto realCreate9 = reinterpret_cast<Create9Fn>(
        GetProcAddress(systemD3D9, "Direct3DCreate9"));

    if (!realCreate9)
        return nullptr;

    IDirect3D9* d3d = realCreate9(sdkVersion);

    if (d3d) {
        // IDirect3D9::CreateDevice = vtable slot 16
        PatchSlot(d3d, 16, reinterpret_cast<void*>(HookCreateDevice),
                  reinterpret_cast<void**>(&gRealCreateDevice));
    }

    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(
    UINT sdkVersion, IDirect3D9Ex** out)
{
    if (!out)
        return D3DERR_INVALIDCALL;

    *out = nullptr;

    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return D3DERR_NOTAVAILABLE;

    auto realCreate9Ex = reinterpret_cast<Create9ExFn>(
        GetProcAddress(systemD3D9, "Direct3DCreate9Ex"));

    if (!realCreate9Ex)
        return D3DERR_NOTAVAILABLE;

    return realCreate9Ex(sdkVersion, out);
}

extern "C" int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return -1;

    auto fn = reinterpret_cast<PerfBeginEventFn>(
        GetProcAddress(systemD3D9, "D3DPERF_BeginEvent"));

    return fn ? fn(color, name) : -1;
}

extern "C" int WINAPI Proxy_D3DPERF_EndEvent()
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return -1;

    auto fn = reinterpret_cast<PerfEndEventFn>(
        GetProcAddress(systemD3D9, "D3DPERF_EndEvent"));

    return fn ? fn() : -1;
}

extern "C" void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return;

    auto fn = reinterpret_cast<PerfSetMarkerFn>(
        GetProcAddress(systemD3D9, "D3DPERF_SetMarker"));

    if (fn) fn(color, name);
}

extern "C" void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name)
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return;

    auto fn = reinterpret_cast<PerfSetRegionFn>(
        GetProcAddress(systemD3D9, "D3DPERF_SetRegion"));

    if (fn) fn(color, name);
}

extern "C" BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame()
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return FALSE;

    auto fn = reinterpret_cast<PerfQueryRepeatFrameFn>(
        GetProcAddress(systemD3D9, "D3DPERF_QueryRepeatFrame"));

    return fn ? fn() : FALSE;
}

extern "C" void WINAPI Proxy_D3DPERF_SetOptions(DWORD options)
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return;

    auto fn = reinterpret_cast<PerfSetOptionsFn>(
        GetProcAddress(systemD3D9, "D3DPERF_SetOptions"));

    if (fn) fn(options);
}

extern "C" DWORD WINAPI Proxy_D3DPERF_GetStatus()
{
    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9)
        return 0;

    auto fn = reinterpret_cast<PerfGetStatusFn>(
        GetProcAddress(systemD3D9, "D3DPERF_GetStatus"));

    return fn ? fn() : 0;
}
