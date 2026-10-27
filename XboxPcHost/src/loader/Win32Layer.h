#pragma once

#include "loader/PeImage.h"

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
// In WindowsApp.lib, though the app partition's headers don't declare them.
extern "C" __declspec(dllimport) PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
extern "C" __declspec(dllimport) ULONG WINAPI RemoveVectoredExceptionHandler(PVOID handle);
#endif

// The game's view of Windows: a table of "dll!Name" -> function pointer that
// the PE loader binds imports against.
class Win32Layer final : public ImportResolver
{
public:
    Win32Layer();

    void* ResolveImport(const char* dll, const char* name, uint16_t ordinal) override;

    // Logs every import that fell back to a stub; call after PeImage::Load.
    void LogUnresolved() const;
    // A registered function, or null; unlike ResolveImport this never makes a stub (for GetProcAddress).
    void* Find(const char* dll, const char* name, uint16_t ordinal) const;

    // key is "dll!Name" with the dll already normalized (lowercase, no .dll).
    void Register(const char* key, void* fn);
    // Whether any shim or pass-through is registered under this (normalized) DLL name.
    bool HasDll(std::string const& dll) const;

    // Test harnesses: a word of the table's own memory nothing writes after startup.
    void* WatchAddress();

private:
    static void* MakeFallback(std::string const& key);

    mutable std::mutex m_lock;
    std::unordered_map<std::string, void*> m_table;
    std::unordered_map<std::string, void*> m_fallbacks;
    std::vector<std::string> m_unresolved;
    std::set<std::string> m_dlls; // every DLL name a key uses
};

// DLLs the game ships, mapped like its exe (Win32GameDlls.cpp). A handle is the DLL's base address.
void SetGameDllLayer(Win32Layer& layer);
void BeginGameDllLoading(std::filesystem::path const& exeFolder); // before the exe's imports are bound
void RunDeferredGameDllInits();                                    // on the game thread, before its entry
HMODULE LoadGameDllByPath(const wchar_t* path, bool& isGameFile);
void* ResolveFromGameDll(const char* dll, const char* name, uint16_t ordinal);
// The game's own opengl32 (Mesa): a few functions it hands out are wrapped (Win32OpenGl.cpp).
void* AdjustGameOpenGlExport(HMODULE module, const char* name, void* fn);
bool GameDllPreferred(const char* dll); // the game ships this DLL whole: its copy wins over the layer's shims
void* GameDllExport(HMODULE module, const char* name, uint16_t ordinal);
HMODULE GameDllNamed(const char* name);
HMODULE GameDllContaining(const void* p);
std::filesystem::path GameDllPath(HMODULE module);
BOOL WINAPI GameDllDisableThreadLibraryCalls(HMODULE module);
void NotifyGameDlls(DWORD reason); // DLL_THREAD_ATTACH / DLL_THREAD_DETACH

// One per area of the Win32 surface (Win32Kernel.cpp, ...). RegisterPassThrough runs first, so a
// shim registered by any of the others replaces a pass-through of the same name.
void RegisterPassThrough(Win32Layer& layer);
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
void RegisterLocale(Win32Layer& layer);
void RegisterCom(Win32Layer& layer);
void RegisterGdiPlus(Win32Layer& layer);
// WASAPI's MMDeviceEnumerator, which CoCreateInstance hands out (Win32MMDevice.cpp).
bool IsMMDeviceEnumerator(REFCLSID clsid);
HRESULT CreateMMDeviceEnumerator(REFIID iid, void** out);
class GameDisplay;
void SetGameDisplayForD3D(GameDisplay* display);
void SetGameDisplayForGdi(GameDisplay* display);

// The game "process": what GetModuleHandle, GetModuleFileName, GetCommandLine
// and ExitProcess report. One game runs at a time. Call from the game thread.
// root is the game's folder (what the game may see); the exe can be anywhere below it.
void SetGameProcess(void* imageBase, std::filesystem::path const& exe, std::filesystem::path const& root);
std::filesystem::path GameExePath(); // the running game's exe
void KeepHostFaultHandlerFirst();    // after a game adds a vectored handler
void ApplyLazyRanges(uint8_t* base, std::filesystem::path const& dir); // host-lazy.txt: ranges committed on use
int64_t LazyCommittedBytes();        // how much of them is committed so far
void RegisterD3D12(Win32Layer& layer);
void RegisterMemStats(Win32Layer& layer);
void NoteGameVirtualAlloc(const void* caller, void* p, SIZE_T size, DWORD type); // Win32MemStats.cpp
uint64_t GameGpuMemoryUsage(); // the game's adapter's local video memory in use, 0 if unknown
uint64_t GameGpuMemoryBudget(); // and the budget the system gives it (the real one, not the game's capped view)
struct ID3D12Device;
void LogD3D12RemovedDetails(ID3D12Device* device); // the reason and, with HOST_D3D12_DRED=1, DRED data (once)
// Points a COM object at a copy of its vtable with slots replaced; returns the original table.
void** PatchComTable(void* object, size_t slots, std::initializer_list<std::pair<size_t, void*>> replacements);
void RegisterLegacy(Win32Layer& layer);

