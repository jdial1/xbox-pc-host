#include "pch.h"
#include <bcrypt.h>
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"

// Old desktop calls older games and runtimes still make (Unity 5's static CRT, Mono), which the app
// partition dropped: the lstr* string helpers, GetVersion, GlobalMemoryStatus, A-versions of the date
// and locale functions, CommandLineToArgvW, a few shlwapi path helpers and rectangle helpers, and the
// CryptoAPI's random numbers.
namespace
{
    // ------------------------------------------------------------------ strings
    int WINAPI Shim_lstrlenA(LPCSTR s) { return s ? static_cast<int>(strlen(s)) : 0; }
    LPSTR WINAPI Shim_lstrcpyA(LPSTR to, LPCSTR from)
    {
        if (to && from) memcpy(to, from, strlen(from) + 1);
        return to;
    }
    LPSTR WINAPI Shim_lstrcpynA(LPSTR to, LPCSTR from, int n)
    {
        if (!to || !from || n <= 0) return to;
        strncpy_s(to, static_cast<size_t>(n), from, _TRUNCATE);
        return to;
    }
    LPWSTR WINAPI Shim_lstrcpynW(LPWSTR to, LPCWSTR from, int n)
    {
        if (!to || !from || n <= 0) return to;
        wcsncpy_s(to, static_cast<size_t>(n), from, _TRUNCATE);
        return to;
    }
    int WINAPI Shim_lstrcmpiA(LPCSTR a, LPCSTR b) { return _stricmp(a ? a : "", b ? b : ""); }

    // wsprintf writes at most 1024 characters.
    int WINAPI Shim_wvsprintfA(LPSTR out, LPCSTR format, va_list args) { return vsnprintf(out, 1024, format, args); }
    int WINAPIV Shim_wsprintfA(LPSTR out, LPCSTR format, ...)
    {
        va_list args;
        va_start(args, format);
        int n = vsnprintf(out, 1024, format, args);
        va_end(args);
        return n;
    }

