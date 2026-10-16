#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"

namespace fs = std::filesystem;

namespace
{
    std::mutex g_lock;
    fs::path g_root; // the game's folder in LocalState; the game can't see outside it
    fs::path g_cwd;  // the game's current directory, always under g_root or its host mirror

    bool StartsWithNoCase(std::wstring const& s, std::wstring const& prefix)
    {
        return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    bool IsUnderRoot(fs::path const& p)
    {
        std::wstring s = p.wstring(), root = g_root.wstring();
        return StartsWithNoCase(s, root) && (s.size() == root.size() || s[root.size()] == L'\\');
    }
}

// Maps the path a game uses to a real one. Paths inside the game folder are
// used as they are; any other absolute path, like C:\Windows\x, lands in
// <game>\_host\C\Windows\x so the game can't reach (or need) the real system.
fs::path MapGamePath(const wchar_t* name)
{
    std::lock_guard lock(g_lock);
    std::wstring s = name ? name : L"";
    if (s.rfind(L"\\\\?\\", 0) == 0) s.erase(0, 4);
    std::replace(s.begin(), s.end(), L'/', L'\\');

    fs::path p(s), result;
    if (p.has_root_name() && p.has_root_directory())
    {
        result = p;
        if (!IsUnderRoot(result.lexically_normal()))
        {
            std::wstring drive(1, p.root_name().wstring()[0]);
            result = g_root / L"_host" / drive / p.relative_path();
        }
    }
    else if (p.has_root_directory())
        result = g_root / L"_host" / L"C" / p.relative_path();
    else
        result = g_cwd / p;
    return result.lexically_normal();
}

void SetGameRoot(fs::path const& root)
{
    std::lock_guard lock(g_lock);
    g_root = root.lexically_normal();
    g_cwd = g_root;
}

namespace
{
    HANDLE WINAPI Shim_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD disposition, DWORD flags, HANDLE)
    {
        fs::path real = MapGamePath(name);
        CREATEFILE2_EXTENDED_PARAMETERS ext{};
        ext.dwSize = sizeof(ext);
        ext.dwFileAttributes = flags & 0x0000FFFF;
        ext.dwFileFlags = flags & 0xFFF00000;
        ext.dwSecurityQosFlags = flags & 0x000F0000;
        ext.lpSecurityAttributes = security;

        HANDLE h = CreateFile2FromAppW(real.c_str(), access, share, disposition, &ext);
        DWORD error = GetLastError();
        if (h == INVALID_HANDLE_VALUE)
            HOST_LOG(L"open %s -> %s : error %lu", name ? name : L"(null)", real.c_str(), error);
        else
            HOST_LOG(L"open %s -> %s : ok", name ? name : L"(null)", real.c_str());
        SetLastError(error);
        return h;
    }

    HANDLE WINAPI Shim_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD disposition, DWORD flags, HANDLE templateFile)
    {
        return Shim_CreateFileW(Widen(name).c_str(), access, share, security, disposition, flags, templateFile);
    }

    // GetFileSize isn't in the app partition; GetFileSizeEx is.
    DWORD WINAPI Shim_GetFileSize(HANDLE file, LPDWORD high)
    {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size))
            return INVALID_FILE_SIZE;
        if (high) *high = static_cast<DWORD>(size.HighPart);
        SetLastError(ERROR_SUCCESS);
        return size.LowPart;
    }
}

void RegisterFiles(Win32Layer& layer)
{
    layer.Register("kernel32!CreateFileA", reinterpret_cast<void*>(&Shim_CreateFileA));
    layer.Register("kernel32!CreateFileW", reinterpret_cast<void*>(&Shim_CreateFileW));
    layer.Register("kernel32!GetFileSize", reinterpret_cast<void*>(&Shim_GetFileSize));
}
