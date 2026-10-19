#include "pch.h"
#include <fstream>
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

    std::mutex g_threadLock;
    std::vector<HANDLE> g_threads;

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
        return name ? FakeModule(Narrow(name).c_str()) : g_process.base;
    }

    HMODULE WINAPI Shim_GetModuleHandleA(LPCSTR name)
    {
        return name ? FakeModule(name) : g_process.base;
    }

    DWORD WINAPI Shim_GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
    {
        return IsGameModule(module) ? CopyPath(g_process.exeW, buffer, size) : GetModuleFileNameW(module, buffer, size);
    }

    DWORD WINAPI Shim_GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
    {
        return IsGameModule(module) ? CopyPath(g_process.exeA, buffer, size) : GetModuleFileNameA(module, buffer, size);
    }

    bool InGameImage(const void* p)
    {
        auto base = reinterpret_cast<const uint8_t*>(g_process.base);
        if (!base) return false;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
    }

    BOOL WINAPI Shim_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module)
    {
        if (!module)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
        {
            if (InGameImage(name))
            {
                *module = g_process.base;
                return TRUE;
            }
            return GetModuleHandleExW(flags, name, module);
        }
        *module = Shim_GetModuleHandleW(name);
        return *module != nullptr;
    }

    // C++ throw and the unwinder find an address's image this way, and ntdll has never heard of the game's.
    PVOID NTAPI Shim_RtlPcToFileHeader(PVOID pc, PVOID* base)
    {
        HMODULE module = nullptr;
        if (InGameImage(pc))
            module = g_process.base;
        else
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(pc), &module);
        *base = module;
        return module;
    }

    // The app partition only has the FromApp forms. Executable pages are allowed by the codeGeneration capability.
    LPVOID WINAPI Shim_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
    {
        return VirtualAllocFromApp(address, size, type, protect);
    }

    BOOL WINAPI Shim_VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old)
    {
        return VirtualProtectFromApp(address, size, protect, reinterpret_cast<PULONG>(old));
    }

    // No parent process: nothing inherited, no window placement.
    VOID WINAPI Shim_GetStartupInfoW(LPSTARTUPINFOW info)
    {
        memset(info, 0, sizeof(*info));
        info->cb = sizeof(*info);
    }

    LPWSTR WINAPI Shim_GetCommandLineW() { return g_process.commandLineW.data(); }
    LPSTR WINAPI Shim_GetCommandLineA() { return g_process.commandLineA.data(); }

    // A game's threads outlive its main thread here, unlike in a real process that exits, so we keep
    // a handle on each to see whether any are still running when the game ends.
    // Every thread the game starts sets up the game's TLS before its own code runs.
    struct GameThreadStart
    {
        LPTHREAD_START_ROUTINE start;
        LPVOID parameter;
    };

    DWORD WINAPI RunGameThread(LPVOID context)
    {
        auto run = *static_cast<GameThreadStart*>(context);
        delete static_cast<GameThreadStart*>(context);
        EnterGameThread(DLL_THREAD_ATTACH);
        DWORD result = run.start(run.parameter);
        LeaveGameThread();
        return result;
    }

    HANDLE WINAPI Shim_CreateThread(LPSECURITY_ATTRIBUTES security, SIZE_T stackSize, LPTHREAD_START_ROUTINE start,
        LPVOID parameter, DWORD flags, LPDWORD threadId)
    {
        auto context = new GameThreadStart{ start, parameter };
        HANDLE thread = CreateThread(security, stackSize, &RunGameThread, context, flags, threadId);
        if (!thread)
            delete context;
        HANDLE copy = nullptr;
        if (thread && DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0))
        {
            std::lock_guard lock(g_threadLock);
            g_threads.push_back(copy);
        }
        return thread;
    }

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

    HRESULT WINAPI Shim_GetErrorInfo(ULONG, IUnknown** info)
    {
        if (info) *info = nullptr;
        return S_FALSE;
    }
    HRESULT WINAPI Shim_SetErrorInfo(ULONG, IUnknown*) { return S_OK; }

    // The game's "process" is itself; anything else is out of reach.
    BOOL WINAPI Shim_TerminateProcess(HANDLE process, UINT code)
    {
        if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
            Shim_ExitProcess(code);
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

void SetGameProcess(void* imageBase, std::filesystem::path const& exe)
{
    g_process.base = static_cast<HMODULE>(imageBase);
    g_process.exeW = exe.wstring();
    g_process.exeA = Narrow(g_process.exeW);
    g_process.commandLineW = L"\"" + g_process.exeW + L"\"";
    g_process.commandLineA = "\"" + g_process.exeA + "\"";

    // A game with no menu of its own can be pointed at what to do with host-args.txt next to its exe.
    std::ifstream argsFile(exe.parent_path() / L"host-args.txt");
    std::string args;
    if (argsFile && std::getline(argsFile, args) && !args.empty())
    {
        while (!args.empty() && (args.back() == '\r' || args.back() == ' ')) args.pop_back();
        g_process.commandLineA += " " + args;
        g_process.commandLineW += L" " + Widen(args.c_str());
        HOST_LOG(L"command line: %S", g_process.commandLineA.c_str());
    }
    g_process.mainThread = GetCurrentThreadId();
    SetGameRoot(exe.parent_path());
    g_process.exitCode = 0;
}

size_t GameThreadsAlive(DWORD waitMs)
{
    std::lock_guard lock(g_threadLock);
    size_t alive = 0;
    for (HANDLE& thread : g_threads)
    {
        if (thread && WaitForSingleObject(thread, waitMs) == WAIT_OBJECT_0)
        {
            CloseHandle(thread);
            thread = nullptr;
        }
        else if (thread)
            ++alive;
    }
    if (!alive)
        g_threads.clear();
    return alive;
}

unsigned long GameExitCode() { return g_process.exitCode; }

// kernel32 functions the game can call as they are. Taking the address of each
// one is what proves it is in the app partition. Left out on purpose: the
// file, path, process and loader calls, which need the sandbox mapping.
void RegisterKernel32(Win32Layer& layer)
{
    size_t count = 0;
#define PASS(dll, fn) (layer.Register(dll "!" #fn, reinterpret_cast<void*>(&fn)), ++count)

    layer.Register("kernel32!CreateThread", reinterpret_cast<void*>(&Shim_CreateThread));
    layer.Register("kernel32!ExitProcess", reinterpret_cast<void*>(&Shim_ExitProcess));
    layer.Register("kernel32!GetModuleHandleA", reinterpret_cast<void*>(&Shim_GetModuleHandleA));
    layer.Register("kernel32!GetModuleHandleW", reinterpret_cast<void*>(&Shim_GetModuleHandleW));
    layer.Register("kernel32!GetModuleFileNameA", reinterpret_cast<void*>(&Shim_GetModuleFileNameA));
    layer.Register("kernel32!GetModuleFileNameW", reinterpret_cast<void*>(&Shim_GetModuleFileNameW));
    layer.Register("kernel32!GetCommandLineA", reinterpret_cast<void*>(&Shim_GetCommandLineA));
    layer.Register("kernel32!GetCommandLineW", reinterpret_cast<void*>(&Shim_GetCommandLineW));
    layer.Register("kernel32!GetModuleHandleExW", reinterpret_cast<void*>(&Shim_GetModuleHandleExW));
    layer.Register("kernel32!RtlPcToFileHeader", reinterpret_cast<void*>(&Shim_RtlPcToFileHeader));
    layer.Register("kernel32!VirtualAlloc", reinterpret_cast<void*>(&Shim_VirtualAlloc));
    layer.Register("kernel32!VirtualProtect", reinterpret_cast<void*>(&Shim_VirtualProtect));
    layer.Register("kernel32!GetStartupInfoW", reinterpret_cast<void*>(&Shim_GetStartupInfoW));
    layer.Register("kernel32!TerminateProcess", reinterpret_cast<void*>(&Shim_TerminateProcess));

    PASS("kernel32", CloseHandle);
    PASS("kernel32", CreateEventA);
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
    PASS("kernel32", GetSystemTimeAsFileTime);
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

    // Statically linked CRTs (OpenTTD) call these directly; all exist in the app partition.
    PASS("kernel32", AcquireSRWLockExclusive);
    PASS("kernel32", AcquireSRWLockShared);
    PASS("kernel32", CloseThreadpoolTimer);
    PASS("kernel32", CloseThreadpoolWait);
    PASS("kernel32", CompareStringEx);
    PASS("kernel32", CompareStringW);
    PASS("kernel32", CreateEventExW);
    PASS("kernel32", CreateEventW);
    PASS("kernel32", CreateSemaphoreExW);
    PASS("kernel32", CreateSemaphoreW);
    PASS("kernel32", CreateThreadpoolTimer);
    PASS("kernel32", CreateThreadpoolWait);
    PASS("kernel32", DecodePointer);
    PASS("kernel32", DuplicateHandle);
    PASS("kernel32", EncodePointer);
    PASS("kernel32", ExitThread);
    PASS("kernel32", ExpandEnvironmentStringsW);
    PASS("kernel32", FlsAlloc);
    PASS("kernel32", FlsFree);
    PASS("kernel32", FlsGetValue);
    PASS("kernel32", FlsSetValue);
    PASS("kernel32", FlushFileBuffers);
    PASS("kernel32", FlushProcessWriteBuffers);
    PASS("kernel32", FormatMessageA);
    PASS("kernel32", FreeEnvironmentStringsW);
    PASS("kernel32", FreeLibraryWhenCallbackReturns);
    PASS("kernel32", GetACP);
    PASS("kernel32", GetCPInfo);
    PASS("kernel32", GetCurrentProcess);
    PASS("kernel32", GetCurrentProcessorNumber);
    PASS("kernel32", GetCurrentThreadId);
    PASS("kernel32", GetEnvironmentStringsW);
    PASS("kernel32", GetExitCodeThread);
    PASS("kernel32", GetFileInformationByHandle);
    PASS("kernel32", GetFileInformationByHandleEx);
    PASS("kernel32", GetFileSizeEx);
    PASS("kernel32", GetFileType);
    PASS("kernel32", GetFinalPathNameByHandleW);
    PASS("kernel32", GetLocaleInfoA);
    PASS("kernel32", GetLocaleInfoEx);
    PASS("kernel32", GetLocaleInfoW);
    PASS("kernel32", GetNativeSystemInfo);
    PASS("kernel32", GetProcessId);
    PASS("kernel32", GetStringTypeW);
    PASS("kernel32", GetSystemInfo);
    PASS("kernel32", GetThreadContext);
    PASS("kernel32", GetTickCount);
    PASS("kernel32", GetTickCount64);
    PASS("kernel32", GetTimeZoneInformation);
    PASS("kernel32", GetUserDefaultUILanguage);
    PASS("kernel32", GlobalMemoryStatusEx);
    PASS("kernel32", HeapSize);
    PASS("kernel32", InitOnceExecuteOnce);
    PASS("kernel32", InitializeCriticalSectionAndSpinCount);
    PASS("kernel32", InitializeCriticalSectionEx);
    PASS("kernel32", InitializeSListHead);
    PASS("kernel32", InterlockedFlushSList);
    PASS("kernel32", InterlockedPushEntrySList);
    PASS("kernel32", IsProcessorFeaturePresent);
    PASS("kernel32", IsThreadAFiber);
    PASS("kernel32", IsValidCodePage);
    PASS("kernel32", LCMapStringEx);
    PASS("kernel32", MultiByteToWideChar);
    PASS("kernel32", OpenThread);
    PASS("kernel32", OutputDebugStringW);
    PASS("kernel32", RaiseException);
    PASS("kernel32", ReleaseSRWLockExclusive);
    PASS("kernel32", ReleaseSRWLockShared);
    PASS("kernel32", ReleaseSemaphore);
    PASS("kernel32", ResetEvent);
    PASS("kernel32", RtlCaptureContext);
    PASS("kernel32", RtlLookupFunctionEntry);
    PASS("kernel32", RtlUnwind);
    PASS("kernel32", RtlUnwindEx);
    PASS("kernel32", RtlVirtualUnwind);
    PASS("kernel32", SetEndOfFile);
    PASS("kernel32", SetEnvironmentVariableW);
    PASS("kernel32", SetFileInformationByHandle);
    PASS("kernel32", SetFilePointerEx);
    PASS("kernel32", SetLastError);
    PASS("kernel32", SetThreadpoolTimer);
    PASS("kernel32", SetThreadpoolWait);
    PASS("kernel32", SignalObjectAndWait);
    PASS("kernel32", SleepConditionVariableSRW);
    PASS("kernel32", SuspendThread);
    PASS("kernel32", SwitchToThread);
    PASS("kernel32", TlsAlloc);
    PASS("kernel32", TlsFree);
    PASS("kernel32", TlsGetValue);
    PASS("kernel32", TlsSetValue);
    PASS("kernel32", TryAcquireSRWLockExclusive);
    PASS("kernel32", TryAcquireSRWLockShared);
    PASS("kernel32", WaitForMultipleObjects);
    PASS("kernel32", WaitForSingleObjectEx);
    PASS("kernel32", WaitForThreadpoolTimerCallbacks);
    PASS("kernel32", WakeAllConditionVariable);
    PASS("kernel32", WakeConditionVariable);
    PASS("kernel32", WideCharToMultiByte);

    // Looked up at run time by the static CRT, which falls back when one is missing.
    PASS("kernel32", GetSystemTimePreciseAsFileTime);
    PASS("kernel32", EnumSystemLocalesEx);
    PASS("kernel32", GetDateFormatEx);
    PASS("kernel32", GetTimeFormatEx);
    PASS("kernel32", GetUserDefaultLocaleName);
    PASS("kernel32", IsValidLocaleName);
    PASS("kernel32", LCIDToLocaleName);
    PASS("kernel32", LocaleNameToLCID);

    // COM error info (OpenTTD imports these from oleaut32 by ordinal: 200 and 201). They aren't in
    // the app partition, and a stub GetErrorInfo would "succeed" leaving its out pointer for the
    // caller to release; there is never any error info here.
    for (const char* key : { "oleaut32!GetErrorInfo", "oleaut32!#200" })
        layer.Register(key, reinterpret_cast<void*>(&Shim_GetErrorInfo));
    for (const char* key : { "oleaut32!SetErrorInfo", "oleaut32!#201" })
        layer.Register(key, reinterpret_cast<void*>(&Shim_SetErrorInfo));

    // The host CRT stands in for msvcrt.
    PASS("msvcrt", memcpy);
    PASS("msvcrt", memmove);
    PASS("msvcrt", memset);
    PASS("msvcrt", strlen);
    PASS("msvcrt", longjmp);
#undef PASS

    HOST_LOG(L"pass-through: %zu imports", count);
}
