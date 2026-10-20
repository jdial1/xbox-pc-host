#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// Static TLS (__declspec(thread), thread_local) for the game's exe.
//
// Compiled code finds its TLS block at gs:[0x58][_tls_index]: the TEB's array
// of per-module blocks. The Windows loader only fills that array for modules it
// loaded itself, so the host exe carries one TLS buffer of its own: the loader
// gives every thread a copy and keeps it through array resizes and thread exit.
// The game's _tls_index is pointed at the host's, and each game thread copies
// the game's template into its buffer before running game code.
//
// The game's data goes at the start of the host's block. Apart from the buffer
// the block holds only the CRT's _tls_start marker, which nothing reads at run
// time; InstallGameTls checks the host's TLS directory for anything else.
// Threads the game didn't create (thread pool callbacks) see a zeroed block.

extern "C" unsigned long _tls_index; // the host's index, set by the Windows loader
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    constexpr size_t kGameTlsBytes = 64 * 1024;
    constexpr size_t kGameTlsAlign = 64; // covers any game's TLS alignment
    alignas(kGameTlsAlign) __declspec(thread) uint8_t t_gameTls[kGameTlsBytes];

    struct GameTls
    {
        const uint8_t* data = nullptr; // the game's initialized TLS bytes
        size_t rawSize = 0;
        size_t totalSize = 0; // raw bytes plus zero fill
        std::vector<std::pair<PIMAGE_TLS_CALLBACK, void*>> callbacks; // with the base of the image each belongs to
        std::wstring owner;   // whose template is in the buffer: the exe's, or one DLL's
    } g_tls;
    size_t g_capacity = 0; // usable bytes from the start of each thread's host block

    uint8_t* ThisThreadsBlock()
    {
        auto blocks = reinterpret_cast<uint8_t**>(__readgsqword(0x58));
        return blocks ? blocks[_tls_index] : nullptr;
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

    void RunCallbacks(DWORD reason)
    {
        for (auto const& [callback, base] : g_tls.callbacks)
            callback(base, reason, nullptr);
    }

    // The buffer has to end the host's block and sit right after the marker's padding.
    bool CheckHostBlock(std::wstring& error)
    {
        uint8_t* block = ThisThreadsBlock();
        size_t lead = block && t_gameTls >= block ? static_cast<size_t>(t_gameTls - block) : SIZE_MAX;
        if (lead > kGameTlsAlign || HostTlsSize() != lead + kGameTlsBytes)
        {
            error = L"the host's TLS block holds more than the game buffer (another thread_local in the host?): buffer at +" +
                std::to_wstring(lead) + L", block " + std::to_wstring(HostTlsSize()) + L" bytes";
            return false;
        }
        g_capacity = lead + kGameTlsBytes;
        return true;
    }
}

bool InstallGameTls(PeImage const& image, std::wstring& error)
{
    g_tls = {};
    for (auto cb : image.TlsCallbacks())
        g_tls.callbacks.emplace_back(cb, image.Base());
    auto const& tls = image.Tls();
    if (!tls.index)
        return true; // no TLS directory: callbacks only, if any

    if (!CheckHostBlock(error))
        return false;
    if (tls.totalSize > g_capacity)
    {
        error = L"the game's static TLS needs " + std::to_wstring(tls.totalSize) + L" bytes; the host has " +
            std::to_wstring(g_capacity);
        return false;
    }
    g_tls.data = tls.data;
    g_tls.rawSize = tls.rawSize;
    g_tls.totalSize = tls.totalSize;
    g_tls.owner = image.Path().filename().wstring();
    *tls.index = _tls_index;
    HOST_LOG(L"static TLS: %zu bytes (%zu initialized), index %lu, %zu callback(s)",
        g_tls.totalSize, g_tls.rawSize, _tls_index, g_tls.callbacks.size());
    return true;
}

bool InstallDllTls(PeImage const& image, std::wstring& error)
{
    auto const& tls = image.Tls();
    if (!tls.index)
        return true;
    if (g_tls.totalSize)
    {
        error = L"the TLS buffer already holds " + g_tls.owner + L"'s; only one image can have static TLS here";
        return false;
    }
    if (!CheckHostBlock(error))
        return false;
    if (tls.totalSize > g_capacity)
    {
        error = L"needs " + std::to_wstring(tls.totalSize) + L" bytes of static TLS; the host has " + std::to_wstring(g_capacity);
        return false;
    }
    g_tls.data = tls.data;
    g_tls.rawSize = tls.rawSize;
    g_tls.totalSize = tls.totalSize;
    g_tls.owner = image.Path().filename().wstring();
    *tls.index = _tls_index;
    // Threads running now get the template on this one only; they had a zeroed block, which is what
    // most of a template is. Threads the game starts later get a full copy in EnterGameThread.
    uint8_t* block = ThisThreadsBlock();
    memcpy(block, g_tls.data, g_tls.rawSize);
    memset(block + g_tls.rawSize, 0, g_tls.totalSize - g_tls.rawSize);
    for (auto cb : image.TlsCallbacks())
    {
        g_tls.callbacks.emplace_back(cb, image.Base());
        cb(image.Base(), DLL_PROCESS_ATTACH, nullptr); // before its DllMain, as Windows does
    }
    HOST_LOG(L"static TLS for %s: %zu bytes, %zu callback(s)", g_tls.owner.c_str(), g_tls.totalSize, image.TlsCallbacks().size());
    return true;
}

void EnterGameThread(DWORD reason)
{
    if (g_tls.totalSize)
    {
        uint8_t* block = ThisThreadsBlock(); // what the game's code finds at gs:[0x58][_tls_index]
        memcpy(block, g_tls.data, g_tls.rawSize);
        memset(block + g_tls.rawSize, 0, g_tls.totalSize - g_tls.rawSize);
    }
    RunCallbacks(reason);
    if (reason == DLL_THREAD_ATTACH)
        NotifyGameDlls(DLL_THREAD_ATTACH);
}

void LeaveGameThread()
{
    NotifyGameDlls(DLL_THREAD_DETACH);
    RunCallbacks(DLL_THREAD_DETACH);
}
