#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/GameDisplay.h"
#include "Log.h"

#include <map>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <intrin.h>

// The game's Direct3D, with the window part rewritten. D3D11CreateDevice runs
// for real, but on an adapter whose DXGI factory is patched so the game's
// CreateSwapChain (which wants an HWND we don't have) builds a CoreWindow
// swapchain instead. Everything else the game does goes straight to the GPU.
//
// The patch swaps the factory's vtable pointer for a copy with two slots
// replaced; the copy is per object, so nothing shared in dxgi.dll is touched.
namespace
{
    // IUnknown 0-2, IDXGIObject 3-6, IDXGIFactory 7-11, so these are the slots we replace.
    constexpr size_t kMakeWindowAssociation = 8, kCreateSwapChain = 10;
    constexpr size_t kCreateSwapChainForHwnd = 15; // IDXGIFactory1 ends at 13, then IsWindowedStereoEnabled
    constexpr size_t kPresent = 8; // IDXGIDeviceSubObject ends at 7, then IDXGISwapChain starts with Present
    constexpr size_t kResizeBuffers = 13; // after GetBuffer, Set/GetFullscreenState and GetDesc
    constexpr size_t kSetFullscreenState = 10, kGetFullscreenState = 11, kResizeTarget = 14, kGetContainingOutput = 15;
    // The factory's ways to an adapter, and the adapter's to its outputs.
    constexpr size_t kEnumAdapters = 7, kEnumAdapters1 = 12, kEnumAdapterByLuid = 26, kEnumAdapterByGpuPreference = 29;
    constexpr size_t kEnumOutputs = 7, kAdapterSlots = 24;

    // Big enough for IDXGIFactory7 and IDXGISwapChain4. Older, shorter tables just copy a few
    // harmless extra pointers from the read-only data that follows.
    constexpr size_t kFactorySlots = 32, kSwapChainSlots = 48;

    GameDisplay* g_display = nullptr;
    std::mutex g_lock;
    std::map<void**, void**> g_clones; // original vtable -> patched copy
    std::atomic<bool> g_presented{ false };
    std::atomic<ULONGLONG> g_firstPresent{ 0 }, g_lastPresent{ 0 }; // frame-gap log (file scope: no TLS guard)
    std::atomic<int> g_gapsLogged{ 0 };
    std::atomic<bool> g_watchdogStarted{ false };

    // Samples every game thread's stack when no frame has come for 400 ms, once per stall, 6 stalls at most,
    // starting 2 minutes after the first frame (loading has long gaps of its own).
    DWORD WINAPI StallWatchdog(LPVOID)
    {
        int reports = 0;
        ULONGLONG reported = 0;
        while (reports < 6)
        {
            Sleep(100);
            ULONGLONG last = g_lastPresent, now = GetTickCount64();
            if (last && now - last > 400 && reported != last && now - g_firstPresent > 120000)
            {
                reported = last;
                ++reports;
                LogGameThreadStacks(L"stall (no frame for 400 ms)");
            }
        }
        return 0;
    }

    using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_originalPresent = nullptr;
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    ResizeBuffersFn g_originalResizeBuffers = nullptr;

