#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/PeImage.h"
#include "loader/Win32Text.h"
#include "Log.h"

// DLLs a game ships (SDL2.dll, Unity's mono.dll, plugins) are mapped like its exe: sections, relocations,
// imports bound through the Win32 layer (or to other game DLLs), exception tables, then DllMain. A DLL
// the layer has shims for (d3d11, xinput, ...) stays a fake module, so the shims win over a copy the
// game happens to carry. DLLs with static TLS are refused: the host's TLS scheme covers the exe only.
// Mapped DLLs are never unmapped; their handle is their base address, as on Windows.
namespace
{
    struct GameDll
    {
        std::string name; // normalized
        std::filesystem::path path;
        std::unique_ptr<PeImage> image;
        bool threadCalls = true; // false after DisableThreadLibraryCalls
    };

    std::recursive_mutex g_lock; // DllMain may load more DLLs on the same thread
    std::vector<std::unique_ptr<GameDll>> g_dlls;
    std::set<std::string> g_refused; // logged once
    Win32Layer* g_layer = nullptr;
    std::filesystem::path g_searchDir;  // the game exe's folder
    bool g_deferInit = false;           // while the exe's imports are bound, DllMain waits for the game thread
    std::vector<GameDll*> g_pendingInit;
    // DLLs being mapped right now (nested when one imports another), and whether a dependency failed.
    // image is set once the DLL is mapped, so a DLL that imports it back (a cycle) can bind to it.
    struct Loading { std::string name; std::string missing; PeImage* image = nullptr; };
    std::vector<Loading> g_loading;

    using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);

    // DllMain runs game code; SEH can't share a function with C++ objects.
    bool CallDllMain(void* entry, void* base, DWORD reason, DWORD& code)
    {
        __try
        {
            reinterpret_cast<DllMainFn>(entry)(static_cast<HINSTANCE>(base), reason, nullptr);
            return true;
        }
        __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    GameDll* ByName(std::string const& name)
    {
        for (auto& d : g_dlls)
            if (d->name == name)
                return d.get();
        return nullptr;
    }

    // A DLL still binding its imports, already mapped: one further up an import cycle.
    PeImage* StillLoading(std::string const& name)
    {
        for (auto const& l : g_loading)
            if (l.name == name && l.image)
                return l.image;
        return nullptr;
    }

    // Where Windows would look for a DLL the game names without a path: next to the exe, then the
    // current directory; plus next to the DLL that imports it.
    std::filesystem::path FindGameDllFile(std::string const& file, std::filesystem::path const& besides)
    {
        std::vector<std::filesystem::path> dirs;
        if (!besides.empty()) dirs.push_back(besides);
        dirs.push_back(g_searchDir);
        dirs.push_back(MapGamePath(L"."));
        std::error_code ec;
        for (auto const& dir : dirs)
        {
            auto p = dir / file;
            if (std::filesystem::is_regular_file(p, ec))
                return p;
        }
        return {};
    }

    std::string FileOf(std::string const& normalized) { return normalized + ".dll"; }

    // DLLs the layer has a few pass-throughs for, but which a game can bring whole: then the game's
    // copy serves all of its imports (MinGW builds need the real msvcrt, startup code and all).
    bool BringsOwnCopy(std::string const& normalized)
    {
        // No function-local statics here: their thread-safe init would add a thread_local to the host's TLS.
        return normalized == "msvcrt" && !FindGameDllFile(FileOf(normalized), {}).empty();
    }
}

bool GameDllPreferred(const char* dll)
{
    return BringsOwnCopy(NormalizeDllName(dll));
}

void SetGameDllLayer(Win32Layer& layer) { g_layer = &layer; }

// A new game: forget the last one's DLLs (their images stay mapped: its threads may still run) and
// hold DllMain back until the game thread starts, as Windows runs static imports' DllMain first thing.
void BeginGameDllLoading(std::filesystem::path const& exeFolder)
{
    ResetGameTls(); // a new game: every TLS slot is free again
    std::lock_guard lock(g_lock);
    for (auto& d : g_dlls)
        (void)d->image.release();
    g_dlls.clear();
    g_refused.clear();
    g_pendingInit.clear();
    g_searchDir = exeFolder;
    g_deferInit = true;
}

