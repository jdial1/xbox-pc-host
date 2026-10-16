#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

namespace
{
    std::string NormalizeDll(const char* dll)
    {
        std::string s = dll;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0)
            s.resize(s.size() - 4);
        // These are forwarders into kernel32's functionality.
        if (s == "kernelbase" || s.rfind("api-ms-win-core-", 0) == 0)
            return "kernel32";
        return s;
    }
}

Win32Layer::Win32Layer()
{
    RegisterKernel32(*this);
}

void Win32Layer::Register(const char* key, void* fn)
{
    m_table[key] = fn;
}

void* Win32Layer::ResolveImport(const char* dll, const char* name, uint16_t ordinal)
{
    std::string key = NormalizeDll(dll);
    key += '!';
    key += name ? std::string(name) : "#" + std::to_string(ordinal);

    auto it = m_table.find(key);
    if (it != m_table.end())
        return it->second;

    auto fb = m_fallbacks.find(key);
    if (fb == m_fallbacks.end())
    {
        fb = m_fallbacks.emplace(key, MakeFallback(key)).first;
        m_unresolved.push_back(key);
    }
    return fb->second;
}

void Win32Layer::LogUnresolved() const
{
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

