#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// Static TLS (__declspec(thread), thread_local) for the game's exe and its DLLs.
//
// Compiled code finds its TLS block at gs:[0x58][_tls_index]: the TEB's array of per-module blocks.
// The Windows loader only fills that array for modules it loaded itself, so every game image with
// static TLS needs a module the loader did load to borrow a block from: a slot. The host exe is one
// (it carries a 64 KB TLS buffer of its own); the others are the XboxPcHostTls<N>.dll files in the
// package (tools/tls-slot), each nothing but a TLS block. The loader gives every thread a copy of
// each and keeps them through array resizes and thread exit.
//
// An image's _tls_index is pointed at its slot's index, and each game thread copies the image's
// template into that slot's block before running game code. In the host's own block the game's
// data goes at the start; apart from the buffer it holds only the CRT's _tls_start marker, which
// nothing reads at run time (CheckHostBlock makes sure of that).
//
// MinGW images all have a TLS directory whose template is a zeroed marker (their __thread variables
// are emulated through TlsAlloc); those share one slot instead of using one each.
// Threads the game didn't create (thread pool callbacks) see zeroed blocks.

extern "C" unsigned long _tls_index; // the host's index, set by the Windows loader
extern "C" IMAGE_DOS_HEADER __ImageBase;

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
extern "C" __declspec(dllimport) HMODULE WINAPI LoadPackagedLibrary(LPCWSTR name, DWORD reserved);
#endif

namespace
{
    constexpr size_t kGameTlsBytes = 64 * 1024;
    constexpr size_t kGameTlsAlign = 64; // covers any game's TLS alignment
    constexpr int kSlotDlls = 10;        // XboxPcHostTls1.dll ... XboxPcHostTls10.dll
    constexpr size_t kMarkerBytes = 8;   // a template this small and all zero is only a marker
    alignas(kGameTlsAlign) __declspec(thread) uint8_t t_gameTls[kGameTlsBytes];

    struct Slot
    {
        unsigned long index = 0;
        size_t capacity = 0;   // bytes in each thread's block
        std::wstring owner;    // whose template is in it; empty while free
        const uint8_t* data = nullptr;
        size_t rawSize = 0;
        size_t totalSize = 0;  // raw bytes plus zero fill
        int markers = 0;       // marker-only images sharing it
    };
    struct Callback
    {
        PIMAGE_TLS_CALLBACK callback;
        void* base;            // the image it belongs to
        bool exe;
    };

    std::mutex g_lock;
    std::vector<Slot> g_slots; // [0] the host's own block, then the slot DLLs that loaded
    bool g_slotsLoaded = false;
    int g_markerSlot = -1;
    std::vector<Callback> g_callbacks;

    uint8_t* BlockOf(unsigned long index)
    {
        auto blocks = reinterpret_cast<uint8_t**>(__readgsqword(0x58));
        return blocks ? blocks[index] : nullptr;
    }

