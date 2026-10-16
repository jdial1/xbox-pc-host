#include "pch.h"
#include <fstream>
#include "Audio.h"
#include "GameLibrary.h"
#include "GameRuntime.h"
#include "Graphics.h"
#include "Input.h"
#include "Log.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::System::Profile;
using namespace winrt::Windows::UI::Core;

// The host shell: a CoreWindow app with no XAML. It owns the window, the
// swapchain, input, audio and the games folders, and shows a status screen
// with the game list until a runtime takes over the frame.
struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const& view)
    {
        auto logPath = std::filesystem::path(ApplicationData::Current().LocalFolder().Path().c_str()) / L"host.log";
        host::log::Init(logPath);
        HOST_LOG(L"XboxPcHost starting on %s, memory limit %llu MB", AnalyticsInfo::VersionInfo().DeviceFamily().c_str(),
            winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() / (1024 * 1024));

        view.Activated({ this, &App::OnActivated });
        CoreApplication::Suspending({ this, &App::OnSuspending });
        CoreApplication::Resuming({ this, &App::OnResuming });
    }

    void SetWindow(CoreWindow const& window)
    {
        window.SizeChanged({ this, &App::OnSizeChanged });
        window.VisibilityChanged([this](auto&&, VisibilityChangedEventArgs const& args) { m_visible = args.Visible(); });
        window.Closed([this](auto&&, auto&&) { m_closed = true; });
        DisplayInformation::GetForCurrentView().DpiChanged([this](auto&&, auto&&) { m_graphics.OnWindowSizeChanged(); });

        // On Xbox the B button raises "back"; unhandled, it would close the app.
        SystemNavigationManager::GetForCurrentView().BackRequested([](auto&&, BackRequestedEventArgs const& args)
        {
            args.Handled(true);
        });

        m_graphics.Initialize(window);
        m_input.Initialize(window);
        m_audio.Initialize();
        m_library.Initialize();
        m_runtime = CreateGameRuntime();
    }

    void Load(hstring const&) {}

    void Run()
    {
        CoreDispatcher dispatcher = CoreWindow::GetForCurrentThread().Dispatcher();
        while (!m_closed)
        {
            if (m_visible)
            {
                dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                Frame();
            }
            else
            {
                dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            }
        }
    }

    void Uninitialize()
    {
        if (m_runtime) m_runtime->Stop();
    }

