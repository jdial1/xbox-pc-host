#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/GameDisplay.h"
#include "Log.h"

#include <map>

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
    constexpr size_t kPresent = 8; // IDXGIDeviceSubObject ends at 7, then IDXGISwapChain starts with Present
    constexpr size_t kResizeBuffers = 13; // after GetBuffer, Set/GetFullscreenState and GetDesc

    // Big enough for IDXGIFactory7 and IDXGISwapChain4. Older, shorter tables just copy a few
    // harmless extra pointers from the read-only data that follows.
    constexpr size_t kFactorySlots = 32, kSwapChainSlots = 48;

    GameDisplay* g_display = nullptr;
    std::mutex g_lock;
    std::map<void**, void**> g_clones; // original vtable -> patched copy
    std::atomic<bool> g_presented{ false };

    using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_originalPresent = nullptr;
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    ResizeBuffersFn g_originalResizeBuffers = nullptr;

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
            void** clone = new void*[slots];
            memcpy(clone, current, slots * sizeof(void*));
            for (auto const& replacement : replacements)
                clone[replacement.first] = replacement.second;
            it = g_clones.emplace(current, clone).first;
        }
        *vtablePointer = it->second;
        return current;
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

    HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* self, UINT syncInterval, UINT flags)
    {
        if (!g_presented.exchange(true))
            HOST_LOG(L"first game frame presented");
        return g_originalPresent(self, syncInterval, flags);
    }

    // Width or height 0 means "the window's size", which DXGI can't read from a CoreWindow off its
    // UI thread and turns into 8x8; hand it the display size instead.
    HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        UINT w = width ? width : (g_display ? g_display->Width() : 0);
        UINT h = height ? height : (g_display ? g_display->Height() : 0);
        HRESULT hr = g_originalResizeBuffers(self, count, w, h, FlipFormat(format), flags);
        HOST_LOG(L"game ResizeBuffers %ux%u -> %ux%u: 0x%08x", width, height, w, h, static_cast<unsigned>(hr));
        return hr;
    }

    // The game has no HWND association to make.
    HRESULT STDMETHODCALLTYPE Hook_MakeWindowAssociation(IDXGIFactory*, HWND, UINT) { return S_OK; }

    HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory*, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
    {
        if (!g_display || !device || !desc || !out)
            return DXGI_ERROR_INVALID_CALL;

        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = desc->BufferDesc.Width ? desc->BufferDesc.Width : g_display->Width();
        d.Height = desc->BufferDesc.Height ? desc->BufferDesc.Height : g_display->Height();
        d.Format = FlipFormat(desc->BufferDesc.Format);
        d.SampleDesc.Count = 1; // flip model has no multisampled swapchains
        d.BufferUsage = desc->BufferUsage | DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = std::max<UINT>(2, desc->BufferCount);
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (desc->SampleDesc.Count > 1)
            HOST_LOG(L"game asked for %u-sample swapchain; using 1", desc->SampleDesc.Count);

        IDXGISwapChain1* swapChain1 = nullptr;
        HRESULT hr = g_display->CreateSwapChain(device, d, &swapChain1);
        if (FAILED(hr))
        {
            HOST_LOG(L"game swapchain failed: 0x%08x", static_cast<unsigned>(hr));
            return hr;
        }
        HOST_LOG(L"game swapchain %ux%u created on CoreWindow", d.Width, d.Height);

        IDXGISwapChain* swapChain = nullptr;
        hr = swapChain1->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&swapChain));
        swapChain1->Release();
        if (FAILED(hr))
            return hr;

        void** original = Patch(swapChain, kSwapChainSlots, {
            { kPresent, reinterpret_cast<void*>(&Hook_Present) },
            { kResizeBuffers, reinterpret_cast<void*>(&Hook_ResizeBuffers) } });
        g_originalPresent = reinterpret_cast<PresentFn>(original[kPresent]);
        g_originalResizeBuffers = reinterpret_cast<ResizeBuffersFn>(original[kResizeBuffers]);
        *out = swapChain;
        return S_OK;
    }

    void PatchFactory(IDXGIFactory* factory)
    {
        Patch(factory, kFactorySlots, {
            { kMakeWindowAssociation, reinterpret_cast<void*>(&Hook_MakeWindowAssociation) },
            { kCreateSwapChain, reinterpret_cast<void*>(&Hook_CreateSwapChain) } });
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
            PatchFactory(parent.get());
        else
            HOST_LOG(L"couldn't find the DXGI factory to patch; the game's CreateSwapChain will fail");
        return hr;
    }
}

void SetGameDisplayForD3D(GameDisplay* display)
{
    g_display = display;
    g_presented = false;
}

void RegisterD3D(Win32Layer& layer)
{
    layer.Register("d3d11!D3D11CreateDevice", reinterpret_cast<void*>(&Shim_D3D11CreateDevice));
}
