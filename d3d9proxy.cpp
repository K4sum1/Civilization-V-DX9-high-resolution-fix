#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>

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

// IDirect3DDevice9 vtable slots
constexpr int VTABLE_SLOT_RESET = 16;
constexpr int VTABLE_SLOT_CREATE_DEPTH = 29;
// IDirect3D9 vtable slots
constexpr int VTABLE_SLOT_CREATE_DEVICE = 16;

// Backbuffer dimension tracking with proper synchronization
struct BackBufferDim {
    UINT width;
    UINT height;
};

static HMODULE gSystemD3D9 = nullptr;
static std::once_flag gLoadOnce;

static std::atomic<BackBufferDim> gBackBuffer{ {0, 0} };
static std::mutex gBackBufferMutex;

static CreateDeviceFn gRealCreateDevice = nullptr;
static ResetFn gRealReset = nullptr;
static CreateDepthFn gRealCreateDepth = nullptr;

static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "d3d9proxy.log", "a") != 0)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fclose(f);
}

static void LoadSystemD3D9()
{
    wchar_t path[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 12) {
        Log("ERROR: Failed to get system directory or path too long\n");
        return;
    }

    wcscat_s(path, L"\\d3d9.dll");
    gSystemD3D9 = LoadLibraryW(path);

    if (!gSystemD3D9) {
        Log("ERROR: Failed to load system d3d9.dll from %ls\n", path);
    } else {
        Log("d3d9 proxy initialized successfully\n");
    }
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
    if (!vtable)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("ERROR: VirtualProtect failed for vtable slot %d\n", index);
        return false;
    }

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
    if (w == 0 || h == 0)
        return; // Ignore zero dimensions

    std::lock_guard<std::mutex> lock(gBackBufferMutex);
    gBackBuffer.store({ w, h }, std::memory_order_release);
    Log("Backbuffer dimensions updated: %ux%u\n", w, h);
}

static BackBufferDim GetBackBufferSize()
{
    std::lock_guard<std::mutex> lock(gBackBufferMutex);
    return gBackBuffer.load(std::memory_order_acquire);
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
        if (SUCCEEDED(surface->GetDesc(&desc))) {
            RememberBackBufferSize(desc.Width, desc.Height);
        } else {
            Log("ERROR: Failed to get backbuffer surface descriptor\n");
        }

        surface->Release();
    } else {
        Log("ERROR: Failed to get backbuffer surface\n");
    }
}

static UINT RoundUp2048(UINT value)
{
    constexpr UINT64 bucket = 2048;
    const UINT64 rounded =
        ((static_cast<UINT64>(value) + bucket - 1) / bucket) * bucket;

    if (rounded > (std::numeric_limits<UINT>::max)()) {
        Log("WARNING: RoundUp2048 overflow for value %u, returning original\n", value);
        return value;
    }

    return static_cast<UINT>(rounded);
}

