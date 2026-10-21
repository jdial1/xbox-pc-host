#include "pch.h"
#include <d3d12.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// D3D12 for the game: plain pass-throughs. Games don't use it directly yet; Mesa's d3d12 OpenGL driver
// (a game DLL, for OpenGL games) does, and presents through DXGI like any D3D game.
//
// D3D12GetInterface and D3D12EnableExperimentalFeatures are looked up at run time: a static import of
// a function the console's d3d12.dll lacks would keep the host from starting at all.

void RegisterD3D12(Win32Layer& layer)
{
    layer.Register("d3d12!D3D12CreateDevice", reinterpret_cast<void*>(&D3D12CreateDevice));
    layer.Register("d3d12!D3D12GetDebugInterface", reinterpret_cast<void*>(&D3D12GetDebugInterface));
    layer.Register("d3d12!D3D12SerializeRootSignature", reinterpret_cast<void*>(&D3D12SerializeRootSignature));
    layer.Register("d3d12!D3D12SerializeVersionedRootSignature", reinterpret_cast<void*>(&D3D12SerializeVersionedRootSignature));
    layer.Register("d3d12!D3D12CreateRootSignatureDeserializer", reinterpret_cast<void*>(&D3D12CreateRootSignatureDeserializer));
    layer.Register("d3d12!D3D12CreateVersionedRootSignatureDeserializer", reinterpret_cast<void*>(&D3D12CreateVersionedRootSignatureDeserializer));

    HMODULE d3d12 = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&D3D12CreateDevice), &d3d12))
    {
        for (const char* name : { "D3D12GetInterface", "D3D12EnableExperimentalFeatures" })
            if (FARPROC fn = GetProcAddress(d3d12, name))
                layer.Register((std::string("d3d12!") + name).c_str(), reinterpret_cast<void*>(fn));
    }
}