    // Size of the host's TLS block, from its own TLS directory.
    size_t HostTlsSize()
    {
        auto base = reinterpret_cast<uint8_t*>(&__ImageBase);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + __ImageBase.e_lfanew);
        auto const& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (!dir.VirtualAddress) return 0;
        auto tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + dir.VirtualAddress);
        return static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData) + tls->SizeOfZeroFill;
    }

    // The buffer has to end the host's block and sit right after the marker's padding.
    size_t HostCapacity(std::wstring& error)
    {
        uint8_t* block = BlockOf(_tls_index);
        size_t lead = block && t_gameTls >= block ? static_cast<size_t>(t_gameTls - block) : SIZE_MAX;
        if (lead > kGameTlsAlign || HostTlsSize() != lead + kGameTlsBytes)
        {
            error = L"the host's TLS block holds more than the game buffer (another thread_local in the host?): buffer at +" +
                std::to_wstring(lead) + L", block " + std::to_wstring(HostTlsSize()) + L" bytes";
            return 0;
        }
        return lead + kGameTlsBytes;
    }

    HMODULE LoadSlotDll(std::wstring const& name)
    {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
        return LoadLibraryExW((std::filesystem::path(self).parent_path() / name).c_str(), nullptr, 0);
#else
        return LoadPackagedLibrary(name.c_str(), 0);
#endif
    }

    void LoadSlots()
    {
        if (g_slotsLoaded)
            return;
        g_slotsLoaded = true;
        std::wstring error;
        if (size_t capacity = HostCapacity(error))
            g_slots.push_back({ _tls_index, capacity });
        else
            HOST_LOG(L"static TLS: %s", error.c_str());
        for (int i = 1; i <= kSlotDlls; ++i)
        {
            std::wstring name = L"XboxPcHostTls" + std::to_wstring(i) + L".dll";
            HMODULE dll = LoadSlotDll(name);
            using Info = ULONG (*)(SIZE_T*);
            auto info = dll ? reinterpret_cast<Info>(GetProcAddress(dll, "TlsSlotIndex")) : nullptr;
            if (!info)
            {
                HOST_LOG(L"static TLS: %s didn't load (%lu)", name.c_str(), GetLastError());
                continue;
            }
            SIZE_T bytes = 0;
            unsigned long index = info(&bytes);
            if (!BlockOf(index))
            {
                HOST_LOG(L"static TLS: %s has no block on this thread", name.c_str());
                continue;
            }
            g_slots.push_back({ index, static_cast<size_t>(bytes) });
        }
        HOST_LOG(L"static TLS: %zu slot(s)", g_slots.size());
    }

    bool MarkerOnly(PeImage::TlsTemplate const& tls)
    {
        if (tls.totalSize > kMarkerBytes)
            return false;
        for (size_t i = 0; i < tls.rawSize; ++i)
            if (tls.data[i])
                return false;
        return true;
    }

    // Finds the image a slot: the shared marker slot for a marker, otherwise the smallest free slot
    // it fits in (so the big ones stay free for big templates). Returns its position or -1.
    int PickSlot(PeImage::TlsTemplate const& tls, bool marker)
    {
        if (marker && g_markerSlot >= 0)
            return g_markerSlot;
        int best = -1;
        for (int i = 0; i < static_cast<int>(g_slots.size()); ++i)
        {
            Slot const& s = g_slots[i];
            if (!s.owner.empty() || s.capacity < tls.totalSize)
                continue;
            if (best < 0 || s.capacity < g_slots[best].capacity)
                best = i;
        }
        if (marker)
            g_markerSlot = best;
        return best;
    }

    void FillThisThread(Slot const& s)
    {
        if (!s.totalSize)
            return;
        uint8_t* block = BlockOf(s.index);
        memcpy(block, s.data, s.rawSize);
        memset(block + s.rawSize, 0, s.totalSize - s.rawSize);
    }

    bool Install(PeImage const& image, bool exe, std::wstring& error)
    {
        std::lock_guard lock(g_lock);
        for (auto cb : image.TlsCallbacks())
            g_callbacks.push_back({ cb, image.Base(), exe });
        auto const& tls = image.Tls();
        if (!tls.index)
            return true; // no TLS directory: callbacks only, if any

        LoadSlots();
        std::wstring name = image.Path().filename().wstring();
        bool marker = MarkerOnly(tls);
        int pick = PickSlot(tls, marker);
        // A game exe with real TLS data goes in the host's own block when it fits, as before 0.1.18.0. In a
        // slot DLL's block Factorio's static initializers crashed (null read in TypeToIDLoaderMapping::set,
        // a function-local static seen as built): why that block differs for it is still open.
        if (exe && !marker && !g_slots.empty() && g_slots[0].index == _tls_index && g_slots[0].owner.empty() &&
            g_slots[0].capacity >= tls.totalSize)
            pick = 0;
        if (pick < 0)
        {
            size_t largest = 0;
            for (Slot const& s : g_slots)
                if (s.owner.empty()) largest = (std::max)(largest, s.capacity);
            error = L"needs " + std::to_wstring(tls.totalSize) + L" bytes of static TLS; the largest free slot has " +
                std::to_wstring(largest);
            return false;
        }
        Slot& s = g_slots[pick];
        *tls.index = s.index;
        if (marker)
        {
            s.owner = L"markers";
            ++s.markers;
            HOST_LOG(L"static TLS for %s: %zu-byte marker, shared slot %lu", name.c_str(), tls.totalSize, s.index);
            return true;
        }
        s.owner = name;
        s.data = tls.data;
        s.rawSize = tls.rawSize;
        s.totalSize = tls.totalSize;
        // Threads running now get the template on this one only; they had a zeroed block, which is what
        // most of a template is. Threads the game starts later get a full copy in EnterGameThread.
        FillThisThread(s);
        HOST_LOG(L"static TLS for %s: %zu bytes (%zu initialized), slot %lu of %zu bytes, %zu callback(s)", name.c_str(),
            tls.totalSize, tls.rawSize, s.index, s.capacity, image.TlsCallbacks().size());
        return true;
    }
}

void ResetGameTls()
{
    std::lock_guard lock(g_lock);
    for (Slot& s : g_slots)
        s = { s.index, s.capacity };
    g_markerSlot = -1;
    g_callbacks.clear();
}

bool InstallGameTls(PeImage const& image, std::wstring& error)
{
    return Install(image, true, error);
}

bool InstallDllTls(PeImage const& image, std::wstring& error)
{
    if (!Install(image, false, error))
        return false;
    for (auto cb : image.TlsCallbacks())
        cb(image.Base(), DLL_PROCESS_ATTACH, nullptr); // before its DllMain, as Windows does
    return true;
}

void EnterGameThread(DWORD reason)
{
    std::vector<Callback> callbacks;
    {
        std::lock_guard lock(g_lock);
        for (Slot const& s : g_slots)
            FillThisThread(s);
        callbacks = g_callbacks;
    }
    // The game's main thread is the exe's process attach; for the DLLs it is just another new thread.
    for (auto const& c : callbacks)
        c.callback(c.base, reason == DLL_PROCESS_ATTACH && !c.exe ? DLL_THREAD_ATTACH : reason, nullptr);
    if (reason == DLL_THREAD_ATTACH)
        NotifyGameDlls(DLL_THREAD_ATTACH);
}

void LeaveGameThread()
{
    NotifyGameDlls(DLL_THREAD_DETACH);
    std::vector<Callback> callbacks;
    {
        std::lock_guard lock(g_lock);
        callbacks = g_callbacks;
    }
    for (auto const& c : callbacks)
        c.callback(c.base, DLL_THREAD_DETACH, nullptr);
}
