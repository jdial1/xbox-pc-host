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

// One per area of the Win32 surface (Win32Kernel.cpp, ...).
void RegisterKernel32(Win32Layer& layer);
void RegisterFiles(Win32Layer& layer);

// The game "process": what GetModuleHandle, GetModuleFileName, GetCommandLine
// and ExitProcess report. One game runs at a time. Call from the game thread.
void SetGameProcess(void* imageBase, std::filesystem::path const& exe);

// ExitProcess on the game's main thread raises this so Run() can unwind to its
// handler; the exit code comes from GameExitCode().
constexpr unsigned long kGameExitException = 0xE0584954;
unsigned long GameExitCode();

// File paths: the game only sees its own folder (see Win32Files.cpp).
std::filesystem::path MapGamePath(const wchar_t* name);
void SetGameRoot(std::filesystem::path const& root);
