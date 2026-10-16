// Desktop harness for the loader: runs the same PeImage / Win32Layer / runtime
// code as the app, without the UWP shell, so it works over SSH.
//   host-test.exe <exe> [--expect "text"] [--timeout seconds]
//   --inject keys|pad  holds scripted keyboard/mouse or controller input for a moment, then closes the window
// Exit code: 0 ok, 1 usage or load failure, 2 expectation not met, 3 timeout.
#include "pch.h"
#include "GameRuntime.h"
#include "Graphics.h"
#include "Audio.h"
#include "Input.h"
#include "loader/Win32Layer.h"
#include "Log.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fputws(L"usage: host-test <exe> [--expect text] [--timeout seconds]\n", stderr);
        return 1;
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded); // XAudio2 needs COM
    std::filesystem::path exe = std::filesystem::absolute(argv[1]);
    std::wstring expect;
    std::wstring inject;
    int timeout = 20;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (wcscmp(argv[i], L"--expect") == 0) expect = argv[i + 1];
        else if (wcscmp(argv[i], L"--timeout") == 0) timeout = _wtoi(argv[i + 1]);
        else if (wcscmp(argv[i], L"--inject") == 0) inject = argv[i + 1];
    }

    host::log::Init(exe.parent_path() / L"host.log");

    // The runtime only touches the library for bundled games; writable entries skip that.
    GameEntry game{ exe.parent_path().filename().wstring(), exe.parent_path(), exe, true };
    GameLibrary library;
    Graphics graphics; // never initialized: it reports size 0 and refuses to hand over a window
    Input input; // never initialized: no devices, so the game sees an idle keyboard and mouse
    Audio audio;
    audio.Initialize(); // real XAudio2 where the machine has an audio device, otherwise no audio
    HostServices services{ graphics, input, audio, library };

    auto runtime = CreateGameRuntime();
    runtime->Start(game, services);
    for (int ms = 0; runtime->IsRunning() && ms < timeout * 1000; ms += 10)
    {
        // Test only: Input hands out const references, but the objects behind them are ours.
        auto& kbm = const_cast<KeyboardMouseState&>(input.KeyboardMouse());
        auto& pad = const_cast<PadState&>(input.Pad());
        if (ms == 300 && inject == L"keys")
        {
            kbm.keys['W'] = kbm.keys[VK_SPACE] = true;
            kbm.mouseDeltaX = 10;
            kbm.mouseButtons[0] = true;
            kbm.wheel = 1;
        }
        if (ms == 300 && inject == L"pad")
        {
            pad.connected = true;
            pad.leftY = 1;
            pad.rightX = 1;
            pad.buttons = pad.pressed = static_cast<uint32_t>(winrt::Windows::Gaming::Input::GamepadButtons::A |
                winrt::Windows::Gaming::Input::GamepadButtons::RightShoulder);
            pad.rightTrigger = 1;
        }
        if (ms == 400)
        {
            kbm = KeyboardMouseState{};
            pad = PadState{};
        }
        if (ms == 600 && !inject.empty())
            PostGameMessage(0x0010 /* WM_CLOSE */, 0, 0);

        runtime->Tick(); // runs what the game asks of the "UI thread", and feeds it input
        Sleep(10);
    }

    std::wstring message = runtime->LastMessage();
    wprintf(L"%s\n", message.c_str());
    if (runtime->IsRunning()) return 3;
    if (!expect.empty() && message.find(expect) == std::wstring::npos) return 2;
    return message.find(L" exited with code") != std::wstring::npos || !expect.empty() ? 0 : 1;
}