    // How long a vtable really is: the implementing class adds its own virtuals after the public
    // interface (dxgi's factory calls them through the same table), so count every entry that
    // points into the module the table's first method is in.
    size_t TableLength(void** table, size_t atLeast)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(table[0]), &module))
            return atLeast;
        auto base = reinterpret_cast<uint8_t*>(module);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        uint8_t* end = base + nt->OptionalHeader.SizeOfImage;
        size_t n = 0;
        while (n < 512 && table + n + 1 <= reinterpret_cast<void**>(end))
        {
            auto entry = static_cast<uint8_t*>(table[n]);
            if (entry < base || entry >= end)
                break;
            ++n;
        }
        return std::max(n, atLeast);
    }

    // Points the object at a copy of its vtable with the given slots replaced.
    // Returns the original table, whether or not the object was patched already.
    void** Patch(void* object, size_t slots, std::initializer_list<std::pair<size_t, void*>> replacements)
    {
        void*** vtablePointer = static_cast<void***>(object);
        void** current = *vtablePointer;
        std::lock_guard lock(g_lock);
        for (auto const& entry : g_clones)
            if (current == entry.second)
                return entry.first; // already patched
        auto it = g_clones.find(current);
        if (it == g_clones.end())
        {
            slots = TableLength(current, slots);
            void** clone = new void*[slots];
            memcpy(clone, current, slots * sizeof(void*));
            for (auto const& replacement : replacements)
                clone[replacement.first] = replacement.second;
            it = g_clones.emplace(current, clone).first;
        }
        *vtablePointer = it->second;
        return current;
    }

    // The table a patched object had before Patch, to call the methods it replaced.
    void** OriginalTable(void* object)
    {
        void** current = *static_cast<void***>(object);
        std::lock_guard lock(g_lock);
        for (auto const& entry : g_clones)
            if (current == entry.second)
                return entry.first;
        return current;
    }
    template <typename F> F Original(void* object, size_t slot) { return reinterpret_cast<F>(OriginalTable(object)[slot]); }

    // Whether a hooked method was called by the game rather than by dxgi or d3d11 themselves, which
    // call through the same vtables and need their own objects back. The game's image is mapped by
    // hand, so no loaded module contains its code.
    bool CalledByGame(void* returnAddress)
    {
        HMODULE module = nullptr;
        return !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(returnAddress), &module);
    }

    // Every adapter has one output for the game: the screen (see Win32DxgiOutput.cpp).
    using EnumOutputsFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, UINT, IDXGIOutput**);
    HRESULT STDMETHODCALLTYPE Hook_EnumOutputs(IDXGIAdapter* self, UINT index, IDXGIOutput** out)
    {
        if (!CalledByGame(_ReturnAddress()))
            return Original<EnumOutputsFn>(self, kEnumOutputs)(self, index, out);
        if (!out) return DXGI_ERROR_INVALID_CALL;
        *out = index == 0 ? MakeGameOutput(self) : nullptr;
        return *out ? S_OK : DXGI_ERROR_NOT_FOUND;
    }

    winrt::com_ptr<IDXGIAdapter> g_lastAdapter; // for the output a swapchain reports
    void PatchAdapter(IUnknown* object)
    {
        winrt::com_ptr<IDXGIAdapter> adapter;
        if (!object || FAILED(object->QueryInterface(__uuidof(IDXGIAdapter), adapter.put_void())))
            return;
        Patch(adapter.get(), kAdapterSlots, { { kEnumOutputs, reinterpret_cast<void*>(&Hook_EnumOutputs) } });
        std::lock_guard lock(g_lock);
        g_lastAdapter = adapter;
    }

    using EnumAdaptersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, UINT, IUnknown**);
    HRESULT STDMETHODCALLTYPE Hook_EnumAdapters(IDXGIFactory* self, UINT index, IUnknown** out)
    {
        HRESULT hr = Original<EnumAdaptersFn>(self, kEnumAdapters)(self, index, out);
        if (SUCCEEDED(hr) && out) PatchAdapter(*out);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Hook_EnumAdapters1(IDXGIFactory* self, UINT index, IUnknown** out)
    {
        HRESULT hr = Original<EnumAdaptersFn>(self, kEnumAdapters1)(self, index, out);
        if (SUCCEEDED(hr) && out) PatchAdapter(*out);
        return hr;
    }
    using EnumAdapterByLuidFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, LUID, REFIID, void**);
    HRESULT STDMETHODCALLTYPE Hook_EnumAdapterByLuid(IDXGIFactory* self, LUID luid, REFIID iid, void** out)
    {
        HRESULT hr = Original<EnumAdapterByLuidFn>(self, kEnumAdapterByLuid)(self, luid, iid, out);
        if (SUCCEEDED(hr) && out) PatchAdapter(static_cast<IUnknown*>(*out));
        return hr;
    }
    using EnumAdapterByGpuPreferenceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, UINT, DXGI_GPU_PREFERENCE, REFIID, void**);
    HRESULT STDMETHODCALLTYPE Hook_EnumAdapterByGpuPreference(IDXGIFactory* self, UINT index, DXGI_GPU_PREFERENCE preference,
        REFIID iid, void** out)
    {
        HRESULT hr = Original<EnumAdapterByGpuPreferenceFn>(self, kEnumAdapterByGpuPreference)(self, index, preference, iid, out);
        if (SUCCEEDED(hr) && out) PatchAdapter(static_cast<IUnknown*>(*out));
        return hr;
    }

    // A CoreWindow swapchain always covers the whole screen: "fullscreen" is a state the game sets
    // and reads back, nothing more, and resizing the target is the system's business.
    std::atomic<bool> g_fullscreen{ false };
    HRESULT STDMETHODCALLTYPE Hook_SetFullscreenState(IDXGISwapChain*, BOOL fullscreen, IDXGIOutput*)
    {
        g_fullscreen = fullscreen != FALSE;
        return S_OK;
    }
    using GetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
    HRESULT STDMETHODCALLTYPE Hook_GetFullscreenState(IDXGISwapChain* self, BOOL* fullscreen, IDXGIOutput** output)
    {
        if (!CalledByGame(_ReturnAddress()))
            return Original<GetFullscreenStateFn>(self, kGetFullscreenState)(self, fullscreen, output);
        if (fullscreen) *fullscreen = g_fullscreen ? TRUE : FALSE;
        if (output)
        {
            std::lock_guard lock(g_lock);
            *output = g_fullscreen ? MakeGameOutput(g_lastAdapter.get()) : nullptr;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Hook_ResizeTarget(IDXGISwapChain*, const DXGI_MODE_DESC*) { return S_OK; }
    using GetContainingOutputFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, IDXGIOutput**);
    HRESULT STDMETHODCALLTYPE Hook_GetContainingOutput(IDXGISwapChain* self, IDXGIOutput** output)
    {
        if (!CalledByGame(_ReturnAddress()))
            return Original<GetContainingOutputFn>(self, kGetContainingOutput)(self, output);
        if (!output) return DXGI_ERROR_INVALID_CALL;
        std::lock_guard lock(g_lock);
        *output = MakeGameOutput(g_lastAdapter.get());
        return S_OK;
    }

    DXGI_FORMAT FlipFormat(DXGI_FORMAT format)
    {
        // Flip-model swapchains can't be sRGB; the game can still make sRGB views of the buffers.
        switch (format)
        {
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default: return format;
        }
    }

    // Test harnesses: the back buffer about to be presented, copied through a staging texture.
    std::mutex g_tapLock;
    std::function<void(const uint32_t*, UINT, UINT, UINT)> g_d3dTap;
    ULONGLONG g_lastCapture = 0;

    void CaptureBackBuffer(IDXGISwapChain* swapChain)
    {
        winrt::com_ptr<ID3D11Texture2D> buffer;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void())))
            return;
        D3D11_TEXTURE2D_DESC desc;
        buffer->GetDesc(&desc);
        bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (!bgra && !rgba)
            return;
        winrt::com_ptr<ID3D11Device> device;
        buffer->GetDevice(device.put());
        winrt::com_ptr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        staging.MipLevels = staging.ArraySize = 1;
        staging.SampleDesc = { 1, 0 };
        winrt::com_ptr<ID3D11Texture2D> copy;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, copy.put())))
            return;
        context->CopyResource(copy.get(), buffer.get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(copy.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return;
        std::vector<uint32_t> pixels(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
        {
            auto row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
            uint32_t* out = &pixels[static_cast<size_t>(y) * desc.Width];
            for (UINT x = 0; x < desc.Width; ++x)
            {
                uint32_t c = row[x];
                out[x] = (bgra ? c : (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF)) | 0xFF000000u;
            }
        }
        context->Unmap(copy.get(), 0);
        g_d3dTap(pixels.data(), desc.Width, desc.Height, desc.Width * 4);
    }

    HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* self, UINT syncInterval, UINT flags)
    {
        if (!g_presented.exchange(true))
            HOST_LOG(L"first game frame presented");
        {
            // Hitches: a frame that took more than 100 ms since the last one.
            ULONGLONG now = GetTickCount64(), prev = g_lastPresent.exchange(now);
            if (!g_firstPresent) g_firstPresent = now;
            if (!g_watchdogStarted.exchange(true))
                if (HANDLE t = CreateThread(nullptr, 0, &StallWatchdog, nullptr, 0, nullptr))
                    CloseHandle(t);
            if (prev && now - prev > 100 && g_gapsLogged++ < 200)
                HOST_LOG(L"frame gap %llu ms at %.1f s", now - prev, (now - g_firstPresent) / 1000.0);
        }
        {
            std::lock_guard lock(g_tapLock);
            ULONGLONG now = GetTickCount64();
            if (g_d3dTap && now - g_lastCapture >= 250)
            {
                g_lastCapture = now;
                CaptureBackBuffer(self);
            }
        }
        return g_originalPresent(self, syncInterval, flags);
    }

    // Width or height 0 means "the window's size", which DXGI can't read from a CoreWindow off its
    // UI thread and turns into 8x8; hand it the display size instead.
    HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        UINT w = width ? width : (g_display ? g_display->Width() : 0);
        UINT h = height ? height : (g_display ? g_display->Height() : 0);
        // The swapchain is flip model, made for a legacy one: one buffer isn't enough (0 keeps the count),
        // and mode switching means nothing on a CoreWindow.
        UINT n = count == 1 ? 2 : count;
        UINT f = flags & ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
        HRESULT hr = g_originalResizeBuffers(self, n, w, h, FlipFormat(format), f);
        HOST_LOG(L"game ResizeBuffers %u x %ux%u flags 0x%x -> %u x %ux%u: 0x%08x", count, width, height, flags, n, w, h,
            static_cast<unsigned>(hr));
        return hr;
    }

    // The game has no HWND association to make.
    HRESULT STDMETHODCALLTYPE Hook_MakeWindowAssociation(IDXGIFactory*, HWND, UINT) { return S_OK; }

    // Every way a game makes a swapchain ends here: it is made on the CoreWindow, in the flip model
    // a CoreWindow needs, and its Present and ResizeBuffers are hooked. Returns the game's interface.
    HRESULT MakeGameSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC1 d, UINT sampleCount, REFIID iid, void** out)
    {
        if (!g_display || !device || !out)
            return DXGI_ERROR_INVALID_CALL;
        d.Width = d.Width ? d.Width : g_display->Width();
        d.Height = d.Height ? d.Height : g_display->Height();
        d.Format = FlipFormat(d.Format);
        d.SampleDesc = { 1, 0 }; // flip model has no multisampled swapchains
        d.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = std::clamp<UINT>(d.BufferCount, 2, 16);
        d.Scaling = DXGI_SCALING_STRETCH;
        if (d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD)
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Stereo = FALSE;
        if (sampleCount > 1)
            HOST_LOG(L"game asked for %u-sample swapchain; using 1", sampleCount);

        IDXGISwapChain1* swapChain1 = nullptr;
        HRESULT hr = g_display->CreateSwapChain(device, d, &swapChain1);
        if (FAILED(hr))
        {
            HOST_LOG(L"game swapchain failed: 0x%08x", static_cast<unsigned>(hr));
            return hr;
        }
        HOST_LOG(L"game swapchain %ux%u created on CoreWindow", d.Width, d.Height);

        // One vtable serves IDXGISwapChain and its later versions, so patching it covers all of them.
        void** original = Patch(swapChain1, kSwapChainSlots, {
            { kPresent, reinterpret_cast<void*>(&Hook_Present) },
            { kResizeBuffers, reinterpret_cast<void*>(&Hook_ResizeBuffers) },
            { kSetFullscreenState, reinterpret_cast<void*>(&Hook_SetFullscreenState) },
            { kGetFullscreenState, reinterpret_cast<void*>(&Hook_GetFullscreenState) },
            { kResizeTarget, reinterpret_cast<void*>(&Hook_ResizeTarget) },
            { kGetContainingOutput, reinterpret_cast<void*>(&Hook_GetContainingOutput) } });
        g_originalPresent = reinterpret_cast<PresentFn>(original[kPresent]);
        g_originalResizeBuffers = reinterpret_cast<ResizeBuffersFn>(original[kResizeBuffers]);
        hr = swapChain1->QueryInterface(iid, out);
        swapChain1->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory*, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
    {
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = desc->BufferDesc.Width;
        d.Height = desc->BufferDesc.Height;
        d.Format = desc->BufferDesc.Format;
        d.BufferUsage = desc->BufferUsage;
        d.BufferCount = desc->BufferCount;
        d.SwapEffect = desc->SwapEffect;
        d.Flags = desc->Flags;
        return MakeGameSwapChain(device, d, desc->SampleDesc.Count, __uuidof(IDXGISwapChain), reinterpret_cast<void**>(out));
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2*, IUnknown* device, HWND, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1** out)
    {
        if (!desc)
            return DXGI_ERROR_INVALID_CALL;
        return MakeGameSwapChain(device, *desc, desc->SampleDesc.Count, __uuidof(IDXGISwapChain1), reinterpret_cast<void**>(out));
    }

    void PatchFactory(IDXGIFactory* factory)
    {
        // Slots past IDXGIFactory/1 are never called on such a factory (dxgi's one class implements them all).
        Patch(factory, kFactorySlots, {
            { kEnumAdapters, reinterpret_cast<void*>(&Hook_EnumAdapters) },
            { kEnumAdapters1, reinterpret_cast<void*>(&Hook_EnumAdapters1) },
            { kEnumAdapterByLuid, reinterpret_cast<void*>(&Hook_EnumAdapterByLuid) },
            { kEnumAdapterByGpuPreference, reinterpret_cast<void*>(&Hook_EnumAdapterByGpuPreference) },
            { kMakeWindowAssociation, reinterpret_cast<void*>(&Hook_MakeWindowAssociation) },
            { kCreateSwapChain, reinterpret_cast<void*>(&Hook_CreateSwapChain) },
            { kCreateSwapChainForHwnd, reinterpret_cast<void*>(&Hook_CreateSwapChainForHwnd) } });
    }

    // A factory the game makes itself is a real one, patched like the device's.
    HRESULT PatchedFactory(HRESULT hr, REFIID iid, void** out)
    {
        if (SUCCEEDED(hr) && out && *out)
        {
            winrt::com_ptr<IDXGIFactory> factory;
            if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(IDXGIFactory), factory.put_void())))
                PatchFactory(factory.get());
        }
        HOST_LOG(L"CreateDXGIFactory -> 0x%08x", static_cast<unsigned>(hr));
        return hr;
    }

    HRESULT WINAPI Shim_CreateDXGIFactory(REFIID iid, void** out) { return PatchedFactory(CreateDXGIFactory1(iid, out), iid, out); }
    HRESULT WINAPI Shim_CreateDXGIFactory2(UINT flags, REFIID iid, void** out)
    {
        return PatchedFactory(CreateDXGIFactory2(flags & ~DXGI_CREATE_FACTORY_DEBUG, iid, out), iid, out);
    }

    HRESULT WINAPI Shim_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
        const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion, ID3D11Device** device,
        D3D_FEATURE_LEVEL* chosen, ID3D11DeviceContext** context)
    {
        // Make the adapter ourselves so we know which factory the game will find as its parent.
        winrt::com_ptr<IDXGIFactory1> factory;
        winrt::com_ptr<IDXGIAdapter1> ownAdapter;
        if (!adapter && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())) &&
            SUCCEEDED(factory->EnumAdapters1(0, ownAdapter.put())))
        {
            adapter = ownAdapter.get();
            driverType = D3D_DRIVER_TYPE_UNKNOWN;
        }

        HRESULT hr = D3D11CreateDevice(adapter, driverType, software, flags, levels, levelCount, sdkVersion, device, chosen, context);
        HOST_LOG(L"D3D11CreateDevice -> 0x%08x", static_cast<unsigned>(hr));
        if (FAILED(hr) || !device || !*device)
            return hr;

        // Whatever factory the device's adapter reports is the one the game will call.
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::com_ptr<IDXGIAdapter> deviceAdapter;
        winrt::com_ptr<IDXGIFactory> parent;
        if (SUCCEEDED((*device)->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void())) &&
            SUCCEEDED(dxgiDevice->GetAdapter(deviceAdapter.put())) &&
            SUCCEEDED(deviceAdapter->GetParent(__uuidof(IDXGIFactory), parent.put_void())))
        {
            PatchAdapter(deviceAdapter.get());
            PatchFactory(parent.get());
        }
        else
            HOST_LOG(L"couldn't find the DXGI factory to patch; the game's CreateSwapChain will fail");
        return hr;
    }
}