void RunDeferredGameDllInits()
{
    std::vector<GameDll*> pending;
    {
        std::lock_guard lock(g_lock);
        pending.swap(g_pendingInit);
        g_deferInit = false;
    }
    for (GameDll* d : pending)
    {
        DWORD code = 0;
        if (!CallDllMain(d->image->EntryPoint(), d->image->Base(), DLL_PROCESS_ATTACH, code))
            HOST_LOG(L"game DLL %s: DllMain crashed (0x%08X)", d->path.filename().c_str(), static_cast<unsigned>(code));
    }
}

HMODULE LoadGameDll(std::filesystem::path const& file, std::string const& name)
{
    std::lock_guard lock(g_lock);
    if (GameDll* d = ByName(name))
        return reinterpret_cast<HMODULE>(d->image->Base());
    if (PeImage* p = StillLoading(name))
        return reinterpret_cast<HMODULE>(p->Base());
    if (g_refused.count(name) || !g_layer)
        return nullptr;

    auto image = std::make_unique<PeImage>();
    std::wstring error;
    // The DLL's own imports may name other game DLLs; the layer asks back through ResolveFromGameDll.
    size_t loading = g_loading.size();
    g_loading.push_back({ name, {} });
    bool loaded = image->Load(file, *g_layer, error, [&] { g_loading[loading].image = image.get(); });
    std::string missing = g_loading.back().missing;
    g_loading.pop_back();
    if (loaded && !missing.empty())
    {
        g_refused.insert(name);
        HOST_LOG(L"game DLL %s: needs %S, which couldn't be loaded; not loaded either", file.filename().c_str(), missing.c_str());
        return nullptr;
    }
    if (!loaded)
    {
        g_refused.insert(name);
        HOST_LOG(L"game DLL %s: can't map (%s)", file.c_str(), error.c_str());
        return nullptr;
    }
    if (image->Tls().index && !InstallDllTls(*image, error))
    {
        g_refused.insert(name);
        HOST_LOG(L"game DLL %s: static TLS: %s; not loaded", file.filename().c_str(), error.c_str());
        return nullptr; // the mapping stays: harmless, and nothing points into it
    }
    auto dll = std::make_unique<GameDll>();
    dll->name = name;
    dll->path = file;
    dll->image = std::move(image);
    GameDll* d = dll.get();
    g_dlls.push_back(std::move(dll)); // registered before DllMain: it may ask for its own handle
    HOST_LOG(L"game DLL %s mapped at %p", file.filename().c_str(), d->image->Base());

    if (d->image->IsDll() && d->image->EntryPoint() && g_deferInit)
        g_pendingInit.push_back(d);
    else if (d->image->IsDll() && d->image->EntryPoint())
    {
        DWORD code = 0;
        if (!CallDllMain(d->image->EntryPoint(), d->image->Base(), DLL_PROCESS_ATTACH, code))
            HOST_LOG(L"game DLL %s: DllMain crashed (0x%08X)", file.filename().c_str(), static_cast<unsigned>(code));
    }
    return reinterpret_cast<HMODULE>(d->image->Base());
}

// LoadLibrary for a name the layer has no shims for: a game DLL if the game has the file. isGameFile
// says whether there was such a file (then null means it couldn't be loaded, not "not a game DLL").
HMODULE LoadGameDllByPath(const wchar_t* path, bool& isGameFile)
{
    isGameFile = false;
    std::filesystem::path p(path ? path : L"");
    std::string name = NormalizeDllName(Narrow(p.filename().wstring()).c_str());
    if (name.empty() || (g_layer->HasDll(name) && !BringsOwnCopy(name)))
        return nullptr;
    {
        std::lock_guard lock(g_lock);
        if (GameDll* d = ByName(name))
            return reinterpret_cast<HMODULE>(d->image->Base());
    }
    std::filesystem::path file;
    if (p.has_parent_path())
    {
        std::error_code ec;
        auto mapped = MapGamePath(p.c_str());
        if (std::filesystem::is_regular_file(mapped, ec))
            file = mapped;
        else if (!p.has_extension() && std::filesystem::is_regular_file(mapped.wstring() + L".dll", ec))
            file = mapped.wstring() + L".dll";
    }
    else
        file = FindGameDllFile(p.has_extension() ? Narrow(p.filename().wstring()) : FileOf(name), {});
    isGameFile = !file.empty();
    return file.empty() ? nullptr : LoadGameDll(file, name);
}

