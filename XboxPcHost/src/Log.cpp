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
    // Keep the previous run's log: after a crash the new run would otherwise overwrite the evidence.
    std::error_code ec;
    if (std::filesystem::exists(file, ec))
        std::filesystem::rename(file, std::filesystem::path(file).replace_filename(L"host.prev.log"), ec);
    // Shared for reading so the log can be tailed while the app runs.
    g_file = _wfsopen(file.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
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