private:
    void OnActivated(CoreApplicationView const&, IActivatedEventArgs const&)
    {
        CoreWindow::GetForCurrentThread().Activate();
    }

    void OnSuspending(IInspectable const&, SuspendingEventArgs const& args)
    {
        auto deferral = args.SuspendingOperation().GetDeferral();
        HOST_LOG(L"Suspending");
        m_audio.Suspend();
        m_graphics.Trim();
        deferral.Complete();
    }

    void OnResuming(IInspectable const&, IInspectable const&)
    {
        HOST_LOG(L"Resuming");
        m_audio.Resume();
    }

    void OnSizeChanged(CoreWindow const&, WindowSizeChangedEventArgs const&)
    {
        m_graphics.OnWindowSizeChanged();
    }

    void Frame()
    {
        m_input.Update();

        if (m_runtime && m_runtime->IsRunning())
            m_runtime->Tick();
        else
            UpdateShell();
        // Until the game presents its own frames, the shell stays up to show its status.
        DrawShell();
    }

    // A file named autorun.txt in LocalState holds a game's name; the shell starts that game once, as if
    // A had been pressed on it. The file is renamed so each upload runs once. This lets the test script
    // launch a game over Device Portal with nobody at the controller.
    void TryAutorun()
    {
        if (m_autorunDone || ++m_frames < 120)
            return;
        m_autorunDone = true;
        auto file = m_library.UserGamesRoot().parent_path() / L"autorun.txt";
        std::ifstream in(file);
        std::string name;
        if (!in || !std::getline(in, name))
            return;
        in.close();
        std::error_code ec;
        std::filesystem::rename(file, file.parent_path() / L"autorun.used", ec);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();

        auto const& games = m_library.Games();
        for (size_t i = 0; i < games.size(); ++i)
        {
            if (_wcsicmp(games[i].name.c_str(), std::wstring(name.begin(), name.end()).c_str()) != 0)
                continue;
            HOST_LOG(L"autorun: %s", games[i].name.c_str());
            m_selected = i;
            HostServices services{ m_graphics, m_input, m_audio, m_library };
            m_runtime->Start(games[i], services);
            m_launched = true;
            return;
        }
        HOST_LOG(L"autorun: no game named %S", name.c_str());
    }

    void UpdateShell()
    {
        TryAutorun();
        PadState const& pad = m_input.Pad();
        auto const& games = m_library.Games();

        if (pad.Pressed(GamepadButtons::DPadDown) && !games.empty())
            m_selected = (m_selected + 1) % games.size();
        if (pad.Pressed(GamepadButtons::DPadUp) && !games.empty())
            m_selected = (m_selected + games.size() - 1) % games.size();
        if (pad.Pressed(GamepadButtons::Y))
        {
            m_library.Rescan();
            m_selected = 0;
            m_message = L"Rescanned games folders";
        }
        if (pad.Pressed(GamepadButtons::X))
        {
            m_audio.PlayTestTone();
            m_message = m_audio.Ready() ? L"Played test tone" : L"Audio is not available";
        }
        if (pad.Pressed(GamepadButtons::A) && m_selected < games.size())
        {
            HostServices services{ m_graphics, m_input, m_audio, m_library };
            m_runtime->Start(games[m_selected], services);
            m_launched = true;
        }

        // Left stick tints the background so input is visible at a glance.
        m_tintR = 0.08f + 0.10f * std::max(0.0f, pad.leftX) + 0.25f * pad.leftTrigger;
        m_tintG = 0.10f + 0.10f * std::max(0.0f, pad.leftY);
        m_tintB = 0.18f + 0.25f * pad.rightTrigger;
    }

    void DrawShell()
    {
        PadState const& pad = m_input.Pad();
        auto const& games = m_library.Games();
        wchar_t buf[256];

        std::vector<std::wstring> lines;
        lines.push_back(L"Xbox PC Host - shell");
        swprintf_s(buf, L"GPU: %s   output: %ux%u", m_graphics.AdapterName().c_str(), m_graphics.Width(), m_graphics.Height());
        lines.push_back(buf);
        if (pad.connected)
            swprintf_s(buf, L"Pad: L(%+.2f,%+.2f) R(%+.2f,%+.2f) LT %.2f RT %.2f buttons 0x%05x",
                pad.leftX, pad.leftY, pad.rightX, pad.rightY, pad.leftTrigger, pad.rightTrigger, pad.buttons);
        else
            swprintf_s(buf, L"Pad: not connected");
        lines.push_back(buf);
        // About 1 GB when registered as an App, several GB as a Game.
        swprintf_s(buf, L"Memory limit: %llu MB", winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() / (1024 * 1024));
        lines.push_back(buf);
        lines.push_back(std::wstring(L"Audio: ") + (m_audio.Ready() ? L"XAudio2 48 kHz stereo" : L"unavailable"));
        lines.push_back(L"Games folder: " + m_library.UserGamesRoot().wstring());
        lines.push_back(L"");

        size_t firstGameLine = lines.size();
        if (games.empty())
            lines.push_back(L"No games found. Copy a game folder into the Games folder above, then press Y.");
        for (auto const& g : games)
            lines.push_back(L"  " + g.name + (g.writable ? L"" : L"  [bundled]"));

        lines.push_back(L"");
        lines.push_back(L"D-pad: select   A: launch   Y: rescan   X: test tone");
        if (m_launched) lines.push_back(m_runtime->LastMessage());
        else if (!m_message.empty()) lines.push_back(m_message);

        m_graphics.BeginFrame(m_tintR, m_tintG, m_tintB);
        m_graphics.DrawTextLines(lines, games.empty() ? SIZE_MAX : firstGameLine + m_selected);
        m_graphics.Present();
    }

    Graphics m_graphics;
    Input m_input;
    Audio m_audio;
    GameLibrary m_library;
    std::unique_ptr<IGameRuntime> m_runtime;
    bool m_autorunDone = false;
    int m_frames = 0;
    bool m_launched = false; // a game was started; the shell shows the runtime's status line

    bool m_visible = true;
    bool m_closed = false;
    size_t m_selected = 0;
    std::wstring m_message;
    float m_tintR = 0, m_tintG = 0, m_tintB = 0;
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment();
    CoreApplication::Run(make<App>());
    return 0;
}
