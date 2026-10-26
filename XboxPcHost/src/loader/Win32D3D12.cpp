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

    // The console's D3D12 reports barycentrics (D3D12_OPTIONS3) and then refuses every pipeline whose pixel
    // shader uses them: Mesa reads flat (nointerpolation) inputs with attributeAtVertex when the device
    // says it can, and BAR's loading screen got through only because none of its shaders had a flat input.
    // Told "no", Mesa handles flat inputs another way (Mesa's own Xbox UWP build hardcodes the same answer).
    std::atomic<bool> g_barycentricsLogged{ false };

    // HOST_TYPED_UAV_LOADS=0 (host-env.txt) leaves the console's format answers alone: Mesa then stops at
    // GL 4.1 (no shader images), for telling whether a game's rendering trouble comes from that report.
    bool TypedUavLoadsReported()
    {
        static bool on = [] {
            wchar_t buf[8]{};
            DWORD n = GetEnvironmentVariableW(L"HOST_TYPED_UAV_LOADS", buf, _countof(buf));
            bool v = !(n && n < _countof(buf) && buf[0] == L'0');
            if (!v)
                HOST_LOG(L"D3D12: typed UAV loads reported as the console says (HOST_TYPED_UAV_LOADS=0)");
            return v;
        }();
        return on;
    }

    HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(ID3D12Device* self, D3D12_FEATURE feature, void* data, UINT size)
    {
        HRESULT hr = g_checkFeatureSupport.load()(self, feature, data, size);
        if (SUCCEEDED(hr) && feature == D3D12_FEATURE_D3D12_OPTIONS3 && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        {
            auto* o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3*>(data);
            if (o->BarycentricsSupported && !g_barycentricsLogged.exchange(true))
                HOST_LOG(L"D3D12: barycentrics reported supported; telling the game no (pipelines using them are refused)");
            o->BarycentricsSupported = FALSE;
        }
        if (SUCCEEDED(hr) && feature == D3D12_FEATURE_FORMAT_SUPPORT && size >= sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT) &&
            TypedUavLoadsReported())
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

    // Resources the runtime refuses to create: logged with their description (the first 40), since Mesa
    // only reports the GL-level result (an incomplete framebuffer, a texture that stays empty).
    constexpr size_t kCreateCommittedResource = 27, kCreatePlacedResource = 29;
    using CreateCommittedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    std::atomic<CreateCommittedFn> g_createCommitted{ nullptr };
    std::atomic<CreatePlacedFn> g_createPlaced{ nullptr };
    std::atomic<int> g_resourceFailures{ 0 };

    void LogResourceFailure(const wchar_t* how, HRESULT hr, const D3D12_RESOURCE_DESC* d, UINT heapFlags)
    {
        if (!d || g_resourceFailures++ >= 40)
            return;
        HOST_LOG(L"D3D12: %s failed 0x%08x: dim %d format %d %llux%u x%u mips %u samples %u/%u layout %d flags 0x%x heap flags 0x%x",
            how, static_cast<unsigned>(hr), static_cast<int>(d->Dimension), static_cast<int>(d->Format),
            static_cast<unsigned long long>(d->Width), d->Height, d->DepthOrArraySize, d->MipLevels, d->SampleDesc.Count,
            d->SampleDesc.Quality, static_cast<int>(d->Layout), static_cast<unsigned>(d->Flags), heapFlags);
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
        REFIID iid, void** out)
    {
        HRESULT hr = g_createCommitted.load()(self, heap, flags, desc, state, clear, iid, out);
        if (hr == E_INVALIDARG && desc && desc->SampleDesc.Count > 1 && desc->Alignment != 0)
        {
            // Mesa asks 64 KB alignment for every texture; MSAA ones need 4 MB on devices without
            // MSAA64KBAlignedTextureSupported (the console). Alignment 0 lets the runtime choose.
            D3D12_RESOURCE_DESC fixed = *desc;
            fixed.Alignment = 0;
            hr = g_createCommitted.load()(self, heap, flags, &fixed, state, clear, iid, out);
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true))
                HOST_LOG(L"D3D12: %u-sample texture refused at 64 KB alignment; with the runtime's alignment: 0x%08x",
                    desc->SampleDesc.Count, static_cast<unsigned>(hr));
        }
        if (FAILED(hr))
            LogResourceFailure(L"CreateCommittedResource", hr, desc, static_cast<UINT>(flags));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out)
    {
        HRESULT hr = g_createPlaced.load()(self, heap, offset, desc, state, clear, iid, out);
        if (FAILED(hr))
            LogResourceFailure(L"CreatePlacedResource", hr, desc, 0);
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
        FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f)
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
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
        HRESULT o4hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
        HOST_LOG(L"D3D12 OPTIONS4 (0x%08x): MSAA 64 KB aligned textures %d", static_cast<unsigned>(o4hr),
            o4.MSAA64KBAlignedTextureSupported);
        HOST_LOG(L"D3D12 features: max level 0x%x, shader model 0x%x (0x%08x), binding tier %d, typed UAV loads %d, "
                 L"tiled tier %d, ROVs %d, conservative raster %d",
            static_cast<unsigned>(fl.MaxSupportedFeatureLevel), static_cast<unsigned>(sm.HighestShaderModel),
            static_cast<unsigned>(smhr), static_cast<int>(o.ResourceBindingTier), o.TypedUAVLoadAdditionalFormats,
            static_cast<int>(o.TiledResourcesTier), o.ROVsSupported, static_cast<int>(o.ConservativeRasterizationTier));
        // Render-target and depth formats GL framebuffers use, with their multisample counts (Mesa builds a
        // multisampled renderbuffer only for counts the device has quality levels for).
        struct Named { DXGI_FORMAT format; const wchar_t* name; };
        Named rts[] = { { DXGI_FORMAT_R8G8B8A8_UNORM, L"RGBA8" }, { DXGI_FORMAT_B8G8R8A8_UNORM, L"BGRA8" },
            { DXGI_FORMAT_R16G16B16A16_FLOAT, L"RGBA16F" }, { DXGI_FORMAT_R10G10B10A2_UNORM, L"RGB10A2" },
            { DXGI_FORMAT_D24_UNORM_S8_UINT, L"D24S8" }, { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, L"D32FS8" },
            { DXGI_FORMAT_D32_FLOAT, L"D32F" }, { DXGI_FORMAT_D16_UNORM, L"D16" }, { DXGI_FORMAT_R24G8_TYPELESS, L"R24G8 typeless" } };
        for (auto const& f : rts)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT s{ f.format };
            HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s));
            std::wstring line = std::wstring(L"  format ") + f.name + L": support1 0x" ;
            wchar_t hex[40];
            swprintf_s(hex, L"%08x support2 0x%08x", static_cast<unsigned>(s.Support1), static_cast<unsigned>(s.Support2));
            line += hex;
            line += FAILED(hr) ? L" (query failed)" : L"";
            line += L"; samples:";
            for (UINT n : { 2u, 4u, 8u })
            {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{ f.format, n, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0 };
                HRESULT qh = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q));
                line += L" " + std::to_wstring(n) + L"x=" + (FAILED(qh) ? std::wstring(L"fail") : std::to_wstring(q.NumQualityLevels));
            }
            HOST_LOG(L"%s", line.c_str());
        }
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

    // The console's D3D12 answers feature level 11_0 at most, and the runtime refuses pipelines whose vertex
    // shader reads a UAV (Mesa's GL storage buffers, which BAR's GL4 renderer uses) below 11_1. Asking for
    // 11_1 outright shows whether 11_0 is the device's limit or only what the sandbox reports; a refusal
    // falls back to what the game asked for.
    std::atomic<bool> g_level111Logged{ false };

    // HOST_D3D12_DRED=1 (host-env.txt): Device Removed Extended Data on, before the game's first device, so a
    // removal can be traced to the command list and operation the GPU was on and to a faulting address.
    std::atomic<bool> g_dredTried{ false }, g_dredOn{ false };
    void EnableDred()
    {
        if (g_dredTried.exchange(true))
            return;
        wchar_t buf[8]{};
        DWORD n = GetEnvironmentVariableW(L"HOST_D3D12_DRED", buf, _countof(buf));
        if (!(n && n < _countof(buf) && buf[0] == L'1'))
            return;
        winrt::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        HRESULT hr = D3D12GetDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), dred.put_void());
        if (SUCCEEDED(hr))
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            g_dredOn = true;
        }
        HOST_LOG(L"D3D12: DRED (breadcrumbs, page faults) %s (0x%08x)", SUCCEEDED(hr) ? L"on" : L"unavailable",
            static_cast<unsigned>(hr));
    }

    const wchar_t* OpName(D3D12_AUTO_BREADCRUMB_OP op)
    {
        switch (op)
        {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return L"DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"ExecuteIndirect";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return L"CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"ResolveSubresource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return L"ClearRenderTargetView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return L"ClearUnorderedAccessView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return L"ClearDepthStencilView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return L"ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return L"Present";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return L"ResolveQueryData";
        case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return L"WriteBufferImmediate";
        default: return L"(other)";
        }
    }
}

