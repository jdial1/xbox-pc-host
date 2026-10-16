// Desktop harness for the loader: runs the same PeImage / Win32Layer / runtime
// code as the app, without the UWP shell, so it works over SSH.
//   host-test.exe <exe> [--expect "text"] [--timeout seconds]
// Exit code: 0 ok, 1 usage or load failure, 2 expectation not met, 3 timeout.
#include "pch.h"
#include "GameRuntime.h"
#include "Graphics.h"
#include "Log.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fputws(L"usage: host-test <exe> [--expect text] [--timeout seconds]\n", stderr);
        return 1;
    }
    std::filesystem::path exe = std::filesystem::absolute(argv[1]);
    std::wstring expect;
    int timeout = 20;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (wcscmp(argv[i], L"--expect") == 0) expect = argv[i + 1];
        else if (wcscmp(argv[i], L"--timeout") == 0) timeout = _wtoi(argv[i + 1]);
    }

    host::log::Init(exe.parent_path() / L"host.log");

    // The runtime only touches the library for bundled games; writable entries skip that.
    GameEntry game{ exe.parent_path().filename().wstring(), exe.parent_path(), exe, true };
    GameLibrary library;
    Graphics graphics; // never initialized: it reports size 0 and refuses to hand over a window
    alignas(16) static char unused[16]; // services the runtime doesn't use yet
    HostServices services{ graphics, reinterpret_cast<Input&>(unused),
        reinterpret_cast<Audio&>(unused), library };

    auto runtime = CreateGameRuntime();
    runtime->Start(game, services);
    for (int ms = 0; runtime->IsRunning() && ms < timeout * 1000; ms += 10)
    {
        runtime->Tick(); // runs what the game asks of the "UI thread"
        Sleep(10);
    }

    std::wstring message = runtime->LastMessage();
    wprintf(L"%s\n", message.c_str());
    if (runtime->IsRunning()) return 3;
    if (!expect.empty() && message.find(expect) == std::wstring::npos) return 2;
    return message.find(L" exited with code") != std::wstring::npos || !expect.empty() ? 0 : 1;
}
