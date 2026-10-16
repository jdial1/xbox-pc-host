#pragma once

#include "loader/PeImage.h"

// The game's view of Windows: a table of "dll!Name" -> function pointer that
// the PE loader binds imports against.
class Win32Layer final : public ImportResolver
{
public:
    Win32Layer();

    void* ResolveImport(const char* dll, const char* name, uint16_t ordinal) override;

    // key is "dll!Name" with the dll already normalized (lowercase, no .dll).
    void Register(const char* key, void* fn);

private:
    static void* Fallback();

    std::unordered_map<std::string, void*> m_table;
};