void LogD3D12RemovedDetails(ID3D12Device* device)
{
    static std::atomic<bool> logged{ false };
    if (!device || logged.exchange(true))
        return;
    HOST_LOG(L"D3D12 device removed: reason 0x%08x", static_cast<unsigned>(device->GetDeviceRemovedReason()));
    if (!g_dredOn)
        return;
    winrt::com_ptr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), dred.put_void())))
    {
        HOST_LOG(L"  DRED: no data interface");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    HRESULT hr = dred->GetAutoBreadcrumbsOutput(&crumbs);
    HOST_LOG(L"  DRED breadcrumbs: 0x%08x", static_cast<unsigned>(hr));
    int nodes = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* n = SUCCEEDED(hr) ? crumbs.pHeadAutoBreadcrumbNode : nullptr; n && nodes < 32;
         n = n->pNext, ++nodes)
    {
        UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (done >= n->BreadcrumbCount)
            continue; // this list finished
        wchar_t head[96];
        swprintf_s(head, L"    queue %p list %p: %u of %u ops done;", n->pCommandQueue, n->pCommandList, done, n->BreadcrumbCount);
        std::wstring line = head;
        UINT from = done > 3 ? done - 3 : 0;
        for (UINT i = from; i < n->BreadcrumbCount && i < done + 4; ++i)
            line += std::wstring(i == done ? L" >>" : L" ") + OpName(n->pCommandHistory[i]);
        HOST_LOG(L"%s", line.c_str());
    }
    HOST_LOG(L"  DRED: %d breadcrumb nodes", nodes);
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    hr = dred->GetPageFaultAllocationOutput(&fault);
    HOST_LOG(L"  DRED page fault: 0x%08x, VA 0x%llx", static_cast<unsigned>(hr), static_cast<unsigned long long>(fault.PageFaultVA));
    int shown = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* a = SUCCEEDED(hr) ? fault.pHeadExistingAllocationNode : nullptr; a && shown < 8;
         a = a->pNext, ++shown)
        HOST_LOG(L"    existing allocation: type %d %s", static_cast<int>(a->AllocationType), a->ObjectNameW ? a->ObjectNameW : L"");
    shown = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* a = SUCCEEDED(hr) ? fault.pHeadRecentFreedAllocationNode : nullptr; a && shown < 8;
         a = a->pNext, ++shown)
        HOST_LOG(L"    recently freed allocation: type %d %s", static_cast<int>(a->AllocationType), a->ObjectNameW ? a->ObjectNameW : L"");
}

