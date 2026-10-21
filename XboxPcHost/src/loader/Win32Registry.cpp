#include "pch.h"
#include <fstream>
#include "loader/Win32Layer.h"
#include "loader/Win32Text.h"
#include "Log.h"

// HKEY_CURRENT_USER for games, kept by the host: the console's sandbox won't let an app create keys
// there, and Unity stores PlayerPrefs (and so every game's settings) in HKCU\Software\<company>\<game>.
// A failed create left Unity writing to a garbage key, which strict handle checks turn into a crash.
// Keys under HKCU are host objects; their values live in memory and in host-registry.dat in the game's
// folder. Values are stored the W way, so A and W callers see the same strings. Every other root goes
// to the real registry (reads of HKLM work in the app partition).
namespace
{
    struct Value
    {
        DWORD type = REG_NONE;
        std::vector<uint8_t> data;
    };
    struct Key
    {
        std::wstring path; // lowercase, below HKCU, no leading backslash
    };

    std::recursive_mutex g_lock;
    bool g_loaded = false;
    std::map<std::wstring, std::map<std::wstring, Value>> g_values; // key path -> lowercase name -> value
    std::map<std::wstring, std::wstring> g_names;                   // lowercase name -> name as first written, per key path + "\n" + name
    std::set<std::wstring> g_keys;                                   // every key that exists
    std::map<uintptr_t, Key> g_open;                                 // our HKEYs
    uintptr_t g_next = 0x7E000000;