// An import the layer has no shim for, from a DLL the game ships: bind it to that DLL's export.
void* ResolveFromGameDll(const char* dll, const char* name, uint16_t ordinal)
{
    std::string n = NormalizeDllName(dll);
    if (!g_layer || (g_layer->HasDll(n) && !BringsOwnCopy(n)))
        return nullptr;
    HMODULE module;
    {
        std::lock_guard lock(g_lock);
        GameDll* d = ByName(n);
        module = d ? reinterpret_cast<HMODULE>(d->image->Base()) : nullptr;
        if (!module)
            if (PeImage* p = StillLoading(n))
                return name ? p->FindExport(name) : p->FindExportByOrdinal(ordinal);
    }
    if (!module)
    {
        auto file = FindGameDllFile(FileOf(n), {});
        if (file.empty())
            return nullptr;
        module = LoadGameDll(file, n);
        if (!module)
        {
            // The game has this DLL but it couldn't be loaded: whatever imports it can't load either.
            std::lock_guard lock(g_lock);
            if (!g_loading.empty() && g_loading.back().missing.empty())
                g_loading.back().missing = n;
        }
    }
    return module ? GameDllExport(module, name, ordinal) : nullptr;
}

void* GameDllExport(HMODULE module, const char* name, uint16_t ordinal)
{
    std::lock_guard lock(g_lock);
    for (auto& d : g_dlls)
        if (reinterpret_cast<HMODULE>(d->image->Base()) == module)
            return name ? d->image->FindExport(name) : d->image->FindExportByOrdinal(ordinal);
    return nullptr;
}

HMODULE GameDllNamed(const char* name)
{
    std::string n = NormalizeDllName(name);
    std::lock_guard lock(g_lock);
    GameDll* d = ByName(n);
    return d ? reinterpret_cast<HMODULE>(d->image->Base()) : nullptr;
}

HMODULE GameDllContaining(const void* p)
{
    std::lock_guard lock(g_lock);
    for (auto& d : g_dlls)
        if (d->image->Contains(p))
            return reinterpret_cast<HMODULE>(d->image->Base());
    return nullptr;
}

std::filesystem::path GameDllPath(HMODULE module)
{
    std::lock_guard lock(g_lock);
    for (auto& d : g_dlls)
        if (reinterpret_cast<HMODULE>(d->image->Base()) == module)
            return d->path;
    return {};
}

BOOL WINAPI GameDllDisableThreadLibraryCalls(HMODULE module)
{
    std::lock_guard lock(g_lock);
    for (auto& d : g_dlls)
        if (reinterpret_cast<HMODULE>(d->image->Base()) == module)
            d->threadCalls = false;
    return TRUE;
}

// DLL_THREAD_ATTACH / DETACH to every mapped DLL that wants them, on a game thread.
void NotifyGameDlls(DWORD reason)
{
    std::vector<std::pair<void*, void*>> calls;
    {
        std::lock_guard lock(g_lock);
        for (auto& d : g_dlls)
            if (d->threadCalls && d->image->IsDll() && d->image->EntryPoint())
                calls.emplace_back(d->image->EntryPoint(), d->image->Base());
    }
    if (reason == DLL_THREAD_DETACH)
        std::reverse(calls.begin(), calls.end());
    for (auto const& [entry, base] : calls)
    {
        DWORD code = 0;
        CallDllMain(entry, base, reason, code);
    }
}