    // ------------------------------------------------------------------ system
    // Windows 10 build 19045, as GetVersionEx reports: build in the high word, minor and major in the low.
    DWORD WINAPI Shim_GetVersion() { return (19045u << 16) | (0u << 8) | 10u; }
    UINT WINAPI Shim_SetHandleCount(UINT n) { return n; }
    BOOL WINAPI Shim_HeapQueryInformation(HANDLE, int, PVOID, SIZE_T, PSIZE_T) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }

    struct MemoryStatus // MEMORYSTATUS
    {
        DWORD length, load;
        SIZE_T totalPhys, availPhys, totalPageFile, availPageFile, totalVirtual, availVirtual;
    };
    void WINAPI Shim_GlobalMemoryStatus(MemoryStatus* out)
    {
        MEMORYSTATUSEX ex{ sizeof(ex) };
        GlobalMemoryStatusEx(&ex);
        *out = { sizeof(MemoryStatus), ex.dwMemoryLoad, static_cast<SIZE_T>(ex.ullTotalPhys), static_cast<SIZE_T>(ex.ullAvailPhys),
            static_cast<SIZE_T>(ex.ullTotalPageFile), static_cast<SIZE_T>(ex.ullAvailPageFile), static_cast<SIZE_T>(ex.ullTotalVirtual),
            static_cast<SIZE_T>(ex.ullAvailVirtual) };
    }

    BOOL WINAPI Shim_FileTimeToDosDateTime(const FILETIME* ft, LPWORD date, LPWORD time)
    {
        SYSTEMTIME st;
        if (!ft || !date || !time || !FileTimeToSystemTime(ft, &st) || st.wYear < 1980 || st.wYear > 2107)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        *date = static_cast<WORD>(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
        *time = static_cast<WORD>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
        return TRUE;
    }

    // ------------------------------------------------------------------ locale (the system's en-US)
    template <typename Fn> int FormatA(Fn fn, LPSTR out, int size)
    {
        wchar_t wide[256];
        int n = fn(wide, 256);
        if (n <= 0) return 0;
        std::string s = Narrow(wide);
        if (!out || size == 0) return static_cast<int>(s.size() + 1);
        if (size <= static_cast<int>(s.size())) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(out, s.c_str(), s.size() + 1);
        return static_cast<int>(s.size() + 1);
    }
    int WINAPI Shim_GetDateFormatA(LCID, DWORD flags, const SYSTEMTIME* t, LPCSTR format, LPSTR out, int size)
    {
        std::wstring f = format ? Widen(format) : L"";
        return FormatA([&](wchar_t* w, int n) { return GetDateFormatEx(L"en-US", flags, t, format ? f.c_str() : nullptr, w, n, nullptr); }, out, size);
    }
    int WINAPI Shim_GetTimeFormatA(LCID, DWORD flags, const SYSTEMTIME* t, LPCSTR format, LPSTR out, int size)
    {
        std::wstring f = format ? Widen(format) : L"";
        return FormatA([&](wchar_t* w, int n) { return GetTimeFormatEx(L"en-US", flags, t, format ? f.c_str() : nullptr, w, n); }, out, size);
    }
    using LocaleEnumA = BOOL(CALLBACK*)(LPSTR);
    BOOL WINAPI Shim_EnumSystemLocalesA(LocaleEnumA fn, DWORD)
    {
        char id[] = "00000409";
        if (fn) fn(id);
        return TRUE;
    }

    // ------------------------------------------------------------------ shell
    // The usual rules: whitespace separates, quotes group, 2n backslashes + quote = n backslashes + a
    // quote toggle, 2n+1 backslashes + quote = n backslashes + a literal quote. One LocalAlloc block.
    LPWSTR* WINAPI Shim_CommandLineToArgvW(LPCWSTR line, int* count)
    {
        std::vector<std::wstring> args;
        if (!line || !*line)
            args.push_back(GameExePath().wstring()); // an empty line names the program
        else
        {
            const wchar_t* p = line;
            while (*p)
            {
                while (*p == L' ' || *p == L'\t') ++p;
                if (!*p) break;
                std::wstring arg;
                bool quoted = false;
                for (; *p && (quoted || (*p != L' ' && *p != L'\t')); ++p)
                {
                    size_t slashes = 0;
                    while (*p == L'\\') { ++slashes; ++p; }
                    if (*p == L'"')
                    {
                        arg.append(slashes / 2, L'\\');
                        if (slashes % 2) arg += L'"';
                        else quoted = !quoted;
                    }
                    else
                    {
                        arg.append(slashes, L'\\');
                        if (!*p) break;
                        if (!quoted && (*p == L' ' || *p == L'\t')) break;
                        arg += *p;
                    }
                }
                args.push_back(arg);
            }
        }
        size_t bytes = (args.size() + 1) * sizeof(LPWSTR);
        for (auto const& a : args) bytes += (a.size() + 1) * sizeof(wchar_t);
        auto block = static_cast<uint8_t*>(LocalAlloc(LMEM_FIXED, bytes));
        if (!block) return nullptr;
        auto argv = reinterpret_cast<LPWSTR*>(block);
        auto text = reinterpret_cast<wchar_t*>(block + (args.size() + 1) * sizeof(LPWSTR));
        for (size_t i = 0; i < args.size(); ++i)
        {
            argv[i] = text;
            memcpy(text, args[i].c_str(), (args[i].size() + 1) * sizeof(wchar_t));
            text += args[i].size() + 1;
        }
        argv[args.size()] = nullptr;
        if (count) *count = static_cast<int>(args.size());
        return argv;
    }

    BOOL WINAPI Shim_PathFileExistsW(LPCWSTR path)
    {
        if (!path || !*path) return FALSE;
        std::error_code ec;
        return std::filesystem::exists(MapGamePath(path), ec) ? TRUE : FALSE;
    }
    BOOL WINAPI Shim_PathCanonicalizeW(LPWSTR out, LPCWSTR in)
    {
        if (!out || !in) return FALSE;
        std::wstring s = std::filesystem::path(in).lexically_normal().wstring();
        if (s.size() >= MAX_PATH) return FALSE;
        memcpy(out, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        return TRUE;
    }

    // ------------------------------------------------------------------ user32 odds and ends
    BOOL WINAPI Shim_CopyRect(RECT* to, const RECT* from) { if (!to || !from) return FALSE; *to = *from; return TRUE; }
    BOOL WINAPI Shim_OffsetRect(RECT* r, int dx, int dy)
    {
        if (!r) return FALSE;
        r->left += dx; r->right += dx; r->top += dy; r->bottom += dy;
        return TRUE;
    }
    BOOL WINAPI Shim_IsWindowVisible(HWND) { return TRUE; } // the game's window is the screen
    VOID WINAPI Shim_PostQuitMessage(int code)
    {
        HOST_LOG(L"PostQuitMessage(%d)", code);
        LogGameStack(L"PostQuitMessage from");
        PostGameMessage(0x0012 /* WM_QUIT */, static_cast<WPARAM>(code), 0);
    }

    // ------------------------------------------------------------------ file mappings (Mono maps assemblies)
    // The FromApp forms; they take no execute protection, which a data mapping never needs anyway.
    ULONG DataProtection(DWORD protect)
    {
        DWORD base = protect & 0xFF, flags = protect & ~0xFFu;
        if (base == PAGE_EXECUTE_READ) base = PAGE_READONLY;
        else if (base == PAGE_EXECUTE_READWRITE) base = PAGE_READWRITE;
        else if (base == PAGE_EXECUTE_WRITECOPY) base = PAGE_WRITECOPY;
        return base | flags;
    }
    HANDLE WINAPI Shim_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES security, DWORD protect, DWORD high, DWORD low, LPCWSTR name)
    {
        return CreateFileMappingFromApp(file, security, DataProtection(protect), (static_cast<ULONG64>(high) << 32) | low, name);
    }
    HANDLE WINAPI Shim_CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES security, DWORD protect, DWORD high, DWORD low, LPCSTR name)
    {
        std::wstring w = name ? Widen(name) : L"";
        return Shim_CreateFileMappingW(file, security, protect, high, low, name ? w.c_str() : nullptr);
    }
    LPVOID WINAPI Shim_MapViewOfFile(HANDLE mapping, DWORD access, DWORD high, DWORD low, SIZE_T size)
    {
        return MapViewOfFileFromApp(mapping, access & ~static_cast<DWORD>(FILE_MAP_EXECUTE), (static_cast<ULONG64>(high) << 32) | low, size);
    }
    // A requested address can't be honoured through the FromApp form; anywhere will do for a data view.
    LPVOID WINAPI Shim_MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD high, DWORD low, SIZE_T size, LPVOID)
    {
        return Shim_MapViewOfFile(mapping, access, high, low, size);
    }

    // ------------------------------------------------------------------ CryptoAPI randomness (Mono, CRTs)
    ULONG_PTR const kProvider = 0x00050010; // an HCRYPTPROV
    BOOL WINAPI Shim_CryptAcquireContext(ULONG_PTR* out, LPCVOID, LPCVOID, DWORD, DWORD)
    {
        if (!out) return FALSE;
        *out = kProvider;
        return TRUE;
    }
    BOOL WINAPI Shim_CryptGenRandom(ULONG_PTR, DWORD size, BYTE* out)
    {
        return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? TRUE : FALSE;
    }
    BOOL WINAPI Shim_CryptReleaseContext(ULONG_PTR, DWORD) { return TRUE; }
    // RtlGenRandom, behind msvcrt's rand_s (std::random_device in MinGW builds).
    BOOLEAN WINAPI Shim_SystemFunction036(PVOID out, ULONG size)
    {
        return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), size, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    // Desktop only; the installed memory is the total physical memory here.
    BOOL WINAPI Shim_GetPhysicallyInstalledSystemMemory(PULONGLONG kilobytes)
    {
        MEMORYSTATUSEX status{ sizeof(status) };
        if (!kilobytes || !GlobalMemoryStatusEx(&status))
            return FALSE;
        *kilobytes = status.ullTotalPhys / 1024;
        return TRUE;
    }
}