    HKEY const kCurrentUser = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(static_cast<LONG>(0x80000001)));

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    std::filesystem::path StoreFile() { return GameExePath().parent_path() / L"host-registry.dat"; }

    // The store's format: lines "K<path>", then "V<type> <name>" followed by one line of hex bytes.
    void Load()
    {
        if (g_loaded) return;
        g_loaded = true;
        std::wifstream in(StoreFile());
        std::wstring line, key;
        while (std::getline(in, line))
        {
            if (line.size() > 1 && line[0] == L'K')
            {
                key = line.substr(1);
                g_keys.insert(key);
            }
            else if (line.size() > 1 && line[0] == L'V')
            {
                size_t space = line.find(L' ');
                Value v;
                v.type = static_cast<DWORD>(wcstoul(line.c_str() + 1, nullptr, 10));
                std::wstring name = space == std::wstring::npos ? L"" : line.substr(space + 1);
                std::wstring hex;
                std::getline(in, hex);
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                    v.data.push_back(static_cast<uint8_t>(wcstoul(hex.substr(i, 2).c_str(), nullptr, 16)));
                g_values[key][Lower(name)] = v;
                g_names[key + L"\n" + Lower(name)] = name;
            }
        }
    }

    void Save()
    {
        std::wofstream out(StoreFile(), std::ios::trunc);
        static const wchar_t* kHex = L"0123456789abcdef";
        for (auto const& key : g_keys)
        {
            out << L'K' << key << L'\n';
            auto it = g_values.find(key);
            if (it == g_values.end()) continue;
            for (auto const& [lower, v] : it->second)
            {
                auto shown = g_names.find(key + L"\n" + lower);
                out << L'V' << v.type << L' ' << (shown != g_names.end() ? shown->second : lower) << L'\n';
                for (uint8_t b : v.data) out << kHex[b >> 4] << kHex[b & 15];
                out << L'\n';
            }
        }
    }

    // A key we keep: under HKCU, or one of our own handles. Returns its path in path.
    bool Ours(HKEY root, LPCWSTR sub, std::wstring& path)
    {
        std::wstring base;
        if (root == kCurrentUser)
            base.clear();
        else
        {
            auto it = g_open.find(reinterpret_cast<uintptr_t>(root));
            if (it == g_open.end()) return false;
            base = it->second.path;
        }
        std::wstring s = sub ? Lower(sub) : L"";
        while (!s.empty() && s.front() == L'\\') s.erase(0, 1);
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        path = base.empty() ? s : s.empty() ? base : base + L"\\" + s;
        return true;
    }

    HKEY NewHandle(std::wstring const& path)
    {
        uintptr_t h = g_next += 4;
        g_open[h] = { path };
        return reinterpret_cast<HKEY>(h);
    }

    bool IsOurHandle(HKEY h) { return h == kCurrentUser || g_open.count(reinterpret_cast<uintptr_t>(h)); }

    // ------------------------------------------------------------------ W forms
    LSTATUS WINAPI Shim_RegCreateKeyExW(HKEY root, LPCWSTR sub, DWORD, LPWSTR, DWORD, REGSAM sam, LPSECURITY_ATTRIBUTES sa,
        PHKEY out, LPDWORD disposition)
    {
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(root, sub, path))
            return RegCreateKeyExW(root, sub, 0, nullptr, 0, sam, sa, out, disposition);
        if (!out) return ERROR_INVALID_PARAMETER;
        bool existed = g_keys.count(path) != 0;
        // Every parent exists too, as in the real registry.
        for (size_t i = path.find(L'\\'); i != std::wstring::npos; i = path.find(L'\\', i + 1))
            g_keys.insert(path.substr(0, i));
        g_keys.insert(path);
        if (disposition) *disposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
        *out = NewHandle(path);
        if (!existed) Save();
        return ERROR_SUCCESS;
    }
    LSTATUS WINAPI Shim_RegCreateKeyW(HKEY root, LPCWSTR sub, PHKEY out)
    {
        return Shim_RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, out, nullptr);
    }

    LSTATUS WINAPI Shim_RegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD options, REGSAM sam, PHKEY out)
    {
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(root, sub, path))
            return RegOpenKeyExW(root, sub, options, sam, out);
        if (!out) return ERROR_INVALID_PARAMETER;
        if (!path.empty() && !g_keys.count(path)) return ERROR_FILE_NOT_FOUND;
        *out = NewHandle(path);
        return ERROR_SUCCESS;
    }
    LSTATUS WINAPI Shim_RegOpenKeyW(HKEY root, LPCWSTR sub, PHKEY out) { return Shim_RegOpenKeyExW(root, sub, 0, KEY_ALL_ACCESS, out); }

    LSTATUS WINAPI Shim_RegCloseKey(HKEY key)
    {
        std::lock_guard lock(g_lock);
        if (key == kCurrentUser) return ERROR_SUCCESS;
        if (g_open.erase(reinterpret_cast<uintptr_t>(key))) return ERROR_SUCCESS;
        return RegCloseKey(key);
    }

    LSTATUS SetValue(std::wstring const& path, LPCWSTR name, DWORD type, const BYTE* data, DWORD size)
    {
        if (!g_keys.count(path) && !path.empty()) return ERROR_KEY_DELETED;
        Value v;
        v.type = type;
        if (data && size) v.data.assign(data, data + size);
        std::wstring n = name ? name : L"";
        g_values[path][Lower(n)] = std::move(v);
        g_names[path + L"\n" + Lower(n)] = n;
        Save();
        return ERROR_SUCCESS;
    }

    LSTATUS WINAPI Shim_RegSetValueExW(HKEY key, LPCWSTR name, DWORD, DWORD type, const BYTE* data, DWORD size)
    {
        std::lock_guard lock(g_lock);
        std::wstring path;
        if (!Ours(key, nullptr, path))
            return RegSetValueExW(key, name, 0, type, data, size);
        return SetValue(path, name, type, data, size);
    }

    // Copies a value out the way RegQueryValueEx does: size alone, or ERROR_MORE_DATA when it doesn't fit.
    LSTATUS GetValue(std::wstring const& path, LPCWSTR name, LPDWORD type, LPBYTE data, LPDWORD size)
    {
        auto k = g_values.find(path);
        if (k == g_values.end()) return ERROR_FILE_NOT_FOUND;
        auto it = k->second.find(Lower(name ? name : L""));
        if (it == k->second.end()) return ERROR_FILE_NOT_FOUND;
        Value const& v = it->second;
        if (type) *type = v.type;
        DWORD n = static_cast<DWORD>(v.data.size());
        if (!data) { if (size) *size = n; return ERROR_SUCCESS; }
        if (!size) return ERROR_INVALID_PARAMETER;
        if (*size < n) { *size = n; return ERROR_MORE_DATA; }
        memcpy(data, v.data.data(), n);
        *size = n;
        return ERROR_SUCCESS;
    }

    LSTATUS WINAPI Shim_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size)
    {
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(key, nullptr, path))
            return RegQueryValueExW(key, name, nullptr, type, data, size);
        return GetValue(path, name, type, data, size);
    }

    LSTATUS WINAPI Shim_RegDeleteValueW(HKEY key, LPCWSTR name)
    {
        std::lock_guard lock(g_lock);
        std::wstring path;
        if (!Ours(key, nullptr, path)) return ERROR_ACCESS_DENIED;
        auto k = g_values.find(path);
        if (k == g_values.end() || !k->second.erase(Lower(name ? name : L""))) return ERROR_FILE_NOT_FOUND;
        Save();
        return ERROR_SUCCESS;
    }

    LSTATUS WINAPI Shim_RegDeleteKeyW(HKEY root, LPCWSTR sub)
    {
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(root, sub, path)) return ERROR_ACCESS_DENIED;
        if (!g_keys.count(path)) return ERROR_FILE_NOT_FOUND;
        // A key with subkeys can't be deleted this way.
        for (auto const& k : g_keys)
            if (k.size() > path.size() && k.compare(0, path.size(), path) == 0 && k[path.size()] == L'\\')
                return ERROR_ACCESS_DENIED;
        g_keys.erase(path);
        g_values.erase(path);
        Save();
        return ERROR_SUCCESS;
    }

    LSTATUS WINAPI Shim_RegEnumValueW(HKEY key, DWORD index, LPWSTR name, LPDWORD nameSize, LPDWORD, LPDWORD type, LPBYTE data,
        LPDWORD size)
    {
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(key, nullptr, path)) return ERROR_ACCESS_DENIED;
        auto k = g_values.find(path);
        if (k == g_values.end() || index >= k->second.size()) return ERROR_NO_MORE_ITEMS;
        auto it = std::next(k->second.begin(), index);
        auto shown = g_names.find(path + L"\n" + it->first);
        std::wstring n = shown != g_names.end() ? shown->second : it->first;
        if (!name || !nameSize || *nameSize <= n.size()) { if (nameSize) *nameSize = static_cast<DWORD>(n.size()); return ERROR_MORE_DATA; }
        memcpy(name, n.c_str(), (n.size() + 1) * sizeof(wchar_t));
        *nameSize = static_cast<DWORD>(n.size());
        return GetValue(path, n.c_str(), type, data, size);
    }

    LSTATUS WINAPI Shim_RegQueryInfoKeyW(HKEY key, LPWSTR, LPDWORD, LPDWORD, LPDWORD subKeys, LPDWORD maxSubKeyLen, LPDWORD,
        LPDWORD values, LPDWORD maxNameLen, LPDWORD maxValueLen, LPDWORD, PFILETIME written)
    {
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(key, nullptr, path)) return ERROR_ACCESS_DENIED;
        DWORD nKeys = 0, nValues = 0, maxName = 0, maxValue = 0;
        for (auto const& k : g_keys)
            if (k.size() > path.size() && k.compare(0, path.size(), path) == 0 && k.find(L'\\', path.size() + 1) == std::wstring::npos)
                ++nKeys;
        if (auto it = g_values.find(path); it != g_values.end())
            for (auto const& [n, v] : it->second)
            {
                ++nValues;
                maxName = std::max<DWORD>(maxName, static_cast<DWORD>(n.size()));
                maxValue = std::max<DWORD>(maxValue, static_cast<DWORD>(v.data.size()));
            }
        if (subKeys) *subKeys = nKeys;
        if (maxSubKeyLen) *maxSubKeyLen = 255;
        if (values) *values = nValues;
        if (maxNameLen) *maxNameLen = maxName;
        if (maxValueLen) *maxValueLen = maxValue;
        if (written) GetSystemTimeAsFileTime(written);
        return ERROR_SUCCESS;
    }

    LSTATUS WINAPI Shim_RegFlushKey(HKEY) { return ERROR_SUCCESS; }

    // ------------------------------------------------------------------ A forms (names and strings converted)
    bool IsString(DWORD type) { return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ; }

    LSTATUS WINAPI Shim_RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD, LPSTR, DWORD, REGSAM sam, LPSECURITY_ATTRIBUTES sa,
        PHKEY out, LPDWORD disposition)
    {
        std::wstring w = sub ? Widen(sub) : L"";
        return Shim_RegCreateKeyExW(root, sub ? w.c_str() : nullptr, 0, nullptr, 0, sam, sa, out, disposition);
    }
    LSTATUS WINAPI Shim_RegCreateKeyA(HKEY root, LPCSTR sub, PHKEY out)
    {
        return Shim_RegCreateKeyExA(root, sub, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, out, nullptr);
    }
    LSTATUS WINAPI Shim_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD options, REGSAM sam, PHKEY out)
    {
        std::wstring w = sub ? Widen(sub) : L"";
        return Shim_RegOpenKeyExW(root, sub ? w.c_str() : nullptr, options, sam, out);
    }
    LSTATUS WINAPI Shim_RegOpenKeyA(HKEY root, LPCSTR sub, PHKEY out) { return Shim_RegOpenKeyExA(root, sub, 0, KEY_ALL_ACCESS, out); }

    LSTATUS WINAPI Shim_RegSetValueExA(HKEY key, LPCSTR name, DWORD, DWORD type, const BYTE* data, DWORD size)
    {
        std::wstring n = name ? Widen(name) : L"";
        if (!IsString(type) || !data)
            return Shim_RegSetValueExW(key, name ? n.c_str() : nullptr, 0, type, data, size);
        // ANSI string data, possibly without its terminator; stored as UTF-16 with one.
        std::string s(reinterpret_cast<const char*>(data), size);
        while (!s.empty() && s.back() == 0 && type != REG_MULTI_SZ) s.pop_back();
        std::wstring w = Widen(s.c_str());
        if (type == REG_MULTI_SZ)
        {
            w.clear();
            for (size_t i = 0; i < s.size();)
            {
                std::string part = s.c_str() + i;
                w += Widen(part.c_str());
                w += L'\0';
                i += part.size() + 1;
            }
        }
        return Shim_RegSetValueExW(key, name ? n.c_str() : nullptr, 0, type, reinterpret_cast<const BYTE*>(w.c_str()),
            static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
    }

    LSTATUS WINAPI Shim_RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size)
    {
        std::wstring n = name ? Widen(name) : L"";
        std::lock_guard lock(g_lock);
        Load();
        std::wstring path;
        if (!Ours(key, nullptr, path))
            return RegQueryValueExA(key, name, nullptr, type, data, size);
        DWORD t = 0, wsize = 0;
        LSTATUS r = GetValue(path, n.c_str(), &t, nullptr, &wsize);
        if (r != ERROR_SUCCESS) return r;
        if (type) *type = t;
        std::vector<uint8_t> raw(wsize);
        GetValue(path, n.c_str(), nullptr, raw.data(), &wsize);
        if (IsString(t))
        {
            std::wstring w(reinterpret_cast<const wchar_t*>(raw.data()), wsize / sizeof(wchar_t));
            std::string s;
            for (size_t i = 0; i < w.size(); ++i) // keep embedded terminators (REG_MULTI_SZ)
            {
                std::string part = Narrow(std::wstring(1, w[i]).c_str());
                s += w[i] ? part : std::string(1, '\0');
            }
            raw.assign(s.begin(), s.end());
            if (raw.empty() || raw.back() != 0) raw.push_back(0);
        }
        DWORD n2 = static_cast<DWORD>(raw.size());
        if (!data) { if (size) *size = n2; return ERROR_SUCCESS; }
        if (!size) return ERROR_INVALID_PARAMETER;
        if (*size < n2) { *size = n2; return ERROR_MORE_DATA; }
        memcpy(data, raw.data(), n2);
        *size = n2;
        return ERROR_SUCCESS;
    }

    LSTATUS WINAPI Shim_RegDeleteValueA(HKEY key, LPCSTR name)
    {
        std::wstring n = name ? Widen(name) : L"";
        return Shim_RegDeleteValueW(key, name ? n.c_str() : nullptr);
    }
    LSTATUS WINAPI Shim_RegDeleteKeyA(HKEY root, LPCSTR sub)
    {
        std::wstring w = sub ? Widen(sub) : L"";
        return Shim_RegDeleteKeyW(root, sub ? w.c_str() : nullptr);
    }
    LSTATUS WINAPI Shim_RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD nameSize, LPDWORD, LPDWORD type, LPBYTE data,
        LPDWORD size)
    {
        wchar_t wname[16384];
        DWORD wn = 16384;
        std::lock_guard lock(g_lock);
        LSTATUS r = Shim_RegEnumValueW(key, index, wname, &wn, nullptr, type, nullptr, nullptr);
        if (r != ERROR_SUCCESS) return r;
        std::string n = Narrow(wname);
        if (!name || !nameSize || *nameSize <= n.size()) { if (nameSize) *nameSize = static_cast<DWORD>(n.size()); return ERROR_MORE_DATA; }
        memcpy(name, n.c_str(), n.size() + 1);
        *nameSize = static_cast<DWORD>(n.size());
        return Shim_RegQueryValueExA(key, n.c_str(), nullptr, type, data, size);
    }
}

void RegisterRegistry(Win32Layer& layer)
{
#define REG(fn) layer.Register("advapi32!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    REG(RegCreateKeyExW); REG(RegCreateKeyW); REG(RegOpenKeyExW); REG(RegOpenKeyW); REG(RegCloseKey);
    REG(RegSetValueExW); REG(RegQueryValueExW); REG(RegDeleteValueW); REG(RegDeleteKeyW); REG(RegEnumValueW);
    REG(RegQueryInfoKeyW); REG(RegFlushKey);
    REG(RegCreateKeyExA); REG(RegCreateKeyA); REG(RegOpenKeyExA); REG(RegOpenKeyA); REG(RegSetValueExA);
    REG(RegQueryValueExA); REG(RegDeleteValueA); REG(RegDeleteKeyA); REG(RegEnumValueA);
#undef REG
}
