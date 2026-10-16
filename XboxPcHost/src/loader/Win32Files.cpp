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

    void ToFindDataA(WIN32_FIND_DATAW const& w, WIN32_FIND_DATAA& a)
    {
        a.dwFileAttributes = w.dwFileAttributes;
        a.ftCreationTime = w.ftCreationTime;
        a.ftLastAccessTime = w.ftLastAccessTime;
        a.ftLastWriteTime = w.ftLastWriteTime;
        a.nFileSizeHigh = w.nFileSizeHigh;
        a.nFileSizeLow = w.nFileSizeLow;
        a.dwReserved0 = w.dwReserved0;
        a.dwReserved1 = w.dwReserved1;
        strcpy_s(a.cFileName, Narrow(w.cFileName).c_str());
        strcpy_s(a.cAlternateFileName, Narrow(w.cAlternateFileName).c_str());
    }

    HANDLE WINAPI Shim_FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data)
    {
        return FindFirstFileExFromAppW(MapGamePath(name).c_str(), FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0);
    }

    HANDLE WINAPI Shim_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data)
    {
        WIN32_FIND_DATAW w{};
        HANDLE h = Shim_FindFirstFileW(Widen(name).c_str(), &w);
        if (h != INVALID_HANDLE_VALUE)
            ToFindDataA(w, *data);
        return h;
    }

    BOOL WINAPI Shim_FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data)
    {
        WIN32_FIND_DATAW w{};
        if (!FindNextFileW(find, &w))
            return FALSE;
        ToFindDataA(w, *data);
        return TRUE;
    }

    BOOL WINAPI Shim_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security)
    {
        return CreateDirectoryFromAppW(MapGamePath(name).c_str(), security);
    }

    BOOL WINAPI Shim_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES security)
    {
        return Shim_CreateDirectoryW(Widen(name).c_str(), security);
    }

    BOOL WINAPI Shim_DeleteFileW(LPCWSTR name)
    {
        return DeleteFileFromAppW(MapGamePath(name).c_str());
    }

    BOOL WINAPI Shim_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
    {
        fs::path target = MapGamePath(to);
        // MoveFileFromApp has no flags, so replacing means deleting the target first.
        WIN32_FILE_ATTRIBUTE_DATA existing{};
        if ((flags & MOVEFILE_REPLACE_EXISTING) && GetFileAttributesExFromAppW(target.c_str(), GetFileExInfoStandard, &existing))
            DeleteFileFromAppW(target.c_str());
        return MoveFileFromAppW(MapGamePath(from).c_str(), target.c_str());
    }

    DWORD WINAPI Shim_GetFileAttributesW(LPCWSTR name)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExFromAppW(MapGamePath(name).c_str(), GetFileExInfoStandard, &data))
            return INVALID_FILE_ATTRIBUTES;
        return data.dwFileAttributes;
    }

    DWORD WINAPI Shim_GetFileAttributesA(LPCSTR name)
    {
        return Shim_GetFileAttributesW(Widen(name).c_str());
    }

    DWORD WINAPI Shim_GetCurrentDirectoryA(DWORD size, LPSTR buffer)
    {
        std::string cwd;
        {
            std::lock_guard lock(g_lock);
            cwd = Narrow(g_cwd.wstring());
        }
        // Like the real call: when it doesn't fit, return the size needed including the terminator.
        if (cwd.size() + 1 > size)
            return static_cast<DWORD>(cwd.size() + 1);
        memcpy(buffer, cwd.c_str(), cwd.size() + 1);
        return static_cast<DWORD>(cwd.size());
    }

    BOOL WINAPI Shim_SetCurrentDirectoryW(LPCWSTR name)
    {
        fs::path real = MapGamePath(name);
        DWORD attributes = Shim_GetFileAttributesW(name);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            SetLastError(ERROR_DIRECTORY);
            return FALSE;
        }
        std::lock_guard lock(g_lock);
        g_cwd = real;
        return TRUE;
    }
}

void RegisterFiles(Win32Layer& layer)
{
#define SHIM(fn) layer.Register("kernel32!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    SHIM(CreateFileA);
    SHIM(CreateFileW);
    SHIM(GetFileSize);
    SHIM(FindFirstFileA);
    SHIM(FindFirstFileW);
    SHIM(FindNextFileA);
    SHIM(CreateDirectoryA);
    SHIM(CreateDirectoryW);
    SHIM(DeleteFileW);
    SHIM(MoveFileExW);
    SHIM(GetFileAttributesA);
    SHIM(GetFileAttributesW);
    SHIM(GetCurrentDirectoryA);
    SHIM(SetCurrentDirectoryW);
#undef SHIM
    // Handles from FindFirstFileExFromApp work with the real FindNextFileW.
    layer.Register("kernel32!FindNextFileW", reinterpret_cast<void*>(&FindNextFileW));
}