void RegisterLegacy(Win32Layer& layer)
{
#define REG(key, fn) layer.Register(key, reinterpret_cast<void*>(&fn))
    REG("kernel32!lstrlenA", Shim_lstrlenA);
    REG("kernel32!lstrcpyA", Shim_lstrcpyA);
    REG("kernel32!lstrcpynA", Shim_lstrcpynA);
    REG("kernel32!lstrcpynW", Shim_lstrcpynW);
    REG("kernel32!lstrcmpiA", Shim_lstrcmpiA);
    REG("user32!wvsprintfA", Shim_wvsprintfA);
    REG("user32!wsprintfA", Shim_wsprintfA);
    REG("kernel32!GetVersion", Shim_GetVersion);
    REG("kernel32!SetHandleCount", Shim_SetHandleCount);
    REG("kernel32!HeapQueryInformation", Shim_HeapQueryInformation);
    REG("kernel32!GlobalMemoryStatus", Shim_GlobalMemoryStatus);
    REG("kernel32!FileTimeToDosDateTime", Shim_FileTimeToDosDateTime);
    REG("kernel32!GetDateFormatA", Shim_GetDateFormatA);
    REG("kernel32!GetTimeFormatA", Shim_GetTimeFormatA);
    REG("kernel32!EnumSystemLocalesA", Shim_EnumSystemLocalesA);
    REG("shell32!CommandLineToArgvW", Shim_CommandLineToArgvW);
    REG("shlwapi!PathFileExistsW", Shim_PathFileExistsW);
    REG("shlwapi!PathCanonicalizeW", Shim_PathCanonicalizeW);
    REG("user32!CopyRect", Shim_CopyRect);
    REG("user32!OffsetRect", Shim_OffsetRect);
    REG("user32!IsWindowVisible", Shim_IsWindowVisible);
    REG("user32!PostQuitMessage", Shim_PostQuitMessage);
    REG("advapi32!CryptAcquireContextA", Shim_CryptAcquireContext);
    REG("advapi32!CryptAcquireContextW", Shim_CryptAcquireContext);
    REG("advapi32!CryptGenRandom", Shim_CryptGenRandom);
    REG("advapi32!CryptReleaseContext", Shim_CryptReleaseContext);
    REG("advapi32!SystemFunction036", Shim_SystemFunction036);
    REG("cryptbase!SystemFunction036", Shim_SystemFunction036);
    // Recoil sizes its thread pool from these (through GetProcAddress); without them it sees 0 cores.
    REG("kernel32!GetLogicalProcessorInformation", GetLogicalProcessorInformation);
    REG("kernel32!GetLogicalProcessorInformationEx", GetLogicalProcessorInformationEx);
    REG("kernel32!GetPhysicallyInstalledSystemMemory", Shim_GetPhysicallyInstalledSystemMemory);
    // winpthread checks a new thread's handle with GetHandleInformation; a failure there is a failed
    // pthread_create, so std::thread throws. Both these and the completion ports are app APIs.
    REG("kernel32!GetHandleInformation", GetHandleInformation);
    REG("kernel32!SetHandleInformation", SetHandleInformation);
    REG("kernel32!CreateIoCompletionPort", CreateIoCompletionPort);
    REG("kernel32!GetQueuedCompletionStatus", GetQueuedCompletionStatus);
    REG("kernel32!GetQueuedCompletionStatusEx", GetQueuedCompletionStatusEx);
    REG("kernel32!PostQueuedCompletionStatus", PostQueuedCompletionStatus);
    REG("kernel32!CreateFileMappingW", Shim_CreateFileMappingW);
    REG("kernel32!CreateFileMappingA", Shim_CreateFileMappingA);
    REG("kernel32!MapViewOfFile", Shim_MapViewOfFile);
    REG("kernel32!MapViewOfFileEx", Shim_MapViewOfFileEx);
    // JITs (Mono) describe their generated code's unwind data this way; exported to apps as they are.
    for (const char* dll : { "kernel32", "ntdll" })
    {
        layer.Register((std::string(dll) + "!RtlInstallFunctionTableCallback").c_str(), reinterpret_cast<void*>(&RtlInstallFunctionTableCallback));
        layer.Register((std::string(dll) + "!RtlDeleteFunctionTable").c_str(), reinterpret_cast<void*>(&RtlDeleteFunctionTable));
    }
    // The A form passes through already; Mono reads MONO_PATH and friends with the W one.
    layer.Register("kernel32!GetEnvironmentVariableW", reinterpret_cast<void*>(&GetEnvironmentVariableW));
#undef REG
}
