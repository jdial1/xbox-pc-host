#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"

// LoadLibrary and GetProcAddress for the game. No DLL is ever loaded: each
// name gets a fake handle, and GetProcAddress asks the resolver, so the game
// receives the shims where there are any and logging stubs where there are none.
namespace
{
    std::mutex g_lock;
    std::vector<std::string> g_modules; // normalized names; index i is handle kFakeBase + (i + 1) * kFakeStep
    Win32Layer* g_layer = nullptr;

    // Far below any address the host's own modules or heap use, so a fake
    // handle can't be mistaken for a real one.
    constexpr uintptr_t kFakeBase = 0x10000000;
    constexpr uintptr_t kFakeStep = 0x10000;

    // The game may pass a full path; only the file name picks the module.
    std::string ModuleName(const char* path)
    {
        std::string s = path ? path : "";
        size_t slash = s.find_last_of("\\/");
        if (slash != std::string::npos)
            s.erase(0, slash + 1);
        return NormalizeDllName(s.c_str());
    }

    bool FakeName(HMODULE module, std::string& name)
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(module);
        if (v <= kFakeBase || (v - kFakeBase) % kFakeStep != 0)
            return false;
        std::lock_guard lock(g_lock);
        size_t i = (v - kFakeBase) / kFakeStep - 1;
        if (i >= g_modules.size())
            return false;
        name = g_modules[i];
        return true;
    }

    HMODULE WINAPI Shim_LoadLibraryA(LPCSTR path)
    {
        HMODULE handle = FakeModule(path ? ModuleName(path).c_str() : "");
        HOST_LOG(L"LoadLibrary %S -> fake 0x%llx", path ? path : "(null)", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
        return handle;
    }

    HMODULE WINAPI Shim_LoadLibraryW(LPCWSTR path)
    {
        return Shim_LoadLibraryA(Narrow(path ? path : L"").c_str());
    }

    FARPROC WINAPI Shim_GetProcAddress(HMODULE module, LPCSTR name)
    {
        std::string dll;
        if (!FakeName(module, dll))
            return GetProcAddress(module, name);

        // An ordinal arrives as a pointer whose value fits in 16 bits.
        bool byOrdinal = reinterpret_cast<uintptr_t>(name) < 0x10000;
        uint16_t ordinal = byOrdinal ? static_cast<uint16_t>(reinterpret_cast<uintptr_t>(name)) : 0;
        if (byOrdinal)
            HOST_LOG(L"GetProcAddress %S!#%u", dll.c_str(), ordinal);
        else
            HOST_LOG(L"GetProcAddress %S!%S", dll.c_str(), name);
        return reinterpret_cast<FARPROC>(g_layer->ResolveImport(dll.c_str(), byOrdinal ? nullptr : name, ordinal));
    }
}

HMODULE FakeModule(const char* dll)
{
    std::string name = NormalizeDllName(dll);
    std::lock_guard lock(g_lock);
    auto it = std::find(g_modules.begin(), g_modules.end(), name);
    size_t i = static_cast<size_t>(it - g_modules.begin());
    if (it == g_modules.end())
        g_modules.push_back(name);
    return reinterpret_cast<HMODULE>(kFakeBase + (i + 1) * kFakeStep);
}

void RegisterLoader(Win32Layer& layer)
{
    g_layer = &layer;
    layer.Register("kernel32!LoadLibraryA", reinterpret_cast<void*>(&Shim_LoadLibraryA));
    layer.Register("kernel32!LoadLibraryW", reinterpret_cast<void*>(&Shim_LoadLibraryW));
    layer.Register("kernel32!GetProcAddress", reinterpret_cast<void*>(&Shim_GetProcAddress));
}