// ExitProcess on the game's main thread raises this so Run() can unwind to its
// handler; the exit code comes from GameExitCode().
constexpr unsigned long kGameExitException = 0xE0584954;
unsigned long GameExitCode();

// File paths: the game only sees its own folder (see Win32Files.cpp).
std::filesystem::path MapGamePath(const wchar_t* name);
void SetGameRoot(std::filesystem::path const& root, std::filesystem::path const& currentDirectory);

// Test harnesses: the address of a word inside the running game's import table that nothing writes
// after startup (its bucket array pointer), for a hardware watchpoint; null before a game is loaded.
void* GameImportTableWatchAddress();

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
// A US keyboard's scan codes (Win32Input.cpp), and a WM_KEYDOWN/WM_KEYUP lParam carrying one.
UINT VirtualKeyToScanCode(UINT vk, bool* extended = nullptr);
UINT ScanCodeToVirtualKey(UINT scan, bool extended, bool leftRight);
LPARAM KeyMessageLParam(UINT vk, bool down);
void BroadcastGameMessage(UINT message, WPARAM wParam, LPARAM lParam); // to every window the game made
UINT GameScreenWidth();
UINT GameScreenHeight();
HDC GameWindowDc(); // what the game's GetDC(hwnd) returns
bool IsWindowDc(HDC dc); // the game window's DC or any window's from GetDC
HMONITOR GameMonitor(); // the one monitor user32 and DXGI report

// The DXGI output for that monitor, parented to the adapter (Win32DxgiOutput.cpp). Returns a new reference.
struct IDXGIOutput;
struct IDXGIAdapter;
IDXGIOutput* MakeGameOutput(IDXGIAdapter* adapter);

// Test harnesses only: a copy of the game's D3D back buffer (BGRA) a few times a second, as it presents.
void SetGameD3DFrameTap(std::function<void(const uint32_t* bgra, UINT width, UINT height, UINT pitchBytes)> tap);

// The XAudio2 engine winmm's waveOut plays through; null means no audio device.
void SetGameAudio(IXAudio2* engine);
IXAudio2* GameAudioEngine(); // null when there is no audio device
void RegisterDSound(Win32Layer& layer);
void RegisterRegistry(Win32Layer& layer);
void RegisterCursor(Win32Layer& layer);
struct IDXGISwapChain;
void DrawGameCursor(IDXGISwapChain* swapChain); // the system cursor, drawn into the back buffer before Present
POINT GameCursorPosition();                       // in game window pixels

// Static TLS for the game's exe (Win32Tls.cpp). Install once after loading, on any thread;
// every thread that runs game code calls EnterGameThread first (DLL_PROCESS_ATTACH on the
// game's main thread, DLL_THREAD_ATTACH on the others) and LeaveGameThread when it's done.
bool InstallGameTls(PeImage const& image, std::wstring& error);
bool InstallDllTls(PeImage const& image, std::wstring& error); // a game DLL's
void ResetGameTls(); // frees every slot, before a game's images load
void EnterGameThread(DWORD reason);
void LeaveGameThread();

// Logs the calling thread's stack: "game+0x..." for frames in the game's image (resolve them with
// tools/resolve_game_offsets.py and the game's pdb), module+offset for everything else.
void LogGameStack(const wchar_t* why, const CONTEXT* from = nullptr); // from: a fault's context, else the caller's

// Logs where every game thread is right now (each suspended briefly): for stalls.
void LogGameThreadStacks(const wchar_t* why);
void NoteHostUiThread(); // call on the CoreWindow's thread

// How many threads the game started are still running, after waiting up to waitMs for each.
// Their code lives in the game's image, so the image can't be unmapped while any run.
size_t GameThreadsAlive(DWORD waitMs);
