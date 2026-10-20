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

    // Loading a DLL as data (for its resources) has nothing to give back.
    constexpr DWORD kAsData = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

    HMODULE WINAPI Shim_LoadLibraryExA(LPCSTR path, HANDLE, DWORD flags)
    {
        if (flags & kAsData)
        {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }
        return Shim_LoadLibraryA(path);
    }

    HMODULE WINAPI Shim_LoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags)
    {
        return Shim_LoadLibraryExA(Narrow(path ? path : L"").c_str(), file, flags);
    }

    BOOL WINAPI Shim_FreeLibrary(HMODULE module)
    {
        std::string dll;
        return FakeName(module, dll) ? TRUE : FreeLibrary(module);
    }

    [[noreturn]] VOID WINAPI Shim_FreeLibraryAndExitThread(HMODULE module, DWORD code)
    {
        Shim_FreeLibrary(module);
        ExitThread(code);
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
        // Run-time lookups probe for optional functions, so a missing one is null, as on Windows, not a stub.
        void* fn = g_layer->Find(dll.c_str(), byOrdinal ? nullptr : name, ordinal);
        if (!fn)
        {
            HOST_LOG(L"  -> not available");
            SetLastError(ERROR_PROC_NOT_FOUND);
        }
        return reinterpret_cast<FARPROC>(fn);
    }
}

void* GameImportTableWatchAddress() { return g_layer ? g_layer->WatchAddress() : nullptr; }

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
    layer.Register("kernel32!LoadLibraryExA", reinterpret_cast<void*>(&Shim_LoadLibraryExA));
    layer.Register("kernel32!LoadLibraryExW", reinterpret_cast<void*>(&Shim_LoadLibraryExW));
    layer.Register("kernel32!FreeLibrary", reinterpret_cast<void*>(&Shim_FreeLibrary));
    layer.Register("kernel32!FreeLibraryAndExitThread", reinterpret_cast<void*>(&Shim_FreeLibraryAndExitThread));
}
