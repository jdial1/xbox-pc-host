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

    // The console's app D3D12 (feature level 11_0) reports typed UAV stores but no typed UAV loads for the
    // formats ARB_shader_image_load_store needs, so Mesa's GL stops at 4.1 and GL4 games' shaders fail. The
    // GPU does typed loads (RDNA2); a format with typed stores is reported with typed loads too.
    constexpr size_t kCheckFeatureSupport = 13;
    using CheckFeatureSupportFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_FEATURE, void*, UINT);
    std::atomic<CheckFeatureSupportFn> g_checkFeatureSupport{ nullptr };
    std::atomic<int> g_typedLoadsAdded{ 0 };

    HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(ID3D12Device* self, D3D12_FEATURE feature, void* data, UINT size)
    {
        HRESULT hr = g_checkFeatureSupport.load()(self, feature, data, size);
        if (SUCCEEDED(hr) && feature == D3D12_FEATURE_FORMAT_SUPPORT && size >= sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        {
            auto* s = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
            if ((s->Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                (s->Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) && !(s->Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
            {
                s->Support2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
                if (g_typedLoadsAdded++ == 0)
                    HOST_LOG(L"D3D12: reporting typed UAV loads for formats with typed UAV stores (first: format %d)",
                        static_cast<int>(s->Format));
            }
        }
        return hr;
    }

    // A pipeline the runtime refuses: logged, and its shaders (DXIL) saved next to host.log as
    // pso-fail-<n>-<stage>.dxil for `dxc -dumpbin`, to see which feature the console's D3D12 refused.
    // Mesa exits the process when one fails.
    constexpr size_t kCreateGraphicsPipelineState = 10, kCreatePipelineState = 47; // ID3D12Device, ID3D12Device2
    using CreateGraphicsPsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
    using CreatePsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, REFIID, void**);
    std::atomic<CreateGraphicsPsoFn> g_createGraphicsPso{ nullptr };
    std::atomic<CreatePsoFn> g_createPso{ nullptr };
    std::atomic<int> g_psoFailures{ 0 };

    void SaveShader(int n, const wchar_t* stage, D3D12_SHADER_BYTECODE const& code)
    {
        if (!code.pShaderBytecode || !code.BytecodeLength)
            return;
        std::filesystem::path file = host::log::Folder() / (L"pso-fail-" + std::to_wstring(n) + L"-" + stage + L".dxil");
        if (FILE* f = _wfopen(file.c_str(), L"wb"))
        {
            fwrite(code.pShaderBytecode, 1, code.BytecodeLength, f);
            fclose(f);
        }
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
        REFIID iid, void** out)
    {
        HRESULT hr = g_createGraphicsPso.load()(self, desc, iid, out);
        if (FAILED(hr) && desc)
        {
            int n = g_psoFailures++;
            HOST_LOG(L"D3D12: CreateGraphicsPipelineState failed 0x%08x (failure %d): VS %zu PS %zu GS %zu HS %zu DS %zu bytes, "
                     L"%u render targets, format0 %d, depth format %d, samples %u",
                static_cast<unsigned>(hr), n, desc->VS.BytecodeLength, desc->PS.BytecodeLength, desc->GS.BytecodeLength,
                desc->HS.BytecodeLength, desc->DS.BytecodeLength, desc->NumRenderTargets,
                static_cast<int>(desc->RTVFormats[0]), static_cast<int>(desc->DSVFormat), desc->SampleDesc.Count);
            if (n < 4)
            {
                SaveShader(n, L"vs", desc->VS);
                SaveShader(n, L"ps", desc->PS);
                SaveShader(n, L"gs", desc->GS);
                SaveShader(n, L"hs", desc->HS);
                SaveShader(n, L"ds", desc->DS);
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(ID3D12Device* self, const void* desc, REFIID iid, void** out)
    {
        HRESULT hr = g_createPso.load()(self, desc, iid, out);
        if (FAILED(hr))
            HOST_LOG(L"D3D12: CreatePipelineState (stream) failed 0x%08x", static_cast<unsigned>(hr));
        return hr;
    }

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
            // Slot 47 exists only on an ID3D12Device2 (and later).
            winrt::com_ptr<ID3D12Device2> device2;
            static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(ID3D12Device2), device2.put_void());
            void** original = device2
                ? PatchComTable(*out, 128, { { kCreateFence, reinterpret_cast<void*>(&Hook_CreateFence) },
                      { kCheckFeatureSupport, reinterpret_cast<void*>(&Hook_CheckFeatureSupport) },
                      { kCreateGraphicsPipelineState, reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState) },
                      { kCreatePipelineState, reinterpret_cast<void*>(&Hook_CreatePipelineState) } })
                : PatchComTable(*out, 128, { { kCreateFence, reinterpret_cast<void*>(&Hook_CreateFence) },
                      { kCheckFeatureSupport, reinterpret_cast<void*>(&Hook_CheckFeatureSupport) },
                      { kCreateGraphicsPipelineState, reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState) } });
            g_createFence = reinterpret_cast<CreateFenceFn>(original[kCreateFence]);
            g_checkFeatureSupport = reinterpret_cast<CheckFeatureSupportFn>(original[kCheckFeatureSupport]);
            g_createGraphicsPso = reinterpret_cast<CreateGraphicsPsoFn>(original[kCreateGraphicsPipelineState]);
            if (device2)
                g_createPso = reinterpret_cast<CreatePsoFn>(original[kCreatePipelineState]);
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
