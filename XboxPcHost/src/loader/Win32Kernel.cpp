#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// kernel32 functions the game can call as they are. Taking the address of each
// one is what proves it is in the app partition. Left out on purpose: the
// process-level calls (ExitProcess, GetModuleHandle, GetModuleFileName,
// GetCommandLine), which get game-aware replacements, and the file, path,
// process and loader calls, which need the sandbox mapping.
void RegisterKernel32(Win32Layer& layer)
{
    size_t count = 0;
#define PASS(dll, fn) (layer.Register(dll "!" #fn, reinterpret_cast<void*>(&fn)), ++count)

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
