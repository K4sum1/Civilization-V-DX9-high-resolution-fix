#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>

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

static HMODULE gSystemD3D9;
static UINT gBackBufferWidth;
static UINT gBackBufferHeight;

static CreateDeviceFn gRealCreateDevice;
static ResetFn gRealReset;
static CreateDepthFn gRealCreateDepth;

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

static void RememberBackBuffer(D3DPRESENT_PARAMETERS* pp)
{
    if (pp && pp->BackBufferWidth)
        gBackBufferWidth = pp->BackBufferWidth;
    if (pp && pp->BackBufferHeight)
        gBackBufferHeight = pp->BackBufferHeight;
}

static HRESULT WINAPI HookReset(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
{
    if (!gRealReset)
        return D3DERR_INVALIDCALL;

    HRESULT hr = gRealReset(device, pp);
    if (SUCCEEDED(hr))
        RememberBackBuffer(pp);
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

    // Only alter the 2048x2048 surface seen in your trace, and only
    // when the game's backbuffer is wider than that surface.
    if (width == 2048 && height == 2048 &&
        gBackBufferWidth > width)
    {
        UINT newWidth = (std::max)(4096u, gBackBufferWidth);
        UINT newHeight = (std::max)(height, gBackBufferHeight);

        Log("Trying depth surface %ux%u -> %ux%u\n",
            width, height, newWidth, newHeight);

        HRESULT hr = gRealCreateDepth(
            device, newWidth, newHeight, format,
            multisample, quality, discard, surface, sharedHandle);

        if (SUCCEEDED(hr)) {
            Log("Larger depth surface created: %ux%u\n",
                newWidth, newHeight);
            return hr;
        }

        Log("Larger surface failed (0x%08lX); retrying original size\n",
            static_cast<UINT>(hr));
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
        RememberBackBuffer(pp);

        // IDirect3DDevice9 vtable slots: Reset=16, CreateDepthStencilSurface=29.
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
