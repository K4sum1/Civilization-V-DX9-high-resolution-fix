#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>

using Create9Fn = IDirect3D9* (WINAPI*)(UINT);
using CreateDeviceFn = HRESULT (WINAPI*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using ResetFn = HRESULT (WINAPI*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using CreateDepthFn = HRESULT (WINAPI*)(
    IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DMULTISAMPLE_TYPE, DWORD, BOOL,
    IDirect3DSurface9**, HANDLE*);

static HMODULE gSystemD3D9 = nullptr;

static std::atomic<UINT> gBackBufferWidth{ 0 };
static std::atomic<UINT> gBackBufferHeight{ 0 };

static CreateDeviceFn gRealCreateDevice = nullptr;
static ResetFn gRealReset = nullptr;
static CreateDepthFn gRealCreateDepth = nullptr;

static void Log(const char* text, UINT a = 0, UINT b = 0,
                UINT c = 0, UINT d = 0)
{
    FILE* f = nullptr;
    fopen_s(&f, "Civ5DepthFix.log", "a");

    if (f) {
        fprintf(f, text, a, b, c, d);
        fclose(f);
    }
}

static bool PatchSlot(void* object, int index, void* replacement,
                      void** original)
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

static void RememberBackBuffer(IDirect3DDevice9* device)
{
    gBackBufferWidth.store(0, std::memory_order_relaxed);
    gBackBufferHeight.store(0, std::memory_order_relaxed);

    if (!device)
        return;

    IDirect3DSurface9* surface = nullptr;

    if (SUCCEEDED(device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) &&
        surface)
    {
        D3DSURFACE_DESC desc{};

        if (SUCCEEDED(surface->GetDesc(&desc))) {
            gBackBufferWidth.store(desc.Width, std::memory_order_relaxed);
            gBackBufferHeight.store(desc.Height, std::memory_order_relaxed);
        }

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

    // Adjust only the 2048x2048 surface identified in the trace.
    if (width == 2048 && height == 2048) {
        const UINT bbWidth =
            gBackBufferWidth.load(std::memory_order_relaxed);
        const UINT bbHeight =
            gBackBufferHeight.load(std::memory_order_relaxed);

        const UINT requiredWidth = (std::max)(width, bbWidth);
        const UINT requiredHeight = (std::max)(height, bbHeight);

        const UINT roundedWidth = RoundUp2048(requiredWidth);
        const UINT roundedHeight = RoundUp2048(requiredHeight);

        if (roundedWidth > width || roundedHeight > height) {
            Log("Trying depth surface %ux%u -> %ux%u\n",
                width, height, roundedWidth, roundedHeight);

            HRESULT hr = gRealCreateDepth(
                device, roundedWidth, roundedHeight, format,
                multisample, quality, discard, surface, sharedHandle);

            if (SUCCEEDED(hr)) {
                Log("Larger depth surface created: %ux%u\n",
                    roundedWidth, roundedHeight);
                return hr;
            }

            // If the rounded-up allocation fails, try the exact dimensions
            // needed for the backbuffer before falling back to the original.
            if (requiredWidth != roundedWidth ||
                requiredHeight != roundedHeight) {
                Log("Rounded allocation failed (0x%08X); "
                    "trying exact %ux%u\n",
                    static_cast<UINT>(hr),
                    requiredWidth, requiredHeight);

                hr = gRealCreateDepth(
                    device, requiredWidth, requiredHeight, format,
                    multisample, quality, discard, surface, sharedHandle);

                if (SUCCEEDED(hr)) {
                    Log("Exact depth surface created: %ux%u\n",
                        requiredWidth, requiredHeight);
                    return hr;
                }

                Log("Exact allocation failed (0x%08X)\n",
                    static_cast<UINT>(hr));
            } else {
                Log("Rounded allocation failed (0x%08X)\n",
                    static_cast<UINT>(hr));
            }

            Log("Retrying original depth surface size\n");
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
    if (!gRealCreateDevice)
        return D3DERR_INVALIDCALL;

    HRESULT hr = gRealCreateDevice(
        d3d, adapter, type, focusWindow, behavior, pp, device);

    if (SUCCEEDED(hr) && device && *device) {
        RememberBackBuffer(*device);

        // IDirect3DDevice9 vtable slots: Reset=16,
        // CreateDepthStencilSurface=29.
        PatchSlot(*device, 16, reinterpret_cast<void*>(HookReset),
                  reinterpret_cast<void**>(&gRealReset));
        PatchSlot(*device, 29, reinterpret_cast<void*>(HookCreateDepth),
                  reinterpret_cast<void**>(&gRealCreateDepth));
    }

    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion)
{
    if (!gSystemD3D9) {
        wchar_t path[MAX_PATH] = {};
        UINT n = GetSystemDirectoryW(path, MAX_PATH);

        if (!n || n >= MAX_PATH - 12)
            return nullptr;

        wcscat_s(path, L"\\d3d9.dll");

        gSystemD3D9 = LoadLibraryW(path);
        if (!gSystemD3D9)
            return nullptr;
    }

    auto realCreate9 = reinterpret_cast<Create9Fn>(
        GetProcAddress(gSystemD3D9, "Direct3DCreate9"));

    if (!realCreate9)
        return nullptr;

    IDirect3D9* d3d = realCreate9(sdkVersion);

    if (d3d) {
        // IDirect3D9::CreateDevice is vtable slot 16.
        PatchSlot(d3d, 16, reinterpret_cast<void*>(HookCreateDevice),
                  reinterpret_cast<void**>(&gRealCreateDevice));
    }

    return d3d;
}
