#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

Win32Layer::Win32Layer() = default;

void Win32Layer::Register(const char* key, void* fn)
{
    m_table[key] = fn;
}

void* Win32Layer::ResolveImport(const char* dll, const char* name, uint16_t ordinal)
{
    std::string key = dll;
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
