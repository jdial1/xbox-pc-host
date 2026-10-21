#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

std::string NormalizeDllName(const char* dll)
{
    std::string s = dll;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0)
        s.resize(s.size() - 4);
    // These are forwarders into kernel32's functionality.
    if (s == "kernelbase" || s.rfind("api-ms-win-core-", 0) == 0 || s.rfind("api-ms-win-appmodel-", 0) == 0)
        return "kernel32";
    return s;
}

namespace
{
    // ws2_32's ordinals have been fixed since Winsock 1.1, and games import by them.
    const char* Ws2Name(uint16_t ordinal)
    {
        switch (ordinal)
        {
        case 1: return "accept"; case 2: return "bind"; case 3: return "closesocket"; case 4: return "connect";
        case 5: return "getpeername"; case 6: return "getsockname"; case 7: return "getsockopt"; case 8: return "htonl";
        case 9: return "htons"; case 10: return "ioctlsocket"; case 11: return "inet_addr"; case 12: return "inet_ntoa";
        case 13: return "listen"; case 14: return "ntohl"; case 15: return "ntohs"; case 16: return "recv";
        case 17: return "recvfrom"; case 18: return "select"; case 19: return "send"; case 20: return "sendto";
        case 21: return "setsockopt"; case 22: return "shutdown"; case 23: return "socket"; case 51: return "gethostbyaddr";
        case 52: return "gethostbyname"; case 53: return "getprotobyname"; case 54: return "getprotobynumber";
        case 55: return "getservbyname"; case 56: return "getservbyport"; case 57: return "gethostname";
        case 111: return "WSAGetLastError"; case 112: return "WSASetLastError"; case 115: return "WSAStartup";
        case 116: return "WSACleanup"; case 151: return "__WSAFDIsSet";
        default: return nullptr;
        }
    }

    // xinput1_3 and xinput1_4 export by these ordinals too; xinput.lib links 1_4 by ordinal.
    const char* XInputName(uint16_t ordinal)
    {
        switch (ordinal)
        {
        case 2: return "XInputGetState"; case 3: return "XInputSetState"; case 4: return "XInputGetCapabilities";
        case 5: return "XInputEnable"; case 7: return "XInputGetBatteryInformation"; case 8: return "XInputGetKeystroke";
        default: return nullptr; // 100 (XInputGetStateEx) has no name and is registered by ordinal
        }
    }

    std::string ImportKey(const char* dll, const char* name, uint16_t ordinal)
    {
        std::string key = NormalizeDllName(dll);
        if (!name && key == "ws2_32")
            name = Ws2Name(ordinal);
        if (!name && key.rfind("xinput", 0) == 0)
            name = XInputName(ordinal);
        key += '!';
        key += name ? std::string(name) : "#" + std::to_string(ordinal);
        return key;
    }
}

Win32Layer::Win32Layer()
{
    RegisterPassThrough(*this);
    RegisterKernel32(*this);
    RegisterFiles(*this);
    RegisterFakes(*this);
    RegisterLoader(*this);
    RegisterWindow(*this);
    RegisterD3D(*this);
    RegisterInput(*this);
    RegisterAudio(*this);
    RegisterGdi(*this);
    RegisterNet(*this);
    RegisterLocale(*this);
    RegisterCom(*this);
    RegisterGdiPlus(*this);
    RegisterDSound(*this);
    RegisterLegacy(*this); // last: replaces pass-throughs of the same names
    RegisterRegistry(*this); // HKCU kept by the host; replaces the registry pass-throughs
    RegisterCursor(*this);   // ShowCursor/SetCursor decide whether the host draws the cursor
}

void Win32Layer::Register(const char* key, void* fn)
{
    m_table[key] = fn;
    std::string k = key;
    m_dlls.insert(k.substr(0, k.find('!')));
}

bool Win32Layer::HasDll(std::string const& dll) const
{
    std::lock_guard lock(m_lock);
    return m_dlls.count(dll) != 0;
}

void* Win32Layer::ResolveImport(const char* dll, const char* name, uint16_t ordinal)
{
    std::string key = ImportKey(dll, name, ordinal);

    if (GameDllPreferred(dll))
        if (void* fn = ResolveFromGameDll(dll, name, ordinal))
            return fn;
    // The game can resolve names at run time from any of its threads.
    {
        std::lock_guard lock(m_lock);
        auto it = m_table.find(key);
        if (it != m_table.end())
            return it->second;
        // kernel32 forwards ntdll's Rtl* functions; one entry serves both names.
        if (name && key.rfind("ntdll!", 0) == 0)
        {
            it = m_table.find("kernel32!" + key.substr(6));
            if (it != m_table.end())
                return it->second;
        }
    }
    // A DLL the game ships (mapping it binds its imports through here again, so not under the lock).
    if (void* fn = ResolveFromGameDll(dll, name, ordinal))
        return fn;

    std::lock_guard lock(m_lock);
    auto fb = m_fallbacks.find(key);
    if (fb == m_fallbacks.end())
    {
        fb = m_fallbacks.emplace(key, MakeFallback(key)).first;
        m_unresolved.push_back(key);
    }
    return fb->second;
}

void* Win32Layer::Find(const char* dll, const char* name, uint16_t ordinal) const
{
    std::string key = ImportKey(dll, name, ordinal);
    std::lock_guard lock(m_lock);
    auto it = m_table.find(key);
    return it != m_table.end() ? it->second : nullptr;
}

void* Win32Layer::WatchAddress()
{
    // MSVC's unordered_map: a list, then the bucket vector (first, last, end), then the mask.
    static_assert(sizeof(m_table) == 64, "the layout this assumes");
    return reinterpret_cast<uint8_t*>(&m_table) + 0x18;
}

void Win32Layer::LogUnresolved() const
{
    std::lock_guard lock(m_lock);
    HOST_LOG(L"unresolved: %zu imports", m_unresolved.size());
    for (auto const& key : m_unresolved)
        HOST_LOG(L"  unresolved %S", key.c_str());
}

namespace
{
    // A stub can't see which import it stands for, so each unresolved import
    // gets its own stub from a fixed pool, indexed to a slot holding its name.
    constexpr size_t kPoolSize = 512;

    struct Slot
    {
        std::string name;
        std::atomic<bool> logged{ false };
    };
    Slot g_slots[kPoolSize];
    std::atomic<size_t> g_used{ 0 };

    uint64_t Report(size_t i)
    {
        if (!g_slots[i].logged.exchange(true))
            HOST_LOG(L"unimplemented call: %S", g_slots[i].name.c_str());
        return 0;
    }

    uint64_t __stdcall Overflow() { return 0; }

    template <size_t I>
    uint64_t __stdcall Stub() { return Report(I); }

    template <size_t... I>
    constexpr std::array<void*, sizeof...(I)> MakeStubs(std::index_sequence<I...>)
    {
        return { reinterpret_cast<void*>(&Stub<I>)... };
    }

    const auto g_stubs = MakeStubs(std::make_index_sequence<kPoolSize>{});
}

void* Win32Layer::MakeFallback(std::string const& key)
{
    size_t i = g_used.fetch_add(1);
    if (i >= kPoolSize)
        return reinterpret_cast<void*>(&Overflow);
    g_slots[i].name = key;
    return g_stubs[i];
}

