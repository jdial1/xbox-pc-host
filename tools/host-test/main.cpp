// Desktop harness for the loader: runs the same PeImage / Win32Layer / runtime
// code as the app, without the UWP shell, so it works over SSH.
//   host-test.exe <exe> [--expect "text"] [--timeout seconds]
//   --inject keys|pad  holds scripted keyboard/mouse or controller input for a moment, then closes the window
//   --shot file.bmp    saves the last software (GDI) frame when the run ends
//   --root dir         the game's folder, when the exe is below it (bin\x64\Factorio.exe)
//   --watch-layer 1    a hardware watchpoint on the import table, in every thread: logs who overwrites it
//   --script file      timed steps, one per line ("#" starts a comment), seconds from the game's start:
//                        <s> move <x> <y>   pointer to a window position (WM_MOUSEMOVE)
//                        <s> click [right]  press and release a button where the pointer is
//                        <s> key <vk>       press and release a virtual key (decimal, or 0x hex)
//                        <s> text <words>   type the rest of the line (key down, WM_CHAR, key up each)
//                        <s> shot <file>    save the newest frame as a BMP
//                        <s> close          send WM_CLOSE
//                        <s> pad <A+B|none> hold controller buttons (A B X Y LB RB Menu View Up Down Left Right LS RS)
//                        <s> stick <x> <y>  left stick, -1..1 (y up); rstick for the right one
//                        <s> trigger <l> <r> triggers, 0..1
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
#include <csignal>
#include <exception>
#include <set>
#include <thread>
#include <tlhelp32.h>

// DXGI lists the high-performance GPU first for an exe that exports these (games do; a game's own exe
// isn't the process here). Otherwise a PC with a software adapter at 0 hands it to games that take the
// first adapter, as Mesa's d3d12 OpenGL does. The console has one GPU.
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

