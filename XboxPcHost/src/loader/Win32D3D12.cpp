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

    HRESULT WINAPI Shim_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out)
    {
        HRESULT hr = D3D12CreateDevice(adapter, level, iid, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            void** original = PatchComTable(*out, 128, { { kCreateFence, reinterpret_cast<void*>(&Hook_CreateFence) } });
            g_createFence = reinterpret_cast<CreateFenceFn>(original[kCreateFence]);
        }
        HOST_LOG(L"D3D12CreateDevice(feature level 0x%x) -> 0x%08x", static_cast<unsigned>(level), static_cast<unsigned>(hr));
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
