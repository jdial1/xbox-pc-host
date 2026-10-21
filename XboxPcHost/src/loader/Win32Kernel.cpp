#include "pch.h"
#include <fstream>
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
// In WindowsApp.lib, though the app partition's headers don't declare it (RwxFault needs it early).
extern "C" __declspec(dllimport) PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
#endif

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

    // A game DLL by its file name (with or without a path), else the fake module for that name.
    HMODULE ModuleNamed(std::string const& name)
    {
        std::string file = name.substr(name.find_last_of("\\/") == std::string::npos ? 0 : name.find_last_of("\\/") + 1);
        if (HMODULE dll = GameDllNamed(file.c_str()))
            return dll;
        return FakeModule(name.c_str());
    }

    HMODULE WINAPI Shim_GetModuleHandleW(LPCWSTR name)
    {
        return name ? ModuleNamed(Narrow(name)) : g_process.base;
    }

    HMODULE WINAPI Shim_GetModuleHandleA(LPCSTR name)
    {
        return name ? ModuleNamed(name) : g_process.base;
    }

    DWORD WINAPI Shim_GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
    {
        if (IsGameModule(module)) return CopyPath(g_process.exeW, buffer, size);
        if (GameDllContaining(module)) return CopyPath(GameDllPath(module).wstring(), buffer, size);
        return GetModuleFileNameW(module, buffer, size);
    }

    DWORD WINAPI Shim_GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
    {
        if (IsGameModule(module)) return CopyPath(g_process.exeA, buffer, size);
        if (GameDllContaining(module)) return CopyPath(Narrow(GameDllPath(module).wstring().c_str()), buffer, size);
        return GetModuleFileNameA(module, buffer, size);
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
            if (HMODULE dll = GameDllContaining(name))
            {
                *module = dll;
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
        else if (HMODULE dll = GameDllContaining(pc))
            module = dll;
        else
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(pc), &module);
        *base = module;
        return module;
    }

    // ---- Read-write-execute memory, emulated (JITs such as Unity's Mono ask for it).
    // The FromApp forms allow executable pages (the codeGeneration capability) but never writable and
    // executable at once. An RWX request becomes read-write and its range is remembered; a page flips
    // to read-execute when code in it runs (an execute fault) and back to read-write when it is
    // written (a write fault). Faults outside those ranges are left to everyone else.
    std::mutex g_rwxLock;
    std::map<uintptr_t, uintptr_t> g_rwx; // begin -> end
    std::atomic<bool> g_rwxHandler{ false };
    PVOID g_rwxHandle = nullptr;

    bool InRwx(uintptr_t a)
    {
        std::lock_guard lock(g_rwxLock);
        auto it = g_rwx.upper_bound(a);
        return it != g_rwx.begin() && a < std::prev(it)->second;
    }

    LONG CALLBACK RwxFault(EXCEPTION_POINTERS* info)
    {
        auto const* r = info->ExceptionRecord;
        if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2)
            return EXCEPTION_CONTINUE_SEARCH;
        ULONG_PTR kind = r->ExceptionInformation[0], at = r->ExceptionInformation[1];
        if ((kind != 8 && kind != 1) || !InRwx(at))
            return EXCEPTION_CONTINUE_SEARCH;
        ULONG old = 0;
        void* page = reinterpret_cast<void*>(at & ~static_cast<ULONG_PTR>(0xFFF));
        if (!VirtualProtectFromApp(page, 0x1000, kind == 8 ? PAGE_EXECUTE_READ : PAGE_READWRITE, &old))
            return EXCEPTION_CONTINUE_SEARCH;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    bool IsRwx(DWORD protect) { return (protect & 0xFF) == PAGE_EXECUTE_READWRITE || (protect & 0xFF) == PAGE_EXECUTE_WRITECOPY; }
    DWORD WithoutExecute(DWORD protect) { return (protect & ~0xFFu) | PAGE_READWRITE; }

    void RememberRwx(void* p, SIZE_T size)
    {
        if (!g_rwxHandler.exchange(true))
            g_rwxHandle = AddVectoredExceptionHandler(1, RwxFault); // first in line: these faults are expected
        auto begin = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(0xFFF);
        auto end = (reinterpret_cast<uintptr_t>(p) + size + 0xFFF) & ~static_cast<uintptr_t>(0xFFF);
        std::lock_guard lock(g_rwxLock);
        auto it = g_rwx.upper_bound(begin);
        if (it != g_rwx.begin() && std::prev(it)->second >= begin)
        {
            --it; // extend an overlapping or touching range
            begin = it->first;
            end = std::max(end, it->second);
            g_rwx.erase(it);
        }
        g_rwx[begin] = end;
    }

    // The app partition only has the FromApp forms. Executable pages are allowed by the codeGeneration capability.
    LPVOID WINAPI Shim_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
    {
        if (!IsRwx(protect))
            return VirtualAllocFromApp(address, size, type, protect);
        void* p = VirtualAllocFromApp(address, size, type, WithoutExecute(protect));
        if (p)
            RememberRwx(p, size);
        return p;
    }

    BOOL WINAPI Shim_VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old)
    {
        if (!IsRwx(protect))
            return VirtualProtectFromApp(address, size, protect, reinterpret_cast<PULONG>(old));
        BOOL ok = VirtualProtectFromApp(address, size, WithoutExecute(protect), reinterpret_cast<PULONG>(old));
        if (ok)
            RememberRwx(address, size);
        return ok;
    }

    // Releasing a region forgets its emulated RWX range, so later memory at those addresses is ordinary.
    BOOL WINAPI Shim_VirtualFree(LPVOID address, SIZE_T size, DWORD type)
    {
        if (type & MEM_RELEASE)
        {
            std::lock_guard lock(g_rwxLock);
            g_rwx.erase(reinterpret_cast<uintptr_t>(address));
        }
        return VirtualFree(address, size, type);
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
        constexpr DWORD kAccess = SYNCHRONIZE | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
        if (thread && DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &copy, kAccess, FALSE, 0))
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
        LogGameStack(L"ExitProcess from");
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

    BOOL WINAPI Shim_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module)
    {
        if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
            return Shim_GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(name), module);
        return Shim_GetModuleHandleExW(flags, name ? Widen(name).c_str() : nullptr, module);
    }

    struct ModuleInfo { LPVOID base; DWORD size; LPVOID entry; }; // MODULEINFO
    BOOL WINAPI Shim_K32GetModuleInformation(HANDLE, HMODULE module, ModuleInfo* info, DWORD)
    {
        if (!info || !IsGameModule(module))
        {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        auto base = reinterpret_cast<uint8_t*>(g_process.base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        *info = { base, nt->OptionalHeader.SizeOfImage, base + nt->OptionalHeader.AddressOfEntryPoint };
        return TRUE;
    }

    // C++ throw ends up here (code 0xE06D7363). Logging the first throws' stacks shows where an
    // error that a game catches and reports somewhere else really came from.
    std::atomic<int> g_throwsLogged{ 0 };
    VOID WINAPI Shim_RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR* arguments)
    {
        if (code == 0xE06D7363 && g_throwsLogged++ < 16)
            LogGameStack(L"C++ exception thrown");
        RaiseException(code, flags, count, arguments);
    }

    // The game's "process" is itself; anything else is out of reach.
    BOOL WINAPI Shim_TerminateProcess(HANDLE process, UINT code)
    {
        if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
            Shim_ExitProcess(code);
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
}

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
// In WindowsApp.lib, though the app partition's headers don't declare it.
extern "C" __declspec(dllimport) PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
#endif

namespace
{
    // Games catch their own crashes (Factorio turns an access violation into SIGSEGV and a report
    // of its own), so the host's handler never sees them. A first-chance look logs where the first
    // few faults happened before the game's handlers get them.
    std::atomic<int> g_faultsLogged{ 0 };
    std::atomic<bool> g_faultLoggerAdded{ false };
    LONG CALLBACK LogFirstChanceFault(EXCEPTION_POINTERS* info)
    {
        auto const* record = info->ExceptionRecord;
        DWORD code = record->ExceptionCode;
        bool fault = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
            code == STATUS_HEAP_CORRUPTION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || code == STATUS_INVALID_HANDLE;
        if (!fault || g_faultsLogged++ >= 8)
            return EXCEPTION_CONTINUE_SEARCH;
        auto at = static_cast<uint8_t*>(record->ExceptionAddress);
        bool inGame = InGameImage(at);
        auto offset = static_cast<unsigned long long>(inGame ? at - reinterpret_cast<uint8_t*>(g_process.base) : reinterpret_cast<uintptr_t>(at));
        if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            auto kind = record->ExceptionInformation[0];
            HOST_LOG(L"first-chance access violation (%s 0x%llx) at %s0x%llx", kind == 1 ? L"write" : kind == 8 ? L"execute" : L"read",
                static_cast<unsigned long long>(record->ExceptionInformation[1]), inGame ? L"game+" : L"", offset);
        }
        else
            HOST_LOG(L"first-chance exception 0x%08X at %s0x%llx", static_cast<unsigned>(code), inGame ? L"game+" : L"", offset);
        auto const* c = info->ContextRecord;
        HOST_LOG(L"  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx rsp=%llx", c->Rax, c->Rbx, c->Rcx,
            c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9, c->Rsp);
        LogGameStack(L"fault", c);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

std::filesystem::path GameExePath() { return g_process.exeW; }

namespace
{
    // Strict handle checks turn a bad CloseHandle into an exception; on the desktop it just fails.
    bool CloseHandleGuarded(HANDLE handle, BOOL& result)
    {
        __try
        {
            result = CloseHandle(handle);
            return true;
        }
        __except (GetExceptionCode() == STATUS_INVALID_HANDLE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    std::atomic<int> g_badCloses{ 0 };
    BOOL WINAPI Shim_CloseHandle(HANDLE handle)
    {
        // The host's own stand-in handles land here too, and fail quietly like any other bad handle.
        BOOL result = FALSE;
        if (CloseHandleGuarded(handle, result))
            return result;
        if (g_badCloses++ < 8)
        {
            HOST_LOG(L"CloseHandle(%p): not a handle", handle);
            LogGameStack(L"bad CloseHandle from");
        }
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
}

// A game's own vectored handler went in at the head; put the RWX page flipper back in front of it,
// or a JIT's handler would take an expected execute fault for a crash.
void KeepHostFaultHandlerFirst()
{
    if (!g_rwxHandle)
        return;
    RemoveVectoredExceptionHandler(g_rwxHandle);
    g_rwxHandle = AddVectoredExceptionHandler(1, RwxFault);
}

void SetGameProcess(void* imageBase, std::filesystem::path const& exe, std::filesystem::path const& root)
{
    // Not a function-local static: its guard would add a thread_local to the host's TLS block.
    if (!g_faultLoggerAdded.exchange(true))
        AddVectoredExceptionHandler(1, LogFirstChanceFault);
    g_process.base = static_cast<HMODULE>(imageBase);
    g_process.exeW = exe.wstring();
    g_process.exeA = Narrow(g_process.exeW);
    g_process.commandLineW = L"\"" + g_process.exeW + L"\"";
    g_process.commandLineA = "\"" + g_process.exeA + "\"";

    // A game with no menu of its own can be pointed at what to do with host-args.txt, in its
    // folder or next to its exe.
    std::ifstream argsFile(root / L"host-args.txt");
    if (!argsFile)
        argsFile.open(exe.parent_path() / L"host-args.txt");
    std::string args;
    if (argsFile && std::getline(argsFile, args) && !args.empty())
    {
        while (!args.empty() && (args.back() == '\r' || args.back() == ' ')) args.pop_back();
        g_process.commandLineA += " " + args;
        g_process.commandLineW += L" " + Widen(args.c_str());
        HOST_LOG(L"command line: %S", g_process.commandLineA.c_str());
    }
    g_process.mainThread = GetCurrentThreadId();
    SetGameRoot(root, exe.parent_path());
    g_process.exitCode = 0;
}

void LogGameStack(const wchar_t* why, const CONTEXT* from)
{
    // Unwind frame by frame with the tables RtlAddFunctionTable registered for the game's image
    // (RtlCaptureStackBackTrace gives up inside it).
    void* frames[40];
    USHORT count = 0;
    CONTEXT context{};
    if (from)
    {
        context = *from;
        if (!context.Rip) // a call through a null pointer: the caller's return address is on top
        {
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
    }
    else
        RtlCaptureContext(&context);
    while (count < 40 && context.Rip)
    {
        frames[count++] = reinterpret_cast<void*>(context.Rip);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function)
        {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
    }
    HOST_LOG(L"%s; stack:", why);
    auto base = reinterpret_cast<uint8_t*>(g_process.base);
    for (USHORT i = from ? 0 : 1; i < count; ++i) // 0 is this function, unless the walk starts at a fault
    {
        auto at = static_cast<uint8_t*>(frames[i]);
        if (InGameImage(at))
        {
            HOST_LOG(L"  game+0x%llx", static_cast<unsigned long long>(at - base));
            continue;
        }
        if (HMODULE dll = GameDllContaining(at))
        {
            HOST_LOG(L"  %s+0x%llx", GameDllPath(dll).filename().c_str(),
                static_cast<unsigned long long>(at - reinterpret_cast<uint8_t*>(dll)));
            continue;
        }
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(at), &module))
            GetModuleFileNameW(module, name, MAX_PATH);
        HOST_LOG(L"  %s+0x%llx", std::filesystem::path(name).filename().c_str(),
            static_cast<unsigned long long>(module ? at - reinterpret_cast<uint8_t*>(module) : reinterpret_cast<uintptr_t>(at)));
    }
}

namespace
{
    // Unwinds from a context into frames, without allocating or locking (the thread may be suspended).
    USHORT WalkFrames(CONTEXT context, void** frames, USHORT max)
    {
        USHORT count = 0;
        while (count < max && context.Rip)
        {
            frames[count++] = reinterpret_cast<void*>(context.Rip);
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function)
            {
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        }
        return count;
    }

    void LogFrames(void* const* frames, USHORT count)
    {
        auto base = reinterpret_cast<uint8_t*>(g_process.base);
        for (USHORT i = 0; i < count; ++i)
        {
            auto at = static_cast<uint8_t*>(frames[i]);
            if (InGameImage(at))
            {
                HOST_LOG(L"  game+0x%llx", static_cast<unsigned long long>(at - base));
                continue;
            }
            if (HMODULE dll = GameDllContaining(at))
            {
                HOST_LOG(L"  %s+0x%llx", GameDllPath(dll).filename().c_str(),
                    static_cast<unsigned long long>(at - reinterpret_cast<uint8_t*>(dll)));
                continue;
            }
            HMODULE module = nullptr;
            wchar_t name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(at), &module))
                GetModuleFileNameW(module, name, MAX_PATH);
            HOST_LOG(L"  %s+0x%llx", std::filesystem::path(name).filename().c_str(),
                static_cast<unsigned long long>(module ? at - reinterpret_cast<uint8_t*>(module) : reinterpret_cast<uintptr_t>(at)));
        }
    }
}

namespace
{
    std::atomic<DWORD> g_hostUiThread{ 0 };
}

void NoteHostUiThread() { g_hostUiThread = GetCurrentThreadId(); }

void LogGameThreadStacks(const wchar_t* why)
{
    // The game's main thread and the host's UI thread; the game's workers are usually idle.
    std::vector<HANDLE> threads;
    constexpr DWORD kAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    HANDLE main = OpenThread(kAccess, FALSE, g_process.mainThread);
    if (main) threads.push_back(main);
    HANDLE ui = g_hostUiThread ? OpenThread(kAccess, FALSE, g_hostUiThread) : nullptr;
    if (ui) threads.push_back(ui);
    HOST_LOG(L"%s", why);
    for (HANDLE t : threads)
    {
        if (WaitForSingleObject(t, 0) == WAIT_OBJECT_0 || GetThreadId(t) == GetCurrentThreadId())
            continue;
        void* frames[32];
        USHORT count = 0;
        if (SuspendThread(t) != static_cast<DWORD>(-1))
        {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t, &c))
                count = WalkFrames(c, frames, 32);
            ResumeThread(t);
        }
        HOST_LOG(L"thread %lu%s:", GetThreadId(t), t == main ? L" (game main)" : L" (host UI)");
        LogFrames(frames, count);
    }
    if (main) CloseHandle(main);
    if (ui) CloseHandle(ui);
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
namespace
{
    // Stand-ins for stdout and stderr where the process has none (a UWP app). Small values no real
    // handle has; what the game writes to them goes to host.log a line at a time.
    HANDLE const kStdOut = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00040011));
    HANDLE const kStdErr = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00040021));
    std::mutex g_stdLock;
    std::string g_stdLine[2];

    bool IsStdStandIn(HANDLE h) { return h == kStdOut || h == kStdErr; }

    HANDLE WINAPI Shim_GetStdHandle(DWORD which)
    {
        HANDLE h = GetStdHandle(which);
        if (h && h != INVALID_HANDLE_VALUE)
            return h;
        if (which == STD_OUTPUT_HANDLE) return kStdOut;
        if (which == STD_ERROR_HANDLE) return kStdErr;
        return h;
    }

    BOOL WINAPI Shim_WriteFile(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
    {
        if (!IsStdStandIn(file))
            return WriteFile(file, data, size, written, overlapped);
        std::lock_guard lock(g_stdLock);
        std::string& line = g_stdLine[file == kStdErr];
        for (DWORD i = 0; i < size; ++i)
        {
            char c = static_cast<const char*>(data)[i];
            if (c == '\n' || line.size() > 4000)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                HOST_LOG(L"%s: %s", file == kStdErr ? L"stderr" : L"stdout", Widen(line.c_str()).c_str());
                line.clear();
            }
            if (c != '\n')
                line += c;
        }
        if (written) *written = size;
        return TRUE;
    }

    DWORD WINAPI Shim_GetFileType(HANDLE file) { return IsStdStandIn(file) ? FILE_TYPE_PIPE : GetFileType(file); }
    BOOL WINAPI Shim_FlushFileBuffers(HANDLE file) { return IsStdStandIn(file) ? TRUE : FlushFileBuffers(file); }
}

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
    layer.Register("kernel32!RaiseException", reinterpret_cast<void*>(&Shim_RaiseException));
    layer.Register("kernel32!GetModuleHandleExA", reinterpret_cast<void*>(&Shim_GetModuleHandleExA));
    layer.Register("kernel32!K32GetModuleInformation", reinterpret_cast<void*>(&Shim_K32GetModuleInformation));

    layer.Register("kernel32!CloseHandle", reinterpret_cast<void*>(&Shim_CloseHandle));
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
    layer.Register("kernel32!GetStdHandle", reinterpret_cast<void*>(&Shim_GetStdHandle));
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
    layer.Register("kernel32!WriteFile", reinterpret_cast<void*>(&Shim_WriteFile));

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
    layer.Register("kernel32!FlushFileBuffers", reinterpret_cast<void*>(&Shim_FlushFileBuffers));
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
    layer.Register("kernel32!GetFileType", reinterpret_cast<void*>(&Shim_GetFileType));
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
    PASS("kernel32", ReleaseSRWLockExclusive);
    PASS("kernel32", ReleaseSRWLockShared);
    PASS("kernel32", ReleaseSemaphore);
    PASS("kernel32", ResetEvent);
    PASS("kernel32", RtlCaptureContext);
    PASS("kernel32", RtlLookupFunctionEntry);
    PASS("kernel32", RtlUnwind);
    PASS("kernel32", RtlUnwindEx);
    PASS("kernel32", RtlVirtualUnwind);
    layer.Register("kernel32!VirtualFree", reinterpret_cast<void*>(&Shim_VirtualFree));
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
    PASS("kernel32", WaitForMultipleObjectsEx);
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