namespace
{
    // When the host's CRT is about to kill the process (abort, terminate, an invalid parameter to a
    // _s function), say where from: module+offset per frame, "game+offset" inside the mapped exe.
    void LogStack(const wchar_t* why)
    {
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
        HOST_LOG(L"host-test: %s; stack:", why);
        for (USHORT i = 0; i < count; ++i)
        {
            HMODULE module = nullptr;
            wchar_t name[MAX_PATH] = L"";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(frames[i]), &module))
            {
                GetModuleFileNameW(module, name, MAX_PATH);
                HOST_LOG(L"  %s+0x%llx", std::filesystem::path(name).filename().c_str(),
                    static_cast<unsigned long long>(static_cast<uint8_t*>(frames[i]) - reinterpret_cast<uint8_t*>(module)));
            }
            else
                HOST_LOG(L"  game+0x%llx (or another unregistered image: %p)",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - 0x140000000ull), frames[i]);
        }
    }

    void InstallCrashLogging()
    {
        std::signal(SIGABRT, [](int) { LogStack(L"abort()"); });
        std::set_terminate([] {
            LogStack(L"std::terminate");
            std::_Exit(3);
        });
        _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
            LogStack(L"invalid parameter to a CRT function");
        });
    }

    // --watch-layer: DR0 watches 8 bytes for writes in every thread of the process (armed as threads
    // appear); the vectored handler logs the writer's stack and lets it carry on.
    void* g_watchAddress = nullptr;
    std::atomic<int> g_watchHits{ 0 };

    LONG CALLBACK WatchHit(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1))
            return EXCEPTION_CONTINUE_SEARCH;
        info->ContextRecord->Dr6 = 0;
        if (g_watchHits++ < 8)
        {
            HOST_LOG(L"watch: %p now holds %p (thread %lu)", g_watchAddress, *static_cast<void**>(g_watchAddress), GetCurrentThreadId());
            LogGameStack(L"watch: written by", info->ContextRecord);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ArmWatch(std::stop_token stop)
    {
        std::set<DWORD> armed{ GetCurrentThreadId() };
        while (!stop.stop_requested())
        {
            if (!g_watchAddress)
                g_watchAddress = GameImportTableWatchAddress();
            if (g_watchAddress)
            {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{ sizeof(te) };
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || armed.count(te.th32ThreadID))
                        continue;
                    armed.insert(te.th32ThreadID);
                    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
                    if (!t) continue;
                    if (SuspendThread(t) != static_cast<DWORD>(-1))
                    {
                        CONTEXT c{};
                        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        if (GetThreadContext(t, &c))
                        {
                            c.Dr0 = reinterpret_cast<DWORD64>(g_watchAddress);
                            c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (2ull << 18); // L0, write, 8 bytes
                            SetThreadContext(t, &c);
                        }
                        ResumeThread(t);
                    }
                    CloseHandle(t);
                }
                CloseHandle(snap);
            }
            Sleep(20);
        }
    }

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
            if (step.action == L"text")
            {
                std::getline(words, step.a); // the rest of the line, spaces included
                step.a.erase(0, step.a.find_first_not_of(L" 	"));
                step.a.erase(step.a.find_last_not_of(L" 	") + 1);
            }
            else
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

    // The controller as the script holds it; applied to the input every tick once a pad step ran.
    PadState g_scriptPad;
    bool g_scriptPadOn = false;

    uint32_t PadButtons(std::wstring const& names)
    {
        using B = winrt::Windows::Gaming::Input::GamepadButtons;
        static const std::pair<const wchar_t*, B> kNames[] = { { L"a", B::A }, { L"b", B::B }, { L"x", B::X }, { L"y", B::Y },
            { L"lb", B::LeftShoulder }, { L"rb", B::RightShoulder }, { L"menu", B::Menu }, { L"start", B::Menu },
            { L"view", B::View }, { L"back", B::View }, { L"up", B::DPadUp }, { L"down", B::DPadDown },
            { L"left", B::DPadLeft }, { L"right", B::DPadRight }, { L"ls", B::LeftThumbstick }, { L"rs", B::RightThumbstick } };
        uint32_t bits = 0;
        std::wistringstream parts(names);
        std::wstring part;
        while (std::getline(parts, part, L'+'))
        {
            std::transform(part.begin(), part.end(), part.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            bool known = part == L"none";
            for (auto const& [name, bit] : kNames)
                if (part == name)
                {
                    bits |= static_cast<uint32_t>(bit);
                    known = true;
                }
            if (!known)
                fwprintf(stderr, L"script: unknown button %s\n", part.c_str());
        }
        return bits;
    }

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
            PostGameMessage(kWmKeyDown, vk, KeyMessageLParam(static_cast<UINT>(vk), true));
            PostGameMessage(kWmKeyUp, vk, KeyMessageLParam(static_cast<UINT>(vk), false));
        }
        else if (step.action == L"text")
        {
            // Each character as a real keyboard delivers it: key down, the character, key up.
            for (wchar_t c : step.a)
            {
                auto vk = static_cast<WPARAM>(std::iswalnum(c) || c == L' ' ? std::towupper(c) : 0);
                if (vk) PostGameMessage(kWmKeyDown, vk, KeyMessageLParam(static_cast<UINT>(vk), true));
                PostGameMessage(0x0102 /* WM_CHAR */, c, 1);
                if (vk) PostGameMessage(kWmKeyUp, vk, KeyMessageLParam(static_cast<UINT>(vk), false));
            }
        }
        else if (step.action == L"shot")
            SaveFrame(step.a);
        else if (step.action == L"close")
            PostGameMessage(kWmClose, 0, 0);
        else if (step.action == L"pad")
        {
            uint32_t bits = PadButtons(step.a);
            g_scriptPad.pressed = bits & ~g_scriptPad.buttons;
            g_scriptPad.buttons = bits;
            g_scriptPad.connected = g_scriptPadOn = true;
        }
        else if (step.action == L"stick" || step.action == L"rstick")
        {
            float x = static_cast<float>(_wtof(step.a.c_str())), y = static_cast<float>(_wtof(step.b.c_str()));
            (step.action == L"stick" ? g_scriptPad.leftX : g_scriptPad.rightX) = x;
            (step.action == L"stick" ? g_scriptPad.leftY : g_scriptPad.rightY) = y;
            g_scriptPad.connected = g_scriptPadOn = true;
        }
        else if (step.action == L"trigger")
        {
            g_scriptPad.leftTrigger = static_cast<float>(_wtof(step.a.c_str()));
            g_scriptPad.rightTrigger = static_cast<float>(_wtof(step.b.c_str()));
            g_scriptPad.connected = g_scriptPadOn = true;
        }
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
    std::filesystem::path root; // the game's folder when its exe is below it (default: the exe's folder)
    std::vector<Step> script;
    bool watchLayer = false;
    int timeout = 20;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (wcscmp(argv[i], L"--expect") == 0) expect = argv[i + 1];
        else if (wcscmp(argv[i], L"--timeout") == 0) timeout = _wtoi(argv[i + 1]);
        else if (wcscmp(argv[i], L"--inject") == 0) inject = argv[i + 1];
        else if (wcscmp(argv[i], L"--shot") == 0) shot = argv[i + 1];
        else if (wcscmp(argv[i], L"--root") == 0) root = std::filesystem::absolute(argv[i + 1]);
        else if (wcscmp(argv[i], L"--script") == 0) script = LoadScript(argv[i + 1]);
        else if (wcscmp(argv[i], L"--watch-layer") == 0) watchLayer = true;
    }

    host::log::Init(exe.parent_path() / L"host.log");
    InstallCrashLogging();

    // The runtime only touches the library for bundled games; writable entries skip that.
    if (root.empty())
        root = exe.parent_path();
    GameEntry game{ root.filename().wstring(), root, exe, true };
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
    std::jthread watcher;
    if (watchLayer)
    {
        AddVectoredExceptionHandler(1, WatchHit);
        watcher = std::jthread(ArmWatch);
    }
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
        if (g_scriptPadOn)
            pad = g_scriptPad;

        runtime->Tick(); // runs what the game asks of the "UI thread", and feeds it input
        g_scriptPad.pressed = 0; // a press lasts one tick
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