static HRESULT WINAPI HookReset(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
{
    if (pp) {
        RememberBackBufferSize(pp->BackBufferWidth, pp->BackBufferHeight);
    } else {
        // Fallback: query dimensions from device if pp is null
        RememberBackBuffer(device);
    }

    if (!gRealReset) {
        Log("ERROR: gRealReset not initialized in HookReset\n");
        return D3DERR_INVALIDCALL;
    }

    HRESULT hr = gRealReset(device, pp);

    if (SUCCEEDED(hr)) {
        RememberBackBuffer(device);
    } else {
        Log("WARNING: Reset failed with HRESULT 0x%08X\n", static_cast<UINT>(hr));
    }

    return hr;
}

static HRESULT WINAPI HookCreateDepth(
    IDirect3DDevice9* device,
    UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL discard,
    IDirect3DSurface9** surface, HANDLE* sharedHandle)
{
    if (!gRealCreateDepth) {
        Log("ERROR: gRealCreateDepth not initialized in HookCreateDepth\n");
        return D3DERR_INVALIDCALL;
    }

    if (width == 2048 && height == 2048) {
        BackBufferDim bbDim = GetBackBufferSize();

        const UINT requiredWidth = (std::max)(width, bbDim.width);
        const UINT requiredHeight = (std::max)(height, bbDim.height);

        const UINT roundedWidth = RoundUp2048(requiredWidth);
        const UINT roundedHeight = RoundUp2048(requiredHeight);

        // Validate rounding result
        if (roundedWidth < requiredWidth || roundedHeight < requiredHeight) {
            Log("ERROR: RoundUp2048 produced invalid result: "
                "required %ux%u -> rounded %ux%u\n",
                requiredWidth, requiredHeight, roundedWidth, roundedHeight);
        } else if (roundedWidth > width || roundedHeight > height) {
            Log("Scaling depth surface %ux%u -> %ux%u (backbuffer: %ux%u)\n",
                width, height, roundedWidth, roundedHeight, bbDim.width, bbDim.height);

            HRESULT hr = gRealCreateDepth(
                device, roundedWidth, roundedHeight, format,
                multisample, quality, discard, surface, sharedHandle);

            if (SUCCEEDED(hr)) {
                Log("Successfully allocated rounded depth surface %ux%u\n",
                    roundedWidth, roundedHeight);
                return hr;
            }

            Log("Rounded allocation failed: 0x%08X\n", static_cast<UINT>(hr));

            if (requiredWidth != roundedWidth || requiredHeight != roundedHeight) {
                Log("Attempting exact allocation: %ux%u\n", requiredWidth, requiredHeight);
                hr = gRealCreateDepth(
                    device, requiredWidth, requiredHeight, format,
                    multisample, quality, discard, surface, sharedHandle);

                if (SUCCEEDED(hr)) {
                    Log("Successfully allocated exact depth surface %ux%u\n",
                        requiredWidth, requiredHeight);
                    return hr;
                }

                Log("Exact allocation failed: 0x%08X\n", static_cast<UINT>(hr));
            }

            Log("Falling back to original depth size %ux%u\n", width, height);
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

    if (!gRealCreateDevice) {
        Log("ERROR: gRealCreateDevice not initialized in HookCreateDevice\n");
        return D3DERR_INVALIDCALL;
    }

    HRESULT hr = gRealCreateDevice(
        d3d, adapter, type, focusWindow, behavior, pp, device);

    if (SUCCEEDED(hr) && device && *device) {
        RememberBackBuffer(*device);

        // In-place vtable patch, simpler and less crash-prone than cloning.
        // IDirect3DDevice9 vtable slots:
        // Reset = 16
        // CreateDepthStencilSurface = 29

        if (!PatchSlot(*device, VTABLE_SLOT_RESET,
                       reinterpret_cast<void*>(HookReset),
                       reinterpret_cast<void**>(&gRealReset))) {
            Log("ERROR: Failed to patch Reset vtable slot\n");
            return D3DERR_INVALIDCALL;
        }

        if (!PatchSlot(*device, VTABLE_SLOT_CREATE_DEPTH,
                       reinterpret_cast<void*>(HookCreateDepth),
                       reinterpret_cast<void**>(&gRealCreateDepth))) {
            Log("ERROR: Failed to patch CreateDepthStencilSurface vtable slot\n");
            return D3DERR_INVALIDCALL;
        }

        Log("Device hooks installed successfully\n");
    } else {
        if (!SUCCEEDED(hr)) {
            Log("WARNING: CreateDevice failed with HRESULT 0x%08X\n", static_cast<UINT>(hr));
        }
    }

    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion)
{
    Log("Direct3DCreate9 called with SDK version %u\n", sdkVersion);

    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9) {
        Log("ERROR: System d3d9.dll not loaded\n");
        return nullptr;
    }

    auto realCreate9 = reinterpret_cast<Create9Fn>(
        GetProcAddress(systemD3D9, "Direct3DCreate9"));

    if (!realCreate9) {
        Log("ERROR: Failed to get Direct3DCreate9 from system d3d9.dll\n");
        return nullptr;
    }

    IDirect3D9* d3d = realCreate9(sdkVersion);
    if (!d3d) {
        Log("ERROR: Direct3DCreate9 returned null\n");
        return nullptr;
    }

    // IDirect3D9::CreateDevice = slot 16
    if (!PatchSlot(d3d, VTABLE_SLOT_CREATE_DEVICE,
                   reinterpret_cast<void*>(HookCreateDevice),
                   reinterpret_cast<void**>(&gRealCreateDevice))) {
        Log("ERROR: Failed to patch CreateDevice vtable slot\n");
        d3d->Release();
        return nullptr;
    }

    Log("IDirect3D9 proxy created and patched successfully\n");
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(
    UINT sdkVersion, IDirect3D9Ex** out)
{
    Log("Direct3DCreate9Ex called with SDK version %u\n", sdkVersion);

    if (!out) {
        Log("ERROR: Direct3DCreate9Ex called with null output pointer\n");
        return D3DERR_INVALIDCALL;
    }

    *out = nullptr;

    HMODULE systemD3D9 = GetSystemD3D9();
    if (!systemD3D9) {
        Log("ERROR: System d3d9.dll not loaded\n");
        return D3DERR_NOTAVAILABLE;
    }

    auto realCreate9Ex = reinterpret_cast<Create9ExFn>(
        GetProcAddress(systemD3D9, "Direct3DCreate9Ex"));

    if (!realCreate9Ex) {
        Log("ERROR: Failed to get Direct3DCreate9Ex from system d3d9.dll\n");
        return D3DERR_NOTAVAILABLE;
    }

    HRESULT hr = realCreate9Ex(sdkVersion, out);
    if (SUCCEEDED(hr)) {
        Log("Direct3DCreate9Ex succeeded\n");
    } else {
        Log("ERROR: Direct3DCreate9Ex failed with HRESULT 0x%08X\n", static_cast<UINT>(hr));
    }

    return hr;
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
