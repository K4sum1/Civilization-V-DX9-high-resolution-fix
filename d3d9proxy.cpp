#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

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

// IDirect3D9::CreateDevice is also inherited by IDirect3D9Ex.
constexpr int VTABLE_SLOT_CREATE_DEVICE = 16;

struct BackBufferDim {
    UINT width = 0;
    UINT height = 0;
};

static HMODULE gSystemD3D9 = nullptr;
static std::once_flag gLoadOnce;

static BackBufferDim gBackBuffer{};
static std::mutex gBackBufferMutex;

// In-place vtable patching is retained, but originals are associated with
// each vtable instead of being stored in one process-wide function pointer.
using OriginalFunctions = std::unordered_map<void**, void*>;

static OriginalFunctions gCreateDeviceOriginals;
static OriginalFunctions gResetOriginals;
static OriginalFunctions gCreateDepthOriginals;
static std::mutex gVtableMutex;

static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "d3d9proxy.log", "a") != 0 || !f)
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

    if (wcscat_s(path, L"\\d3d9.dll") != 0) {
        Log("ERROR: Failed to construct system d3d9.dll path\n");
        return;
    }

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

static void** GetVtable(void* object)
{
    if (!object)
        return nullptr;

    return *reinterpret_cast<void***>(object);
}

static bool PatchSlot(
    void* object,
    int index,
    void* replacement,
    OriginalFunctions& originals)
{
    void** vtable = GetVtable(object);
    if (!vtable)
        return false;

    std::lock_guard<std::mutex> lock(gVtableMutex);

    if (vtable[index] == replacement) {
        // It should already have been recorded when this vtable was patched.
        return originals.find(vtable) != originals.end();
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            &vtable[index], sizeof(void*),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("ERROR: VirtualProtect failed for vtable slot %d\n", index);
        return false;
    }

    try {
        originals.emplace(vtable, vtable[index]);
    } catch (...) {
        DWORD ignored = 0;
        VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
        Log("ERROR: Could not save original function for vtable slot %d\n", index);
        return false;
    }

    vtable[index] = replacement;

    DWORD ignored = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored))
        Log("WARNING: Could not restore vtable page protection for slot %d\n", index);

    return true;
}

static void* GetOriginal(void* object, OriginalFunctions& originals)
{
    void** vtable = GetVtable(object);
    if (!vtable)
        return nullptr;

    std::lock_guard<std::mutex> lock(gVtableMutex);

    const auto it = originals.find(vtable);
    return it == originals.end() ? nullptr : it->second;
}

static void RememberBackBufferSize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return;

    {
        std::lock_guard<std::mutex> lock(gBackBufferMutex);
        gBackBuffer = { width, height };
    }

    Log("Backbuffer dimensions updated: %ux%u\n", width, height);
}

static BackBufferDim GetBackBufferSize()
{
    std::lock_guard<std::mutex> lock(gBackBufferMutex);
    return gBackBuffer;
}

static bool RememberBackBuffer(IDirect3DDevice9* device)
{
    if (!device)
        return false;

    IDirect3DSurface9* surface = nullptr;
    const HRESULT hr = device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, &surface);

    if (FAILED(hr) || !surface) {
        Log("ERROR: Failed to get backbuffer surface\n");
        return false;
    }

    D3DSURFACE_DESC desc{};
    const HRESULT descHr = surface->GetDesc(&desc);
    surface->Release();

    if (FAILED(descHr)) {
        Log("ERROR: Failed to get backbuffer surface descriptor\n");
        return false;
    }

    RememberBackBufferSize(desc.Width, desc.Height);
    return true;
}

static bool RoundUp2048(UINT value, UINT* result)
{
    if (!result)
        return false;

    constexpr std::uint64_t bucket = 2048;
    const std::uint64_t rounded =
        ((static_cast<std::uint64_t>(value) + bucket - 1) / bucket) * bucket;

    if (rounded > static_cast<std::uint64_t>(MAXUINT)) {
        Log("WARNING: RoundUp2048 overflow for value %u\n", value);
        return false;
    }

    *result = static_cast<UINT>(rounded);
    return true;
}

static HRESULT WINAPI HookReset(
    IDirect3DDevice9* device,
    D3DPRESENT_PARAMETERS* pp)
{
    auto realReset = reinterpret_cast<ResetFn>(
        GetOriginal(device, gResetOriginals));

    if (!realReset) {
        Log("ERROR: Original Reset function not found\n");
        return D3DERR_INVALIDCALL;
    }

    // Do not change the cache before the real reset succeeds.
    const HRESULT hr = realReset(device, pp);

    if (SUCCEEDED(hr)) {
        if (!RememberBackBuffer(device) && pp) {
            // Fallback if querying the backbuffer failed.
            RememberBackBufferSize(pp->BackBufferWidth, pp->BackBufferHeight);
        }
    } else {
        Log("WARNING: Reset failed with HRESULT 0x%08X\n",
            static_cast<UINT>(hr));
    }

    return hr;
}

