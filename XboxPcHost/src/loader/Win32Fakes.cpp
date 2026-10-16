#include "pch.h"
#include "loader/Win32Layer.h"

// Calls with no real answer on the console. They report what a normal Windows
// 10 desktop would, or fail the way a locked-down one would. Global* and
// LocalFree stay on the real functions: they are heap-backed already.
namespace
{
    BOOL WINAPI Shim_GetVersionExA(LPOSVERSIONINFOA info)
    {
        if (!info || info->dwOSVersionInfoSize < sizeof(OSVERSIONINFOA))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        info->dwMajorVersion = 10;
        info->dwMinorVersion = 0;
        info->dwBuildNumber = 19045;
        info->dwPlatformId = VER_PLATFORM_WIN32_NT;
        info->szCSDVersion[0] = '\0';
        if (info->dwOSVersionInfoSize >= sizeof(OSVERSIONINFOEXA))
        {
            auto ex = reinterpret_cast<OSVERSIONINFOEXA*>(info);
            ex->wServicePackMajor = 0;
            ex->wServicePackMinor = 0;
            ex->wSuiteMask = 0;
            ex->wProductType = VER_NT_WORKSTATION;
        }
        return TRUE;
    }

    template <size_t N>
    UINT CopyString(const char (&text)[N], char* buffer, UINT size)
    {
        // Like the real calls: when it doesn't fit, return the size needed including the terminator.
        if (!buffer || size < N)
            return static_cast<UINT>(N);
        memcpy(buffer, text, N);
        return static_cast<UINT>(N - 1);
    }

    UINT WINAPI Shim_GetSystemDirectoryA(LPSTR buffer, UINT size)
    {
        return CopyString("C:\\Windows\\System32", buffer, size);
    }

    UINT WINAPI Shim_GetWindowsDirectoryW(LPWSTR buffer, UINT size)
    {
        static const wchar_t text[] = L"C:\\Windows";
        constexpr UINT needed = static_cast<UINT>(_countof(text));
        if (!buffer || size < needed)
            return needed;
        memcpy(buffer, text, sizeof(text));
        return needed - 1;
    }

    // Stored and forgotten: crashes are caught by the host's own SEH handler.
    LPVOID WINAPI Shim_SetUnhandledExceptionFilter(LPVOID)
    {
        return nullptr;
    }

    BOOL Denied()
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    // The game never gets to start another process or read memory outside itself.
    BOOL WINAPI Shim_CreateProcess(void*, void*, void*, void*, BOOL, DWORD, void*, void*, void*, void*) { return Denied(); }
    BOOL WINAPI Shim_ReadProcessMemory(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T*) { return Denied(); }

    HINSTANCE WINAPI Shim_ShellExecute(HWND, LPCVOID, LPCVOID, LPCVOID, LPCVOID, INT)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(5)); // SE_ERR_ACCESSDENIED
    }

    // The common dialogs aren't in the app partition. FALSE with no extended
    // error is what the game sees when the user cancels.
    BOOL WINAPI Shim_FileDialog(void*) { return FALSE; }
    DWORD WINAPI Shim_CommDlgExtendedError() { return 0; }
}

void RegisterFakes(Win32Layer& layer)
{
#define FAKE(dll, fn, shim) layer.Register(dll "!" #fn, reinterpret_cast<void*>(&shim))
    FAKE("kernel32", GetVersionExA, Shim_GetVersionExA);
    FAKE("kernel32", GetSystemDirectoryA, Shim_GetSystemDirectoryA);
    FAKE("kernel32", GetWindowsDirectoryW, Shim_GetWindowsDirectoryW);
    FAKE("kernel32", SetUnhandledExceptionFilter, Shim_SetUnhandledExceptionFilter);
    FAKE("kernel32", CreateProcessA, Shim_CreateProcess);
    FAKE("kernel32", CreateProcessW, Shim_CreateProcess);
    FAKE("kernel32", ReadProcessMemory, Shim_ReadProcessMemory);
    FAKE("shell32", ShellExecuteA, Shim_ShellExecute);
    FAKE("shell32", ShellExecuteW, Shim_ShellExecute);
    FAKE("comdlg32", GetOpenFileNameA, Shim_FileDialog);
    FAKE("comdlg32", GetOpenFileNameW, Shim_FileDialog);
    FAKE("comdlg32", GetSaveFileNameA, Shim_FileDialog);
    FAKE("comdlg32", GetSaveFileNameW, Shim_FileDialog);
    FAKE("comdlg32", CommDlgExtendedError, Shim_CommDlgExtendedError);
#undef FAKE
}
