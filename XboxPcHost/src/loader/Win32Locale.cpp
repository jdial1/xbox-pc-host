#include "pch.h"
#include "loader/Win32Layer.h"

// The LCID forms of the locale calls aren't in the app partition; the
// locale-name forms are. Each shim turns the LCID into a name and calls those.
namespace
{
    // LOCALE_USER_DEFAULT and friends map to the user's locale, anything else by its LCID.
    bool NameFor(LCID lcid, wchar_t (&name)[LOCALE_NAME_MAX_LENGTH])
    {
        if (lcid == LOCALE_USER_DEFAULT || lcid == LOCALE_SYSTEM_DEFAULT || lcid == LOCALE_INVARIANT ||
            lcid == LOCALE_CUSTOM_DEFAULT)
            return GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0;
        return LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) > 0;
    }

    LCID WINAPI Shim_GetUserDefaultLCID()
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        LCID lcid = GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) ? LocaleNameToLCID(name, 0) : 0;
        return lcid ? lcid : 0x0409; // en-US
    }

    BOOL WINAPI Shim_IsValidLocale(LCID lcid, DWORD)
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        return NameFor(lcid, name) && IsValidLocaleName(name);
    }

    int WINAPI Shim_LCMapStringW(LCID lcid, DWORD flags, LPCWSTR src, int srcLen, LPWSTR dst, int dstLen)
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        if (!NameFor(lcid, name)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        return LCMapStringEx(name, flags, src, srcLen, dst, dstLen, nullptr, nullptr, 0);
    }

    int WINAPI Shim_GetDateFormatW(LCID lcid, DWORD flags, const SYSTEMTIME* time, LPCWSTR format, LPWSTR out, int size)
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        if (!NameFor(lcid, name)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        return GetDateFormatEx(name, flags, time, format, out, size, nullptr);
    }

    int WINAPI Shim_GetTimeFormatW(LCID lcid, DWORD flags, const SYSTEMTIME* time, LPCWSTR format, LPWSTR out, int size)
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        if (!NameFor(lcid, name)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
        return GetTimeFormatEx(name, flags, time, format, out, size);
    }

    // The CRT only enumerates to match a locale name; offering none makes it use the name it was given.
    BOOL WINAPI Shim_EnumSystemLocalesW(LOCALE_ENUMPROCW, DWORD) { return TRUE; }
}

void RegisterLocale(Win32Layer& layer)
{
#define SHIM(fn) layer.Register("kernel32!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    SHIM(GetUserDefaultLCID);
    SHIM(IsValidLocale);
    SHIM(LCMapStringW);
    SHIM(GetDateFormatW);
    SHIM(GetTimeFormatW);
    SHIM(EnumSystemLocalesW);
#undef SHIM
}
