#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"
#include <unordered_set>

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

    // Games open the same files over and over (OpenTTD searches every data folder for its music
    // each time it rescans); each distinct result is logged once. File scope, not function statics:
    // those bring the CRT's thread_local init guard into the host's TLS block (see Win32Tls.cpp).
    std::mutex g_openLogLock;
    std::unordered_set<std::wstring> g_openLogged;

    void LogOpen(std::wstring line)
    {
        {
            std::lock_guard guard(g_openLogLock);
            if (!g_openLogged.insert(line).second)
                return;
        }
        HOST_LOG(L"%s", line.c_str());
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
    // An empty path names nothing: Windows fails it (ERROR_PATH_NOT_FOUND), so the real call has to
    // see it empty too. Made relative to the current directory it would "exist", and a game that
    // probes an optional, unset folder with exists("") then lists it (Factorio's mod manager).
    if (s.empty())
        return {};
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

void SetGameRoot(fs::path const& root, fs::path const& currentDirectory)
{
    std::lock_guard lock(g_lock);
    g_root = root.lexically_normal();
    g_cwd = currentDirectory.lexically_normal(); // Windows starts a game with its exe's folder current
}

namespace
{
    HANDLE WINAPI Shim_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
        DWORD disposition, DWORD flags, HANDLE)
    {
        if (!name || !*name)
            LogGameStack(L"CreateFile with an empty name");
        fs::path real = MapGamePath(name);
        CREATEFILE2_EXTENDED_PARAMETERS ext{};
        ext.dwSize = sizeof(ext);
        ext.dwFileAttributes = flags & 0x0000FFFF;
        ext.dwFileFlags = flags & 0xFFF00000;
        ext.dwSecurityQosFlags = flags & 0x000F0000;
        ext.lpSecurityAttributes = security;

        HANDLE h = CreateFile2FromAppW(real.c_str(), access, share, disposition, &ext);
        DWORD error = GetLastError();
        std::wstring line = L"open " + std::wstring(name ? name : L"(null)") + L" -> " + real.wstring();
        LogOpen(h == INVALID_HANDLE_VALUE ? line + L" : error " + std::to_wstring(error) : line + L" : ok");
        SetLastError(error);
        return h;
    }

    // The console's sandbox refuses VOLUME_NAME_DOS (it asks the mount manager), and the STL's
    // canonical() needs it. Every file the game opens is on the game folder's drive, so the path
    // without a volume plus that drive is the same answer.
    DWORD WINAPI Shim_GetFinalPathNameByHandleW(HANDLE file, LPWSTR out, DWORD size, DWORD flags)
    {
        DWORD n = GetFinalPathNameByHandleW(file, out, size, flags);
        if (n != 0 || GetLastError() != ERROR_ACCESS_DENIED || (flags & 0x3) != 0 /* VOLUME_NAME_DOS */)
            return n;
        std::wstring prefix;
        {
            std::lock_guard lock(g_lock);
            prefix = L"\\\\?\\" + g_root.root_name().wstring(); // \\?\Q:
        }
        DWORD need = GetFinalPathNameByHandleW(file, nullptr, 0, (flags & ~0x3u) | 0x4 /* VOLUME_NAME_NONE */);
        if (need == 0)
            return 0;
        std::wstring tail(need, L'\0');
        DWORD got = GetFinalPathNameByHandleW(file, tail.data(), need, (flags & ~0x3u) | 0x4);
        if (got == 0 || got >= need)
            return 0;
        tail.resize(got);
        std::wstring full = prefix + tail;
        if (!out || size <= full.size())
            return static_cast<DWORD>(full.size() + 1); // the size needed, with the terminator
        memcpy(out, full.c_str(), (full.size() + 1) * sizeof(wchar_t));
        return static_cast<DWORD>(full.size());
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

    // The real FindNextFileW (FindFirstFileExFromApp handles work with it); anything but the normal
    // end of a listing is logged.
    BOOL WINAPI Shim_FindNextFileW(HANDLE find, LPWIN32_FIND_DATAW data)
    {
        BOOL ok = FindNextFileW(find, data);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_NO_MORE_FILES)
            LogOpen(L"find next : error " + std::to_wstring(error));
        SetLastError(error);
        return ok;
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

    DWORD WINAPI Shim_GetCurrentDirectoryW(DWORD size, LPWSTR buffer)
    {
        std::wstring cwd;
        {
            std::lock_guard lock(g_lock);
            cwd = g_cwd.wstring();
        }
        if (cwd.size() + 1 > size)
            return static_cast<DWORD>(cwd.size() + 1);
        memcpy(buffer, cwd.c_str(), (cwd.size() + 1) * sizeof(wchar_t));
        return static_cast<DWORD>(cwd.size());
    }

    // The path the game would see, made absolute against its current directory. Not mapped:
    // a path outside the game folder comes back as the game wrote it, as on Windows.
    DWORD WINAPI Shim_GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buffer, LPWSTR* filePart);

    // The W form, then narrowed; filePart points into the caller's buffer as Windows does.
    DWORD WINAPI Shim_GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buffer, LPSTR* filePart)
    {
        wchar_t wide[MAX_PATH * 4];
        DWORD n = Shim_GetFullPathNameW(name ? Widen(name).c_str() : nullptr, MAX_PATH * 4, wide, nullptr);
        if (n == 0 || n >= MAX_PATH * 4)
            return 0;
        std::string full = Narrow(wide);
        if (!buffer || size <= full.size())
            return static_cast<DWORD>(full.size() + 1);
        memcpy(buffer, full.c_str(), full.size() + 1);
        if (filePart)
        {
            size_t slash = full.find_last_of("\\/");
            *filePart = slash == std::string::npos || slash + 1 == full.size() ? nullptr : buffer + slash + 1;
        }
        return static_cast<DWORD>(full.size());
    }

    DWORD WINAPI Shim_GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buffer, LPWSTR* filePart)
    {
        if (!name || !*name)
        {
            SetLastError(ERROR_INVALID_NAME);
            return 0;
        }
        std::wstring s = name;
        std::replace(s.begin(), s.end(), L'/', L'\\');
        fs::path p(s);
        if (!(p.has_root_name() && p.has_root_directory()))
        {
            std::lock_guard lock(g_lock);
            p = p.has_root_directory() ? g_cwd.root_name() / p : g_cwd / p;
        }
        std::wstring full = p.lexically_normal().wstring();
        if (full.size() + 1 > size)
            return static_cast<DWORD>(full.size() + 1);
        memcpy(buffer, full.c_str(), (full.size() + 1) * sizeof(wchar_t));
        if (filePart)
        {
            size_t slash = full.find_last_of(L'\\');
            *filePart = slash == std::wstring::npos || slash + 1 == full.size() ? nullptr : buffer + slash + 1;
        }
        return static_cast<DWORD>(full.size());
    }

    BOOL WINAPI Shim_GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID data)
    {
        return GetFileAttributesExFromAppW(MapGamePath(name).c_str(), level, data);
    }

    HANDLE WINAPI Shim_FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op,
        LPVOID filter, DWORD flags)
    {
        fs::path real = MapGamePath(name);
        HANDLE h = FindFirstFileExFromAppW(real.c_str(), level, data, op, filter, flags);
        DWORD error = GetLastError();
        std::wstring line = L"find " + std::wstring(name ? name : L"(null)") + L" -> " + real.wstring();
        LogOpen(h == INVALID_HANDLE_VALUE ? line + L" : error " + std::to_wstring(error) : line + L" : ok");
        SetLastError(error);
        return h;
    }

    // GetFileInformationByName (Windows 11 24H2) as the SDK declares it for that version.
    enum : int { kStatByName = 0, kStatBasicByName = 3 };
    struct StatInfo // FILE_STAT_INFORMATION
    {
        LARGE_INTEGER fileId, creationTime, lastAccessTime, lastWriteTime, changeTime, allocationSize, endOfFile;
        DWORD fileAttributes, reparseTag, numberOfLinks, effectiveAccess;
    };
    struct StatBasicInfo // FILE_STAT_BASIC_INFORMATION
    {
        LARGE_INTEGER fileId, creationTime, lastAccessTime, lastWriteTime, changeTime, allocationSize, endOfFile;
        DWORD fileAttributes, reparseTag, numberOfLinks, deviceType, deviceCharacteristics, reserved;
        LARGE_INTEGER volumeSerialNumber;
        FILE_ID_128 fileId128;
    };

    // msvcrt's stat (the one in Windows 11 24H2) asks for this first and only falls back to its old
    // FindFirstFile way, which can't stat "." or a directory named with a trailing slash, when it's
    // missing. Answered from a handle to the file.
    BOOL WINAPI Shim_GetFileInformationByName(PCWSTR name, int infoClass, PVOID buffer, ULONG size)
    {
        bool basic = infoClass == kStatBasicByName;
        if ((infoClass != kStatByName && !basic) || size < (basic ? sizeof(StatBasicInfo) : sizeof(StatInfo)))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        HANDLE h = Shim_CreateFileW(name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return FALSE;
        FILE_BASIC_INFO times{};
        FILE_STANDARD_INFO standard{};
        FILE_ID_INFO id{};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        BOOL ok = GetFileInformationByHandleEx(h, FileBasicInfo, &times, sizeof(times)) &&
            GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof(standard));
        DWORD error = GetLastError();
        GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)); // optional: not every file system has ids
        GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag));
        CloseHandle(h);
        if (!ok)
        {
            SetLastError(error);
            return FALSE;
        }
        StatBasicInfo info{};
        memcpy(&info.fileId, id.FileId.Identifier, sizeof(info.fileId));
        info.creationTime = times.CreationTime;
        info.lastAccessTime = times.LastAccessTime;
        info.lastWriteTime = times.LastWriteTime;
        info.changeTime = times.ChangeTime;
        info.allocationSize = standard.AllocationSize;
        info.endOfFile = standard.EndOfFile;
        info.fileAttributes = times.FileAttributes;
        info.reparseTag = tag.ReparseTag;
        info.numberOfLinks = standard.NumberOfLinks;
        if (basic)
        {
            info.deviceType = 0x07; // FILE_DEVICE_DISK (winioctl.h)
            info.volumeSerialNumber.QuadPart = static_cast<LONGLONG>(id.VolumeSerialNumber);
            info.fileId128 = id.FileId;
            memcpy(buffer, &info, sizeof(StatBasicInfo));
        }
        else
        {
            StatInfo stat{};
            memcpy(&stat, &info, offsetof(StatInfo, effectiveAccess));
            stat.effectiveAccess = FILE_ALL_ACCESS;
            memcpy(buffer, &stat, sizeof(stat));
        }
        return TRUE;
    }

    BOOL WINAPI Shim_CreateDirectoryExW(LPCWSTR, LPCWSTR name, LPSECURITY_ATTRIBUTES security)
    {
        return Shim_CreateDirectoryW(name, security);
    }

    BOOL WINAPI Shim_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
    {
        return CopyFileFromAppW(MapGamePath(from).c_str(), MapGamePath(to).c_str(), failIfExists);
    }

    BOOL WINAPI Shim_SetFileAttributesW(LPCWSTR name, DWORD attributes)
    {
        return SetFileAttributesFromAppW(MapGamePath(name).c_str(), attributes);
    }

    // Free space of the drive the game folder is on, whatever path was asked about.
    BOOL WINAPI Shim_GetDiskFreeSpaceExW(LPCWSTR, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free)
    {
        fs::path root;
        {
            std::lock_guard lock(g_lock);
            root = g_root;
        }
        return GetDiskFreeSpaceExW(root.c_str(), available, total, free);
    }

    // A temp folder inside the game folder, made on first use.
    DWORD WINAPI Shim_GetTempPathW(DWORD size, LPWSTR buffer)
    {
        fs::path temp;
        {
            std::lock_guard lock(g_lock);
            temp = g_root / L"_temp";
        }
        CreateDirectoryFromAppW(temp.c_str(), nullptr);
        std::wstring s = temp.wstring() + L"\\";
        if (s.size() + 1 > size)
            return static_cast<DWORD>(s.size() + 1);
        memcpy(buffer, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        return static_cast<DWORD>(s.size());
    }

    DWORD WINAPI Shim_GetLogicalDriveStringsW(DWORD size, LPWSTR buffer)
    {
        static const wchar_t drives[] = L"C:\\\0"; // "C:\" then the list's final terminator
        constexpr DWORD length = static_cast<DWORD>(_countof(drives) - 1);
        if (size < length + 1)
            return length + 1;
        memcpy(buffer, drives, sizeof(drives));
        return length;
    }

    BOOL WINAPI Shim_CreateLinkW(LPCWSTR, LPCWSTR, LPVOID)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    // Shell folders live in <game>\_user, made on request (CSIDL_FLAG_CREATE) or on first use.
    HRESULT WINAPI Shim_SHGetFolderPathW(HWND, int csidl, HANDLE, DWORD, LPWSTR path)
    {
        const wchar_t* sub = nullptr;
        switch (csidl & 0xFF)
        {
        case 0x05: case 0x2E: sub = L"Documents"; break;             // CSIDL_PERSONAL, CSIDL_COMMON_DOCUMENTS
        case 0x1A: case 0x23: sub = L"AppData\\Roaming"; break;      // CSIDL_APPDATA, CSIDL_COMMON_APPDATA
        case 0x1C: sub = L"AppData\\Local"; break;                   // CSIDL_LOCAL_APPDATA
        case 0x10: sub = L"Desktop"; break;                          // CSIDL_DESKTOPDIRECTORY
        case 0x14: sub = L"Fonts"; break;                            // CSIDL_FONTS
        case 0x28: sub = L""; break;                                 // CSIDL_PROFILE
        default: return E_INVALIDARG;
        }
        fs::path dir;
        {
            std::lock_guard lock(g_lock);
            dir = (g_root / L"_user" / sub).lexically_normal();
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::wstring s = dir.wstring();
        if (!s.empty() && s.back() == L'\\') s.pop_back();
        if (s.size() >= MAX_PATH)
            return E_FAIL;
        wcscpy_s(path, MAX_PATH, s.c_str());
        return S_OK;
    }

    HRESULT WINAPI Shim_SHGetFolderPathA(HWND window, int csidl, HANDLE token, DWORD flags, LPSTR path)
    {
        wchar_t wide[MAX_PATH];
        HRESULT hr = Shim_SHGetFolderPathW(window, csidl, token, flags, wide);
        if (SUCCEEDED(hr))
            strcpy_s(path, MAX_PATH, Narrow(wide).c_str());
        return hr;
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
    SHIM(GetFinalPathNameByHandleW);
    SHIM(GetFileInformationByName);
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
    SHIM(GetCurrentDirectoryW);
    SHIM(GetFullPathNameW);
    SHIM(GetFullPathNameA);
    SHIM(GetFileAttributesExW);
    SHIM(FindFirstFileExW);
    SHIM(CreateDirectoryExW);
    SHIM(CopyFileW);
    SHIM(SetFileAttributesW);
    SHIM(GetDiskFreeSpaceExW);
    SHIM(GetTempPathW);
    SHIM(GetLogicalDriveStringsW);
#undef SHIM
    layer.Register("kernel32!CreateHardLinkW", reinterpret_cast<void*>(&Shim_CreateLinkW));
    layer.Register("kernel32!CreateSymbolicLinkW", reinterpret_cast<void*>(&Shim_CreateLinkW));
    layer.Register("shell32!SHGetFolderPathW", reinterpret_cast<void*>(&Shim_SHGetFolderPathW));
    layer.Register("shell32!SHGetFolderPathA", reinterpret_cast<void*>(&Shim_SHGetFolderPathA));
    layer.Register("kernel32!FindNextFileW", reinterpret_cast<void*>(&Shim_FindNextFileW));
}
