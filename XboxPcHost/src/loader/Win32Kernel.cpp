#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"

namespace
{
    struct GameProcess
    {
        HMODULE base = nullptr;
        std::wstring exeW;
        std::string exeA;
        std::wstring commandLineW;
        std::string commandLineA;
        DWORD mainThread = 0;
        std::atomic<DWORD> exitCode{ 0 };
    } g_process;

    bool IsGameModule(HMODULE module) { return !module || module == g_process.base; }

    // The real functions' truncation rules: copy what fits, null-terminate, and
    // report ERROR_INSUFFICIENT_BUFFER when it didn't all fit.
    template <typename C, typename S>
    DWORD CopyPath(S const& path, C* buffer, DWORD size)
    {
        if (!buffer || size == 0)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        size_t n = std::min<size_t>(path.size(), size - 1);
        memcpy(buffer, path.data(), n * sizeof(C));
        buffer[n] = 0;
        SetLastError(n < path.size() ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
        return static_cast<DWORD>(n);
    }

    HMODULE WINAPI Shim_GetModuleHandleW(LPCWSTR name)
    {
        return name ? GetModuleHandleW(name) : g_process.base;
    }

    HMODULE WINAPI Shim_GetModuleHandleA(LPCSTR name)
    {
        return name ? GetModuleHandleA(name) : g_process.base;
    }

    DWORD WINAPI Shim_GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
    {
        return IsGameModule(module) ? CopyPath(g_process.exeW, buffer, size) : GetModuleFileNameW(module, buffer, size);
    }

    DWORD WINAPI Shim_GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
    {
        return IsGameModule(module) ? CopyPath(g_process.exeA, buffer, size) : GetModuleFileNameA(module, buffer, size);
    }

    LPWSTR WINAPI Shim_GetCommandLineW() { return g_process.commandLineW.data(); }
    LPSTR WINAPI Shim_GetCommandLineA() { return g_process.commandLineA.data(); }

    // Ends the game, not the host. The main thread unwinds to Run(); any other
    // game thread just stops.
    [[noreturn]] VOID WINAPI Shim_ExitProcess(UINT code)
    {
        HOST_LOG(L"ExitProcess(%u)", code);
        g_process.exitCode = code;
        if (GetCurrentThreadId() == g_process.mainThread)
            RaiseException(kGameExitException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        ExitThread(code);
    }
}

void SetGameProcess(void* imageBase, std::filesystem::path const& exe)
{
    g_process.base = static_cast<HMODULE>(imageBase);
    g_process.exeW = exe.wstring();
    g_process.exeA = Narrow(g_process.exeW);
    g_process.commandLineW = L"\"" + g_process.exeW + L"\"";
    g_process.commandLineA = "\"" + g_process.exeA + "\"";
    g_process.mainThread = GetCurrentThreadId();
    SetGameRoot(exe.parent_path());
    g_process.exitCode = 0;
}

unsigned long GameExitCode() { return g_process.exitCode; }

// kernel32 functions the game can call as they are. Taking the address of each
// one is what proves it is in the app partition. Left out on purpose: the
// file, path, process and loader calls, which need the sandbox mapping.
void RegisterKernel32(Win32Layer& layer)
{
    size_t count = 0;
#define PASS(dll, fn) (layer.Register(dll "!" #fn, reinterpret_cast<void*>(&fn)), ++count)

    layer.Register("kernel32!ExitProcess", reinterpret_cast<void*>(&Shim_ExitProcess));
    layer.Register("kernel32!GetModuleHandleA", reinterpret_cast<void*>(&Shim_GetModuleHandleA));
    layer.Register("kernel32!GetModuleHandleW", reinterpret_cast<void*>(&Shim_GetModuleHandleW));
    layer.Register("kernel32!GetModuleFileNameA", reinterpret_cast<void*>(&Shim_GetModuleFileNameA));
    layer.Register("kernel32!GetModuleFileNameW", reinterpret_cast<void*>(&Shim_GetModuleFileNameW));
    layer.Register("kernel32!GetCommandLineA", reinterpret_cast<void*>(&Shim_GetCommandLineA));
    layer.Register("kernel32!GetCommandLineW", reinterpret_cast<void*>(&Shim_GetCommandLineW));

    PASS("kernel32", CloseHandle);
    PASS("kernel32", CreateEventA);
    PASS("kernel32", CreateThread);
    PASS("kernel32", DeleteCriticalSection);
    PASS("kernel32", EnterCriticalSection);
    PASS("kernel32", FindClose);
    PASS("kernel32", FormatMessageW);
    PASS("kernel32", GetCurrentProcessId);
    PASS("kernel32", GetCurrentThread);
    PASS("kernel32", GetFileTime);
    PASS("kernel32", GetLastError);
    PASS("kernel32", GetLocalTime);
    PASS("kernel32", GetProcessHeap);
    PASS("kernel32", GetStdHandle);
    PASS("kernel32", GetSystemTime);
    PASS("kernel32", GlobalAlloc);
    PASS("kernel32", GlobalLock);
    PASS("kernel32", GlobalSize);
    PASS("kernel32", GlobalUnlock);
    PASS("kernel32", HeapAlloc);
    PASS("kernel32", HeapDestroy);
    PASS("kernel32", HeapFree);
    PASS("kernel32", HeapReAlloc);
    PASS("kernel32", InitializeCriticalSection);
    PASS("kernel32", LeaveCriticalSection);
    PASS("kernel32", LocalFree);
    PASS("kernel32", OutputDebugStringA);
    PASS("kernel32", QueryPerformanceCounter);
    PASS("kernel32", QueryPerformanceFrequency);
    PASS("kernel32", ReadFile);
    PASS("kernel32", ResumeThread);
    PASS("kernel32", SetEvent);
    PASS("kernel32", SetFilePointer);
    PASS("kernel32", SetFileTime);
    PASS("kernel32", Sleep);
    PASS("kernel32", SystemTimeToFileTime);
    PASS("kernel32", WaitForSingleObject);
    PASS("kernel32", WriteFile);

    // The host CRT stands in for msvcrt.
    PASS("msvcrt", memcpy);
    PASS("msvcrt", memmove);
    PASS("msvcrt", memset);
    PASS("msvcrt", strlen);
    PASS("msvcrt", longjmp);
#undef PASS

    HOST_LOG(L"pass-through: %zu imports", count);
}
