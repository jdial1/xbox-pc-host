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

    mutable std::mutex m_lock;
    std::unordered_map<std::string, void*> m_table;
    std::unordered_map<std::string, void*> m_fallbacks;
    std::vector<std::string> m_unresolved;
};

// One per area of the Win32 surface (Win32Kernel.cpp, ...).
void RegisterKernel32(Win32Layer& layer);
void RegisterFiles(Win32Layer& layer);
void RegisterFakes(Win32Layer& layer);
void RegisterLoader(Win32Layer& layer);
void RegisterWindow(Win32Layer& layer);
void RegisterD3D(Win32Layer& layer);
void RegisterInput(Win32Layer& layer);
void RegisterAudio(Win32Layer& layer);
void RegisterGdi(Win32Layer& layer);
void RegisterNet(Win32Layer& layer);
class GameDisplay;
void SetGameDisplayForD3D(GameDisplay* display);
void SetGameDisplayForGdi(GameDisplay* display);

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

// "KERNEL32.DLL" -> "kernel32": the form the "dll!Name" table keys use.
std::string NormalizeDllName(const char* dll);

// A stand-in HMODULE for a DLL name (one per normalized name). LoadLibrary and
// GetModuleHandle hand these out; GetProcAddress on one goes through the resolver.
HMODULE FakeModule(const char* dll);

// The screen the game's window reports sizes for. Set before the game starts; null means a default size.
class GameDisplay;
void SetGameDisplay(GameDisplay* display);

// Feeds the game's message loop WM_SIZE, WM_ACTIVATE and WM_CLOSE from the CoreWindow. UI thread only.
void AttachGameWindow(winrt::Windows::UI::Core::CoreWindow const& window);
void DetachGameWindow();

// Window state the input and window shims share.
void PostGameMessage(UINT message, WPARAM wParam, LPARAM lParam);
UINT GameScreenWidth();
UINT GameScreenHeight();
HDC GameWindowDc(); // what the game's GetDC(hwnd) returns

// The XAudio2 engine winmm's waveOut plays through; null means no audio device.
void SetGameAudio(IXAudio2* engine);

// How many threads the game started are still running, after waiting up to waitMs for each.
// Their code lives in the game's image, so the image can't be unmapped while any run.
size_t GameThreadsAlive(DWORD waitMs);
