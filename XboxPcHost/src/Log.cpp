#include "pch.h"
#include "Log.h"

namespace
{
    std::mutex g_lock;
    FILE* g_file = nullptr;
}

void host::log::Init(std::filesystem::path const& file)
{
    std::lock_guard guard(g_lock);
    if (g_file) fclose(g_file);
    if (_wfopen_s(&g_file, file.c_str(), L"w, ccs=UTF-8") != 0) g_file = nullptr;
}

void host::log::Write(const wchar_t* fmt, ...)
{
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    int len = _vsnwprintf_s(buffer, _countof(buffer) - 2, _TRUNCATE, fmt, args);
    va_end(args);
    if (len < 0) len = static_cast<int>(wcslen(buffer));
    buffer[len] = L'\n';
    buffer[len + 1] = L'\0';

    std::lock_guard guard(g_lock);
    OutputDebugStringW(buffer);
    if (g_file)
    {
        fputws(buffer, g_file);
        fflush(g_file);
    }
}
