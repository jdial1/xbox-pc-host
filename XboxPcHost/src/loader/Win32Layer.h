#pragma once

#include "loader/PeImage.h"

// The game's view of Windows: a table of "dll!Name" -> function pointer that
// the PE loader binds imports against.
class Win32Layer final : public ImportResolver
{
public:
    Win32Layer();

    void* ResolveImport(const char* dll, const char* name, uint16_t ordinal) override;

    // Logs every import that fell back to a stub; call after PeImage::Load.
    void LogUnresolved() const;

    // key is "dll!Name" with the dll already normalized (lowercase, no .dll).
    void Register(const char* key, void* fn);

private:
    static void* MakeFallback(std::string const& key);

    std::unordered_map<std::string, void*> m_table;
    std::unordered_map<std::string, void*> m_fallbacks;
    std::vector<std::string> m_unresolved;
};