static HRESULT WINAPI HookCreateDepth(
    IDirect3DDevice9* device,
    UINT width,
    UINT height,
    D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample,
    DWORD quality,
    BOOL discard,
    IDirect3DSurface9** surface,
    HANDLE* sharedHandle)
{
    auto realCreateDepth = reinterpret_cast<CreateDepthFn>(
        GetOriginal(device, gCreateDepthOriginals));

    if (!realCreateDepth) {
        Log("ERROR: Original CreateDepthStencilSurface function not found\n");
        return D3DERR_INVALIDCALL;
    }

    if (width == 2048 && height == 2048) {
        const BackBufferDim bb = GetBackBufferSize();

        const UINT requiredWidth = (std::max)(width, bb.width);
        const UINT requiredHeight = (std::max)(height, bb.height);

        if (requiredWidth > width || requiredHeight > height) {
            Log("Depth surface needs enlargement: original %ux%u, "
                "required %ux%u (backbuffer %ux%u)\n",
                width, height, requiredWidth, requiredHeight,
                bb.width, bb.height);

            // Try the exact required dimensions first to avoid unnecessary
            // memory use from rounding both axes up to a 2048-pixel bucket.
            if (surface)
                *surface = nullptr;

            HRESULT hr = realCreateDepth(
                device, requiredWidth, requiredHeight, format,
                multisample, quality, discard, surface, sharedHandle);

            if (SUCCEEDED(hr)) {
                Log("Allocated exact depth surface %ux%u\n",
                    requiredWidth, requiredHeight);
                return hr;
            }

            Log("Exact depth allocation failed: 0x%08X\n",
                static_cast<UINT>(hr));

            UINT roundedWidth = 0;
            UINT roundedHeight = 0;

            const bool widthRounded =
                RoundUp2048(requiredWidth, &roundedWidth);
            const bool heightRounded =
                RoundUp2048(requiredHeight, &roundedHeight);

            if (widthRounded && heightRounded &&
                (roundedWidth != requiredWidth ||
                 roundedHeight != requiredHeight)) {
                if (surface)
                    *surface = nullptr;

                hr = realCreateDepth(
                    device, roundedWidth, roundedHeight, format,
                    multisample, quality, discard, surface, sharedHandle);

                if (SUCCEEDED(hr)) {
                    Log("Allocated rounded depth surface %ux%u\n",
                        roundedWidth, roundedHeight);
                    return hr;
                }

                Log("Rounded depth allocation failed: 0x%08X\n",
                    static_cast<UINT>(hr));
            }

            Log("Falling back to original depth size %ux%u\n", width, height);
        }
    }

    if (surface)
        *surface = nullptr;

    return realCreateDepth(
        device, width, height, format,
        multisample, quality, discard, surface, sharedHandle);
}

static void InstallDeviceHooks(IDirect3DDevice9* device)
{
    if (!PatchSlot(
            device, VTABLE_SLOT_RESET,
            reinterpret_cast<void*>(HookReset),
            gResetOriginals)) {
        Log("ERROR: Failed to patch Reset vtable slot\n");
    }

    if (!PatchSlot(
            device, VTABLE_SLOT_CREATE_DEPTH,
            reinterpret_cast<void*>(HookCreateDepth),
            gCreateDepthOriginals)) {
        Log("ERROR: Failed to patch CreateDepthStencilSurface vtable slot\n");
    }
}

static HRESULT WINAPI HookCreateDevice(
    IDirect3D9* d3d,
    UINT adapter,
    D3DDEVTYPE type,
    HWND focusWindow,
    DWORD behavior,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** device)
{
    auto realCreateDevice = reinterpret_cast<CreateDeviceFn>(
        GetOriginal(d3d, gCreateDeviceOriginals));

    if (!realCreateDevice) {
        Log("ERROR: Original CreateDevice function not found\n");
        return D3DERR_INVALIDCALL;
    }

    const HRESULT hr = realCreateDevice(
        d3d, adapter, type, focusWindow, behavior, pp, device);

    if (SUCCEEDED(hr) && device && *device) {
        if (!RememberBackBuffer(*device) && pp) {
            RememberBackBufferSize(pp->BackBufferWidth, pp->BackBufferHeight);
        }

        InstallDeviceHooks(*device);
        Log("Device hooks installed or attempted\n");
    } else if (FAILED(hr)) {
        Log("WARNING: CreateDevice failed with HRESULT 0x%08X\n",
            static_cast<UINT>(hr));
    }

    // If hook installation fails, preserve the real CreateDevice result.
    return hr;
}

static bool InstallCreateDeviceHook(IDirect3D9* d3d)
{
    return PatchSlot(
        d3d,
        VTABLE_SLOT_CREATE_DEVICE,
        reinterpret_cast<void*>(HookCreateDevice),
        gCreateDeviceOriginals);
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

    if (!InstallCreateDeviceHook(d3d)) {
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

    const HRESULT hr = realCreate9Ex(sdkVersion, out);

    if (SUCCEEDED(hr) && *out) {
        // IDirect3D9Ex inherits IDirect3D9, including CreateDevice.
        IDirect3D9* base = static_cast<IDirect3D9*>(*out);

        if (!InstallCreateDeviceHook(base)) {
            // Leave the successfully created Ex interface usable, but log that
            // the depth-surface fix will not be active on this path.
            Log("ERROR: Failed to patch IDirect3D9Ex CreateDevice slot\n");
        } else {
            Log("IDirect3D9Ex CreateDevice slot patched\n");
        }
    } else if (FAILED(hr)) {
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