void SetGameD3DFrameTap(std::function<void(const uint32_t*, UINT, UINT, UINT)> tap)
{
    std::lock_guard lock(g_tapLock);
    g_d3dTap = std::move(tap);
}

void SetGameDisplayForD3D(GameDisplay* display)
{
    g_display = display;
    g_presented = false;
}

namespace
{
    // The device as D3D11CreateDevice makes it, then the swapchain as the patched factory would.
    HRESULT WINAPI Shim_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
        UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* desc,
        IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* chosen, ID3D11DeviceContext** context)
    {
        ID3D11Device* made = nullptr;
        HRESULT hr = Shim_D3D11CreateDevice(adapter, driverType, software, flags, levels, levelCount, sdkVersion, &made, chosen, context);
        if (FAILED(hr) || !made)
            return hr;
        if (desc && swapChain)
        {
            DXGI_SWAP_CHAIN_DESC copy = *desc;
            hr = Hook_CreateSwapChain(nullptr, made, &copy, swapChain);
            if (FAILED(hr))
            {
                made->Release();
                return hr;
            }
        }
        if (device) *device = made;
        else made->Release();
        return hr;
    }
}

void RegisterD3D(Win32Layer& layer)
{
    layer.Register("d3d11!D3D11CreateDevice", reinterpret_cast<void*>(&Shim_D3D11CreateDevice));
    layer.Register("d3d11!D3D11CreateDeviceAndSwapChain", reinterpret_cast<void*>(&Shim_D3D11CreateDeviceAndSwapChain));
    layer.Register("dxgi!CreateDXGIFactory", reinterpret_cast<void*>(&Shim_CreateDXGIFactory));
    layer.Register("dxgi!CreateDXGIFactory1", reinterpret_cast<void*>(&Shim_CreateDXGIFactory));
    layer.Register("dxgi!CreateDXGIFactory2", reinterpret_cast<void*>(&Shim_CreateDXGIFactory2));
    // Run-time shader compilation is allowed in the app partition; every version name maps to the one we link.
    for (const char* dll : { "d3dcompiler_47", "d3dcompiler_46", "d3dcompiler_43" })
    {
        std::string d = std::string(dll) + "!";
        layer.Register((d + "D3DCompile").c_str(), reinterpret_cast<void*>(&D3DCompile));
        layer.Register((d + "D3DCompile2").c_str(), reinterpret_cast<void*>(&D3DCompile2));
        layer.Register((d + "D3DReflect").c_str(), reinterpret_cast<void*>(&D3DReflect));
        layer.Register((d + "D3DCreateBlob").c_str(), reinterpret_cast<void*>(&D3DCreateBlob));
        layer.Register((d + "D3DGetBlobPart").c_str(), reinterpret_cast<void*>(&D3DGetBlobPart));
        layer.Register((d + "D3DStripShader").c_str(), reinterpret_cast<void*>(&D3DStripShader));
    }
}
