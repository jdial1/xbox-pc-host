#include "pch.h"
#include <d3d12.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// D3D12 for the game: pass-throughs, plus what the console's sandbox refuses. Games don't use it directly
// yet; Mesa's d3d12 OpenGL driver (a game DLL, for OpenGL games) does, and presents through DXGI like any
// D3D game.
//
// D3D12GetInterface isn't offered: its device factory makes devices this file never sees. Without it Mesa
// uses D3D12CreateDevice. D3D12EnableExperimentalFeatures is looked up at run time, since a static import
// of a function the console's d3d12.dll lacks would keep the host from starting at all.

namespace
{
    constexpr size_t kCreateFence = 36; // ID3D12Device: IUnknown, ID3D12Object (4), then 29 methods to it
    using CreateFenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
    std::atomic<CreateFenceFn> g_createFence{ nullptr };
    std::atomic<bool> g_sharedLogged{ false };

    // The app sandbox can't make shared fences (Mesa asks for one for its screen); an unshared fence works
    // the same within the process.
    HRESULT STDMETHODCALLTYPE Hook_CreateFence(ID3D12Device* self, UINT64 value, D3D12_FENCE_FLAGS flags, REFIID iid, void** out)
    {
        HRESULT hr = g_createFence.load()(self, value, flags, iid, out);
        if (FAILED(hr) && (flags & D3D12_FENCE_FLAG_SHARED))
        {
            HRESULT unshared = g_createFence.load()(self, value, flags & ~D3D12_FENCE_FLAG_SHARED, iid, out);
            if (!g_sharedLogged.exchange(true))
                HOST_LOG(L"D3D12: shared fence refused (0x%08x); unshared: 0x%08x", static_cast<unsigned>(hr),
                    static_cast<unsigned>(unshared));
            hr = unshared;
        }
        return hr;
    }

    // The game's first device, kept: D3D12 has one device per adapter, and the console won't make a new
    // one once a game has let go of it (Mesa destroys its screen when SDL's probe windows go, then makes
    // another: DXGI_ERROR_DEVICE_RESET). Holding it, every D3D12CreateDevice gets that same device back.
    std::mutex g_deviceLock;
    winrt::com_ptr<ID3D12Device> g_device;

    // What this D3D12 reports for what decides an OpenGL-on-D3D12 version (Mesa's d3d12 caps): the
    // console's Mesa offered only GL 4.1 where the PC's offered 4.6.
    void LogFeatures(ID3D12Device* device)
    {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl{ _countof(levels), levels };
        device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_7 };
        HRESULT smhr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        if (FAILED(smhr))
        {
            sm.HighestShaderModel = D3D_SHADER_MODEL_6_5;
            smhr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
        HOST_LOG(L"D3D12 features: max level 0x%x, shader model 0x%x (0x%08x), binding tier %d, typed UAV loads %d, "
                 L"tiled tier %d, ROVs %d, conservative raster %d",
            static_cast<unsigned>(fl.MaxSupportedFeatureLevel), static_cast<unsigned>(sm.HighestShaderModel),
            static_cast<unsigned>(smhr), static_cast<int>(o.ResourceBindingTier), o.TypedUAVLoadAdditionalFormats,
            static_cast<int>(o.TiledResourcesTier), o.ROVsSupported, static_cast<int>(o.ConservativeRasterizationTier));
        // ARB_shader_image_load_store's formats, which Mesa checks for typed UAV load and store.
        DXGI_FORMAT formats[] = { DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R32G32_FLOAT,
            DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT,
            DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM,
            DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R8G8_UNORM,
            DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R16_UNORM,
            DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R8_SNORM };
        for (DXGI_FORMAT f : formats)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT s{ f };
            HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s));
            bool uav = (s.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
            bool load = (s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
            bool store = (s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
            if (FAILED(hr) || !uav || !load || !store)
                HOST_LOG(L"  format %d: 0x%08x typed UAV %d load %d store %d", static_cast<int>(f), static_cast<unsigned>(hr),
                    uav, load, store);
        }
    }

    HRESULT WINAPI Shim_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out)
    {
        HRESULT hr = D3D12CreateDevice(adapter, level, iid, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            void** original = PatchComTable(*out, 128, { { kCreateFence, reinterpret_cast<void*>(&Hook_CreateFence) } });
            g_createFence = reinterpret_cast<CreateFenceFn>(original[kCreateFence]);
            std::lock_guard lock(g_deviceLock);
            if (!g_device)
            {
                static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(ID3D12Device), g_device.put_void());
                if (g_device)
                    LogFeatures(g_device.get());
            }
        }
        HOST_LOG(L"D3D12CreateDevice(feature level 0x%x) -> 0x%08x", static_cast<unsigned>(level), static_cast<unsigned>(hr));
        if (FAILED(hr))
        {
            std::lock_guard lock(g_deviceLock);
            if (g_device)
                HOST_LOG(L"  the game's first D3D12 device: removed reason 0x%08x",
                    static_cast<unsigned>(g_device->GetDeviceRemovedReason()));
        }
        return hr;
    }
}

void RegisterD3D12(Win32Layer& layer)
{
    layer.Register("d3d12!D3D12CreateDevice", reinterpret_cast<void*>(&Shim_D3D12CreateDevice));
    layer.Register("d3d12!D3D12GetDebugInterface", reinterpret_cast<void*>(&D3D12GetDebugInterface));
    layer.Register("d3d12!D3D12SerializeRootSignature", reinterpret_cast<void*>(&D3D12SerializeRootSignature));
    layer.Register("d3d12!D3D12SerializeVersionedRootSignature", reinterpret_cast<void*>(&D3D12SerializeVersionedRootSignature));
    layer.Register("d3d12!D3D12CreateRootSignatureDeserializer", reinterpret_cast<void*>(&D3D12CreateRootSignatureDeserializer));
    layer.Register("d3d12!D3D12CreateVersionedRootSignatureDeserializer", reinterpret_cast<void*>(&D3D12CreateVersionedRootSignatureDeserializer));

    HMODULE d3d12 = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&D3D12CreateDevice), &d3d12))
        if (FARPROC fn = GetProcAddress(d3d12, "D3D12EnableExperimentalFeatures"))
            layer.Register("d3d12!D3D12EnableExperimentalFeatures", reinterpret_cast<void*>(fn));
}