namespace
{
    HRESULT WINAPI Shim_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out)
    {
        EnableDred();
        HRESULT hr = E_FAIL;
        if (level < D3D_FEATURE_LEVEL_11_1)
        {
            hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_1, iid, out);
            if (!g_level111Logged.exchange(true))
                HOST_LOG(L"D3D12CreateDevice(feature level 0xb100, game asked for 0x%x) -> 0x%08x", static_cast<unsigned>(level),
                    static_cast<unsigned>(hr));
        }
        if (FAILED(hr))
            hr = D3D12CreateDevice(adapter, level, iid, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            // Slot 47 exists only on an ID3D12Device2 (and later).
            winrt::com_ptr<ID3D12Device2> device2;
            static_cast<IUnknown*>(*out)->QueryInterface(__uuidof(ID3D12Device2), device2.put_void());
            void** original = device2
                ? PatchComTable(*out, 128, { { kCreateFence, reinterpret_cast<void*>(&Hook_CreateFence) },
                      { kCheckFeatureSupport, reinterpret_cast<void*>(&Hook_CheckFeatureSupport) },
                      { kCreateGraphicsPipelineState, reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState) },
                      { kCreateCommittedResource, reinterpret_cast<void*>(&Hook_CreateCommittedResource) },
                      { kCreatePlacedResource, reinterpret_cast<void*>(&Hook_CreatePlacedResource) },
                      { kCreatePipelineState, reinterpret_cast<void*>(&Hook_CreatePipelineState) } })
                : PatchComTable(*out, 128, { { kCreateFence, reinterpret_cast<void*>(&Hook_CreateFence) },
                      { kCheckFeatureSupport, reinterpret_cast<void*>(&Hook_CheckFeatureSupport) },
                      { kCreateCommittedResource, reinterpret_cast<void*>(&Hook_CreateCommittedResource) },
                      { kCreatePlacedResource, reinterpret_cast<void*>(&Hook_CreatePlacedResource) },
                      { kCreateGraphicsPipelineState, reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState) } });
            g_createCommitted = reinterpret_cast<CreateCommittedFn>(original[kCreateCommittedResource]);
            g_createPlaced = reinterpret_cast<CreatePlacedFn>(original[kCreatePlacedResource]);
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
            {
                HOST_LOG(L"  the game's first D3D12 device: removed reason 0x%08x",
                    static_cast<unsigned>(g_device->GetDeviceRemovedReason()));
                LogD3D12RemovedDetails(g_device.get());
            }
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
