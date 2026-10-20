#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

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

    BOOL WINAPI Shim_GetVersionExW(LPOSVERSIONINFOW info)
    {
        if (!info || info->dwOSVersionInfoSize < sizeof(OSVERSIONINFOW))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        info->dwMajorVersion = 10;
        info->dwMinorVersion = 0;
        info->dwBuildNumber = 19045;
        info->dwPlatformId = VER_PLATFORM_WIN32_NT;
        info->szCSDVersion[0] = L'\0';
        if (info->dwOSVersionInfoSize >= sizeof(OSVERSIONINFOEXW))
        {
            auto ex = reinterpret_cast<OSVERSIONINFOEXW*>(info);
            ex->wServicePackMajor = ex->wServicePackMinor = 0;
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

    // Never a debugger: the host may be under one, and games change behavior when they see it.
    BOOL WINAPI Shim_IsDebuggerPresent() { return FALSE; }

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

    // There is no console. Output goes to host.log; input never arrives.
    BOOL WINAPI Shim_AllocConsole() { return Denied(); }
    BOOL WINAPI Shim_FreeConsole() { return TRUE; }
    BOOL WINAPI Shim_GetConsoleMode(HANDLE, LPDWORD) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    UINT WINAPI Shim_GetConsoleOutputCP() { return CP_UTF8; }
    BOOL WINAPI Shim_ConsoleFails(HANDLE, LPVOID) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    BOOL WINAPI Shim_ConsoleSetter(HANDLE, ULONG_PTR) { return TRUE; }
    BOOL WINAPI Shim_SetConsoleTitleW(LPCWSTR) { return TRUE; }
    BOOL WINAPI Shim_SetConsoleCtrlHandler(LPVOID, BOOL) { return TRUE; }
    BOOL WINAPI Shim_ReadConsoleW(HANDLE, LPVOID, DWORD, LPDWORD read, LPVOID)
    {
        if (read) *read = 0;
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    BOOL WINAPI Shim_WriteConsoleW(HANDLE, const VOID* text, DWORD length, LPDWORD written, LPVOID)
    {
        HOST_LOG(L"console: %.*s", static_cast<int>(length), static_cast<const wchar_t*>(text));
        if (written) *written = length;
        return TRUE;
    }

    UINT WINAPI Shim_SetErrorMode(UINT) { return 0; }
    BOOL WINAPI Shim_AreFileApisANSI() { return TRUE; }
    UINT WINAPI Shim_GetOEMCP() { return 437; }

    // Crash handlers the game installs are recorded nowhere; the host's SEH handler reports crashes.
    PVOID WINAPI Shim_AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER handler)
    {
        return reinterpret_cast<PVOID>(handler); // non-null so the game believes it worked
    }
    LONG WINAPI Shim_UnhandledExceptionFilter(EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }

    // The same Windows 10 GetVersionExA reports.
    ULONGLONG WINAPI Shim_VerSetConditionMask(ULONGLONG mask, DWORD typeMask, BYTE condition)
    {
        for (int bit = 0; bit < 8; ++bit)
            if (typeMask & (1u << bit))
                mask |= static_cast<ULONGLONG>(condition & 7) << (bit * 3);
        return mask;
    }
    BOOL WINAPI Shim_VerifyVersionInfoW(LPOSVERSIONINFOEXW info, DWORD typeMask, DWORDLONG)
    {
        // Games ask "at least version X"; only a build newer than ours fails.
        if ((typeMask & VER_BUILDNUMBER) && info->dwBuildNumber > 19045)
        {
            SetLastError(ERROR_OLD_WIN_VERSION);
            return FALSE;
        }
        return TRUE;
    }

    BOOL WINAPI Shim_TerminateThread(HANDLE, DWORD) { return Denied(); }

    // The game's CRT asks what kind of app it is. The real answers describe the host (a UWP app), which
    // would have it initialize WinRT on every thread; a plain desktop process is what it was built for.
    LONG WINAPI Shim_AppPolicyZero(HANDLE, int* policy) { *policy = 0; return ERROR_SUCCESS; }

    // The host doesn't change the system timer; Sleep is fine-grained enough on the console.
    UINT WINAPI Shim_TimePeriod(UINT) { return 0; } // TIMERR_NOERROR

    SIZE_T WINAPI Shim_VirtualQueryEx(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size)
    {
        if (process == GetCurrentProcess())
            return VirtualQuery(address, info, size);
        Denied();
        return 0;
    }

    // No input method editor: there is no IME context, so games type plain characters.
    LONG_PTR WINAPI Shim_ImmNone() { return 0; }
    BOOL WINAPI Shim_ImmTrue() { return TRUE; }

    // No WinHTTP in the app partition; games treat a failed WinHttpOpen as "no internet".
    LONG_PTR WINAPI Shim_WinHttpUnsupported()
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }

    // No Uniscribe in the app partition. Its stubs used to "succeed" with the outputs left unset; a
    // clean failure sends OpenTTD (and others) to their own layout and caret code instead.
    HRESULT WINAPI Shim_UspFail() { return E_FAIL; }
    HRESULT WINAPI Shim_ScriptFreeCache(void** cache)
    {
        if (cache) *cache = nullptr;
        return S_OK;
    }

    // Crash handlers walk the process; there is nothing of the game's to list beyond its own image.
    HANDLE WINAPI Shim_CreateToolhelp32Snapshot(DWORD, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    BOOL WINAPI Shim_ToolhelpNone(HANDLE, void*) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    HANDLE WINAPI Shim_OpenProcess(DWORD, BOOL, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    BOOL WINAPI Shim_K32EnumProcessModules(HANDLE, HMODULE*, DWORD, LPDWORD needed) { if (needed) *needed = 0; return Denied(); }
    DWORD WINAPI Shim_K32GetModuleFileNameExA(HANDLE, HMODULE, LPSTR, DWORD) { Denied(); return 0; }

    // The process heap is the host's and is fine; a game's "heap validation failed" means our stub, not its heap.
    BOOL WINAPI Shim_HeapValidate(HANDLE, DWORD, LPCVOID) { return TRUE; }

    // Not attached to a console, and no power management to keep awake.
    BOOL WINAPI Shim_AttachConsole(DWORD) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    DWORD WINAPI Shim_SetThreadExecutionState(DWORD) { return 0x80000000; } // previous: ES_CONTINUOUS

    // COM: the app partition has CoInitializeEx; proxy security is the host's.
    HRESULT WINAPI Shim_CoInitialize(LPVOID) { return CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    HRESULT WINAPI Shim_CoSetProxyBlanket(void*, DWORD, DWORD, void*, DWORD, DWORD, void*, DWORD) { return S_OK; }

    // The shell has no folders to open and nothing to drop files onto.
    void* WINAPI Shim_ILCreateFromPathW(LPCWSTR) { return nullptr; }
    void WINAPI Shim_ILFree(void*) {}
    HRESULT WINAPI Shim_SHOpenFolderAndSelectItems(void*, UINT, void*, DWORD) { return E_NOTIMPL; }
    void WINAPI Shim_DragAcceptFiles(HWND, BOOL) {}
    BOOL WINAPI Shim_ShellExecuteExW(void*) { return Denied(); }

    BOOL WINAPI Shim_GetUserNameA(LPSTR buffer, LPDWORD size)
    {
        static const char name[] = "Player";
        if (!size || *size < sizeof(name)) { if (size) *size = sizeof(name); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy(buffer, name, sizeof(name));
        *size = sizeof(name);
        return TRUE;
    }

    // The common dialogs aren't in the app partition. FALSE with no extended
    // error is what the game sees when the user cancels.
    BOOL WINAPI Shim_FileDialog(void*) { return FALSE; }
    DWORD WINAPI Shim_CommDlgExtendedError() { return 0; }

    // HID devices aren't reachable from the app partition; games (SDL's hidapi) see a system with
    // none, so its setup succeeds once instead of failing on every poll. Pads come through XInput.
    BOOLEAN WINAPI Shim_HidFalse() { SetLastError(ERROR_INVALID_HANDLE); return FALSE; } // every HidD_ call needs a device
    void WINAPI Shim_HidGetHidGuid(GUID* guid)
    {
        if (guid) *guid = { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    }
    LONG WINAPI Shim_HidPInvalid() { return static_cast<LONG>(0xC0110001); } // HIDP_STATUS_INVALID_PREPARSED_DATA

    HANDLE WINAPI Shim_SetupDiGetClassDevs(const GUID*, LPCVOID, HWND, DWORD) { return INVALID_HANDLE_VALUE; }
    BOOL WINAPI Shim_SetupDiNoMoreItems() { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    BOOL WINAPI Shim_SetupDiDestroyDeviceInfoList(HANDLE) { return TRUE; }

    // Notifications are accepted and never fire; device nodes don't exist.
    DWORD WINAPI Shim_CMRegisterNotification(void*, void*, void*, HANDLE* context)
    {
        if (context) *context = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00030010));
        return 0; // CR_SUCCESS
    }
    DWORD WINAPI Shim_CMSuccess() { return 0; }
    DWORD WINAPI Shim_CMNoSuchDevnode() { return 0x0D; } // CR_NO_SUCH_DEVNODE
    DWORD WINAPI Shim_CMEmptyListSize(PULONG size, LPCVOID, LPCVOID, ULONG) { if (size) *size = 1; return 0; }
    DWORD WINAPI Shim_CMEmptyListW(LPCVOID, LPCVOID, PWSTR buffer, ULONG length, ULONG)
    {
        if (!buffer || length < 1) return 0x1A; // CR_BUFFER_SMALL
        buffer[0] = 0;
        return 0;
    }
    DWORD WINAPI Shim_CMEmptyListA(LPCVOID, LPCVOID, PSTR buffer, ULONG length, ULONG)
    {
        if (!buffer || length < 1) return 0x1A;
        buffer[0] = 0;
        return 0;
    }
}

void RegisterFakes(Win32Layer& layer)
{
#define FAKE(dll, fn, shim) layer.Register(dll "!" #fn, reinterpret_cast<void*>(&shim))
    FAKE("kernel32", IsDebuggerPresent, Shim_IsDebuggerPresent);
    FAKE("kernel32", GetVersionExA, Shim_GetVersionExA);
    FAKE("kernel32", GetVersionExW, Shim_GetVersionExW);
    FAKE("kernel32", CreateToolhelp32Snapshot, Shim_CreateToolhelp32Snapshot);
    FAKE("kernel32", Process32First, Shim_ToolhelpNone);
    FAKE("kernel32", Process32Next, Shim_ToolhelpNone);
    FAKE("kernel32", Thread32First, Shim_ToolhelpNone);
    FAKE("kernel32", Thread32Next, Shim_ToolhelpNone);
    FAKE("kernel32", OpenProcess, Shim_OpenProcess);
    FAKE("kernel32", K32EnumProcessModules, Shim_K32EnumProcessModules);
    FAKE("kernel32", K32GetModuleFileNameExA, Shim_K32GetModuleFileNameExA);
    FAKE("kernel32", HeapValidate, Shim_HeapValidate);
    FAKE("kernel32", AttachConsole, Shim_AttachConsole);
    FAKE("kernel32", SetThreadExecutionState, Shim_SetThreadExecutionState);
    FAKE("ole32", CoInitialize, Shim_CoInitialize);
    FAKE("ole32", CoSetProxyBlanket, Shim_CoSetProxyBlanket);
    FAKE("shell32", ILCreateFromPathW, Shim_ILCreateFromPathW);
    FAKE("shell32", ILFree, Shim_ILFree);
    FAKE("shell32", SHOpenFolderAndSelectItems, Shim_SHOpenFolderAndSelectItems);
    FAKE("shell32", DragAcceptFiles, Shim_DragAcceptFiles);
    FAKE("shell32", ShellExecuteExW, Shim_ShellExecuteExW);
    FAKE("advapi32", GetUserNameA, Shim_GetUserNameA);
    layer.Register("shell32!#190", reinterpret_cast<void*>(&Shim_ILCreateFromPathW));
    layer.Register("shell32!#155", reinterpret_cast<void*>(&Shim_ILFree));
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
    FAKE("kernel32", AllocConsole, Shim_AllocConsole);
    FAKE("kernel32", FreeConsole, Shim_FreeConsole);
    FAKE("kernel32", GetConsoleMode, Shim_GetConsoleMode);
    FAKE("kernel32", GetConsoleOutputCP, Shim_GetConsoleOutputCP);
    FAKE("kernel32", GetConsoleScreenBufferInfo, Shim_ConsoleFails);
    FAKE("kernel32", SetConsoleOutputCP, Shim_ConsoleSetter);
    FAKE("kernel32", SetConsoleScreenBufferSize, Shim_ConsoleSetter);
    FAKE("kernel32", SetConsoleTitleW, Shim_SetConsoleTitleW);
    FAKE("kernel32", SetConsoleCtrlHandler, Shim_SetConsoleCtrlHandler);
    FAKE("kernel32", ReadConsoleW, Shim_ReadConsoleW);
    FAKE("kernel32", WriteConsoleW, Shim_WriteConsoleW);
    FAKE("kernel32", SetErrorMode, Shim_SetErrorMode);
    FAKE("kernel32", AreFileApisANSI, Shim_AreFileApisANSI);
    FAKE("kernel32", GetOEMCP, Shim_GetOEMCP);
    FAKE("kernel32", AddVectoredExceptionHandler, Shim_AddVectoredExceptionHandler);
    FAKE("kernel32", UnhandledExceptionFilter, Shim_UnhandledExceptionFilter);
    FAKE("kernel32", VerSetConditionMask, Shim_VerSetConditionMask);
    FAKE("kernel32", VerifyVersionInfoW, Shim_VerifyVersionInfoW);
    // The A struct only differs after szCSDVersion; the build number the shim reads is at the same offset.
    FAKE("kernel32", VerifyVersionInfoA, Shim_VerifyVersionInfoW);
    FAKE("kernel32", TerminateThread, Shim_TerminateThread);
    FAKE("kernel32", VirtualQueryEx, Shim_VirtualQueryEx);
    FAKE("winmm", timeBeginPeriod, Shim_TimePeriod);
    for (const char* name : { "ImmGetContext", "ImmGetProperty", "ImmSetCompositionWindow", "ImmSetCandidateWindow",
        "ImmNotifyIME", "ImmGetCompositionStringW", "ImmAssociateContext" })
        layer.Register((std::string("imm32!") + name).c_str(), reinterpret_cast<void*>(&Shim_ImmNone));
    layer.Register("imm32!ImmReleaseContext", reinterpret_cast<void*>(&Shim_ImmTrue));
    for (const char* name : { "ScriptItemize", "ScriptLayout", "ScriptShape", "ScriptPlace", "ScriptGetLogicalWidths",
        "ScriptBreak", "ScriptStringAnalyse", "ScriptTextOut" })
        layer.Register((std::string("usp10!") + name).c_str(), reinterpret_cast<void*>(&Shim_UspFail));
    layer.Register("usp10!ScriptFreeCache", reinterpret_cast<void*>(&Shim_ScriptFreeCache));
    for (const char* name : { "WinHttpOpen", "WinHttpConnect", "WinHttpOpenRequest", "WinHttpSendRequest",
        "WinHttpReceiveResponse", "WinHttpQueryDataAvailable", "WinHttpReadData", "WinHttpQueryHeaders",
        "WinHttpCrackUrl", "WinHttpSetStatusCallback", "WinHttpSetTimeouts", "WinHttpCloseHandle" })
        layer.Register((std::string("winhttp!") + name).c_str(), reinterpret_cast<void*>(&Shim_WinHttpUnsupported));
    FAKE("kernel32", AppPolicyGetProcessTerminationMethod, Shim_AppPolicyZero); // ExitProcess
    FAKE("kernel32", AppPolicyGetThreadInitializationType, Shim_AppPolicyZero); // none
    FAKE("kernel32", AppPolicyGetShowDeveloperDiagnostic, Shim_AppPolicyZero);  // none
    FAKE("kernel32", AppPolicyGetWindowingModel, Shim_AppPolicyZero);           // none
    FAKE("winmm", timeEndPeriod, Shim_TimePeriod);
    for (const char* fn : { "HidD_GetAttributes", "HidD_GetSerialNumberString", "HidD_GetManufacturerString",
        "HidD_GetProductString", "HidD_SetFeature", "HidD_GetFeature", "HidD_GetInputReport", "HidD_GetIndexedString",
        "HidD_GetPreparsedData", "HidD_FreePreparsedData", "HidD_SetNumInputBuffers", "HidD_SetOutputReport",
        "HidD_GetNumInputBuffers", "HidD_FlushQueue", "HidD_GetPhysicalDescriptor" })
        layer.Register((std::string("hid!") + fn).c_str(), reinterpret_cast<void*>(&Shim_HidFalse));
    FAKE("hid", HidD_GetHidGuid, Shim_HidGetHidGuid);
    // Not HidP_GetButtonCaps, GetValueCaps, GetData or MaxDataListLength: with them SDL starts its raw-input
    // joystick driver, which then claims XInput pads and finds none (raw input has no devices here).
    for (const char* fn : { "HidP_GetCaps", "HidP_GetUsages",
        "HidP_GetUsageValue", "HidP_GetScaledUsageValue", "HidP_GetSpecificButtonCaps", "HidP_GetSpecificValueCaps" })
        layer.Register((std::string("hid!") + fn).c_str(), reinterpret_cast<void*>(&Shim_HidPInvalid));
    FAKE("setupapi", SetupDiGetClassDevsA, Shim_SetupDiGetClassDevs);
    FAKE("setupapi", SetupDiGetClassDevsW, Shim_SetupDiGetClassDevs);
    for (const char* fn : { "SetupDiEnumDeviceInterfaces", "SetupDiEnumDeviceInfo", "SetupDiGetDeviceInterfaceDetailA",
        "SetupDiGetDeviceInterfaceDetailW", "SetupDiGetDeviceRegistryPropertyA", "SetupDiGetDeviceRegistryPropertyW" })
        layer.Register((std::string("setupapi!") + fn).c_str(), reinterpret_cast<void*>(&Shim_SetupDiNoMoreItems));
    FAKE("setupapi", SetupDiDestroyDeviceInfoList, Shim_SetupDiDestroyDeviceInfoList);
    for (const char* dll : { "cfgmgr32", "setupapi" })
    {
        std::string d = std::string(dll) + "!";
        layer.Register((d + "CM_Register_Notification").c_str(), reinterpret_cast<void*>(&Shim_CMRegisterNotification));
        layer.Register((d + "CM_Unregister_Notification").c_str(), reinterpret_cast<void*>(&Shim_CMSuccess));
        for (const char* fn : { "CM_Locate_DevNodeA", "CM_Locate_DevNodeW", "CM_Get_Parent", "CM_Get_Device_IDA",
            "CM_Get_Device_IDW", "CM_Get_DevNode_PropertyW", "CM_Get_Device_Interface_PropertyW" })
            layer.Register((d + fn).c_str(), reinterpret_cast<void*>(&Shim_CMNoSuchDevnode));
        layer.Register((d + "CM_Get_Device_Interface_List_SizeW").c_str(), reinterpret_cast<void*>(&Shim_CMEmptyListSize));
        layer.Register((d + "CM_Get_Device_Interface_List_SizeA").c_str(), reinterpret_cast<void*>(&Shim_CMEmptyListSize));
        layer.Register((d + "CM_Get_Device_Interface_ListW").c_str(), reinterpret_cast<void*>(&Shim_CMEmptyListW));
        layer.Register((d + "CM_Get_Device_Interface_ListA").c_str(), reinterpret_cast<void*>(&Shim_CMEmptyListA));
    }
#undef FAKE
}
