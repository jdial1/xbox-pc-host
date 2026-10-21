#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// OpenGL games bring their own opengl32.dll (Mesa's, over D3D12); the host only adjusts what D3D12
// can't do well. Functions come from wglGetProcAddress, which games look up with GetProcAddress, so the
// game is handed a wglGetProcAddress that wraps the few functions concerned.
//
// glPrimitiveRestartIndex: D3D12 can cut strips only at 0xFFFF / 0xFFFFFFFF, and for any other restart
// index Mesa rewrites every draw's indices on the CPU after waiting for the GPU (Recoil uses 0xFFFFFF
// for 32-bit indices: under 1 fps). For a buffer that holds no index as large as 0xFFFFFF, 0xFFFFFFFF
// restarts exactly where it did, so that index becomes 0xFFFFFFFF.

namespace
{
    using ProcFn = PROC(WINAPI*)(LPCSTR);
    using RestartIndexFn = void(WINAPI*)(unsigned);
    std::atomic<ProcFn> g_wglGetProcAddress{ nullptr };
    std::atomic<RestartIndexFn> g_restartIndex{ nullptr }, g_restartIndexNV{ nullptr };
    std::atomic<bool> g_restartLogged{ false };

    unsigned NativeRestartIndex(unsigned index)
    {
        if (index != 0x00FFFFFF)
            return index;
        if (!g_restartLogged.exchange(true))
            HOST_LOG(L"OpenGL: primitive restart index 0xFFFFFF used as 0xFFFFFFFF (native on D3D12)");
        return 0xFFFFFFFF;
    }
    void WINAPI Shim_glPrimitiveRestartIndex(unsigned index) { g_restartIndex.load()(NativeRestartIndex(index)); }
    void WINAPI Shim_glPrimitiveRestartIndexNV(unsigned index) { g_restartIndexNV.load()(NativeRestartIndex(index)); }

    PROC WINAPI Shim_wglGetProcAddress(LPCSTR name)
    {
        PROC fn = g_wglGetProcAddress.load()(name);
        if (!fn || !name)
            return fn;
        if (strcmp(name, "glPrimitiveRestartIndex") == 0)
        {
            g_restartIndex = reinterpret_cast<RestartIndexFn>(fn);
            return reinterpret_cast<PROC>(&Shim_glPrimitiveRestartIndex);
        }
        if (strcmp(name, "glPrimitiveRestartIndexNV") == 0)
        {
            g_restartIndexNV = reinterpret_cast<RestartIndexFn>(fn);
            return reinterpret_cast<PROC>(&Shim_glPrimitiveRestartIndexNV);
        }
        return fn;
    }
}

void* AdjustGameOpenGlExport(HMODULE module, const char* name, void* fn)
{
    if (!fn || !name || module != GameDllNamed("opengl32"))
        return fn;
    if (strcmp(name, "wglGetProcAddress") == 0)
    {
        g_wglGetProcAddress = reinterpret_cast<ProcFn>(fn);
        return reinterpret_cast<void*>(&Shim_wglGetProcAddress);
    }
    return fn;
}
