#pragma once

// Tiny logger: everything goes to the debugger and to LocalState\host.log.
// Later stages (loader, Win32 call layer) log unimplemented calls through this.
namespace host::log
{
    void Init(std::filesystem::path const& file);
    void Write(const wchar_t* fmt, ...);
}

#define HOST_LOG(...) ::host::log::Write(__VA_ARGS__)
