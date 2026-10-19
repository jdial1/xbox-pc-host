// Desktop harness for the loader: runs the same PeImage / Win32Layer / runtime
// code as the app, without the UWP shell, so it works over SSH.
//   host-test.exe <exe> [--expect "text"] [--timeout seconds]
//   --inject keys|pad  holds scripted keyboard/mouse or controller input for a moment, then closes the window
//   --shot file.bmp    saves the last software (GDI) frame when the run ends
//   --script file      timed steps, one per line ("#" starts a comment), seconds from the game's start:
//                        <s> move <x> <y>   pointer to a window position (WM_MOUSEMOVE)
//                        <s> click [right]  press and release a button where the pointer is
//                        <s> key <vk>       press and release a virtual key (decimal, or 0x hex)
//                        <s> text <word>    type the word as WM_CHAR messages (no spaces)
//                        <s> shot <file>    save the newest frame as a BMP
//                        <s> close          send WM_CLOSE
// Exit code: 0 ok, 1 usage or load failure, 2 expectation not met, 3 timeout.
#include "pch.h"
#include "GameRuntime.h"
#include "Graphics.h"
#include "Audio.h"
#include "Input.h"
#include "loader/Win32Layer.h"
#include "Log.h"
#include <fstream>
#include <sstream>

namespace
{
    // The newest frame the game presented, kept for --shot and the script's "shot".
    std::mutex g_frameLock;
    std::vector<uint32_t> g_frame;
    UINT g_frameWidth = 0, g_frameHeight = 0;

    bool SaveFrame(std::filesystem::path const& file)
    {
        std::lock_guard lock(g_frameLock);
        if (g_frame.empty())
        {
            fwprintf(stderr, L"shot %s: no frame yet\n", file.c_str());
            return false;
        }
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = static_cast<LONG>(g_frameWidth);
        ih.biHeight = -static_cast<LONG>(g_frameHeight); // top-down
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        DWORD bytes = static_cast<DWORD>(g_frame.size() * 4);
        fh.bfType = 0x4D42; // "BM"
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + bytes;
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
        out.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
        out.write(reinterpret_cast<const char*>(g_frame.data()), bytes);
        HOST_LOG(L"host-test: saved %ux%u frame to %s", g_frameWidth, g_frameHeight, file.c_str());
        return static_cast<bool>(out);
    }

    struct Step
    {
        int ms;
        std::wstring action;
        std::wstring a, b;
    };

    std::vector<Step> LoadScript(std::filesystem::path const& file)
    {
        std::vector<Step> steps;
        std::wifstream in(file);
        std::wstring line;
        while (std::getline(in, line))
        {
            if (auto hash = line.find(L'#'); hash != std::wstring::npos)
                line.erase(hash);
            std::wistringstream words(line);
            double seconds;
            Step step;
            if (!(words >> seconds >> step.action))
                continue;
            words >> step.a >> step.b;
            step.ms = static_cast<int>(seconds * 1000);
            steps.push_back(step);
        }
        std::stable_sort(steps.begin(), steps.end(), [](Step const& x, Step const& y) { return x.ms < y.ms; });
        return steps;
    }

    // Messages go straight into the game's queue, as the window shims would post them.
    constexpr UINT kWmKeyDown = 0x0100, kWmKeyUp = 0x0101, kWmMouseMove = 0x0200, kWmLButtonDown = 0x0201,
        kWmLButtonUp = 0x0202, kWmRButtonDown = 0x0204, kWmRButtonUp = 0x0205, kWmClose = 0x0010;
    int g_pointerX = 0, g_pointerY = 0;

    void RunStep(Step const& step)
    {
        LPARAM at = MAKELPARAM(g_pointerX, g_pointerY);
        if (step.action == L"move")
        {
            g_pointerX = _wtoi(step.a.c_str());
            g_pointerY = _wtoi(step.b.c_str());
            PostGameMessage(kWmMouseMove, 0, MAKELPARAM(g_pointerX, g_pointerY));
        }
        else if (step.action == L"click")
        {
            bool right = step.a == L"right";
            PostGameMessage(right ? kWmRButtonDown : kWmLButtonDown, right ? 0x0002 : 0x0001, at);
            PostGameMessage(right ? kWmRButtonUp : kWmLButtonUp, 0, at);
        }
        else if (step.action == L"key")
        {
            auto vk = static_cast<WPARAM>(wcstoul(step.a.c_str(), nullptr, 0));
            PostGameMessage(kWmKeyDown, vk, 1);
            PostGameMessage(kWmKeyUp, vk, 0xC0000001);
        }
        else if (step.action == L"text")
        {
            // Each character as a real keyboard delivers it: key down, the character, key up.
            for (wchar_t c : step.a)
            {
                auto vk = static_cast<WPARAM>(std::iswalnum(c) ? std::towupper(c) : 0);
                if (vk) PostGameMessage(kWmKeyDown, vk, 1);
                PostGameMessage(0x0102 /* WM_CHAR */, c, 1);
                if (vk) PostGameMessage(kWmKeyUp, vk, 0xC0000001);
            }
        }
        else if (step.action == L"shot")
            SaveFrame(step.a);
        else if (step.action == L"close")
            PostGameMessage(kWmClose, 0, 0);
        else
            fwprintf(stderr, L"script: unknown action %s\n", step.action.c_str());
        HOST_LOG(L"host-test: step %d ms %s %s %s", step.ms, step.action.c_str(), step.a.c_str(), step.b.c_str());
    }
}

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
    std::filesystem::path shot;
    std::vector<Step> script;
    int timeout = 20;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (wcscmp(argv[i], L"--expect") == 0) expect = argv[i + 1];
        else if (wcscmp(argv[i], L"--timeout") == 0) timeout = _wtoi(argv[i + 1]);
        else if (wcscmp(argv[i], L"--inject") == 0) inject = argv[i + 1];
        else if (wcscmp(argv[i], L"--shot") == 0) shot = argv[i + 1];
        else if (wcscmp(argv[i], L"--script") == 0) script = LoadScript(argv[i + 1]);
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

    SetGameFrameTap([](const uint32_t* bgra, UINT width, UINT height, UINT pitchBytes) {
        std::lock_guard lock(g_frameLock);
        g_frame.resize(static_cast<size_t>(width) * height);
        for (UINT y = 0; y < height; ++y)
            memcpy(&g_frame[static_cast<size_t>(y) * width], reinterpret_cast<const uint8_t*>(bgra) + static_cast<size_t>(y) * pitchBytes,
                static_cast<size_t>(width) * 4);
        g_frameWidth = width;
        g_frameHeight = height;
    });

    auto runtime = CreateGameRuntime();
    runtime->Start(game, services);
    size_t nextStep = 0;
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

        while (nextStep < script.size() && script[nextStep].ms <= ms)
            RunStep(script[nextStep++]);

        runtime->Tick(); // runs what the game asks of the "UI thread", and feeds it input
        Sleep(10);
    }

    if (!shot.empty())
        SaveFrame(shot);
    std::wstring message = runtime->LastMessage();
    wprintf(L"%s\n", message.c_str());
    if (runtime->IsRunning()) return 3;
    if (!expect.empty() && message.find(expect) == std::wstring::npos) return 2;
    return message.find(L" exited with code") != std::wstring::npos || !expect.empty() ? 0 : 1;
}
