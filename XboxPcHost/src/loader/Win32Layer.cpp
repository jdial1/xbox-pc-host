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

Win32Layer::Win32Layer() = default;

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
    return it != m_table.end() ? it->second : Fallback();
}

namespace
{
    uint64_t __stdcall Unimplemented() { return 0; }
}

void* Win32Layer::Fallback()
{
    return reinterpret_cast<void*>(&Unimplemented);
}
