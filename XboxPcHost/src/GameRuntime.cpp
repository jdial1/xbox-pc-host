#include "pch.h"
#include "GameRuntime.h"
#include "Log.h"
#include "Audio.h"
#include "Graphics.h"
#include "InputMapping.h"
#include "loader/GameDisplay.h"
#include "loader/PeImage.h"
#include "loader/Win32Layer.h"

namespace fs = std::filesystem;

namespace
{
    // Runs the entry point; SEH can't share a function with C++ unwinding.
    bool CallEntry(void* entry, DWORD& exitCode, EXCEPTION_RECORD& crash)
    {
        __try
        {
            exitCode = reinterpret_cast<DWORD(WINAPI*)()>(entry)();
            return true;
        }
        __except (crash = *GetExceptionInformation()->ExceptionRecord, EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The game's window reports the output size; before the swapchain exists that is 0, so fall back to 1080p.
    // The CoreWindow belongs to the UI thread, so swapchain creation is queued for Pump().
    class GraphicsDisplay final : public GameDisplay
    {
    public:
        explicit GraphicsDisplay(Graphics& graphics) : m_graphics(graphics) {}
        UINT Width() const override { return m_graphics.Width() ? m_graphics.Width() : 1920; }
        UINT Height() const override { return m_graphics.Height() ? m_graphics.Height() : 1080; }

        HRESULT CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC1 const& desc, IDXGISwapChain1** swapChain) override
        {
            auto job = std::make_shared<Job>();
            job->device.copy_from(device);
            job->desc = desc;
            {
                std::lock_guard lock(m_lock);
                m_jobs.push_back(job);
            }
            std::unique_lock lock(job->lock);
            if (!job->done.wait_for(lock, std::chrono::seconds(5), [&] { return job->finished; }))
            {
                job->abandoned = true; // Pump() skips it
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            *swapChain = job->result.detach();
            return job->hr;
        }

        void PresentPixels(const uint32_t* bgra, UINT width, UINT height, UINT pitchBytes) override
        {
            // Only the newest frame matters; the UI thread draws it on its next pass. The alpha
            // channel is forced opaque because GDI bitmaps leave it zero.
            std::vector<uint32_t> copy(static_cast<size_t>(width) * height);
            for (UINT y = 0; y < height; ++y)
            {
                auto row = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(bgra) + static_cast<size_t>(y) * pitchBytes);
                for (UINT x = 0; x < width; ++x)
                    copy[static_cast<size_t>(y) * width + x] = row[x] | 0xFF000000;
            }
            std::lock_guard lock(m_lock);
            m_frame = std::move(copy);
            m_frameWidth = width;
            m_frameHeight = height;
        }

        void GameEnded() override { m_graphics.ClearGameSurface(); }

        // UI thread, once per frame while the game runs.
        void Pump()
        {
            std::vector<uint32_t> frame;
            UINT frameWidth, frameHeight;
            {
                std::lock_guard lock(m_lock);
                frame.swap(m_frame);
                frameWidth = m_frameWidth;
                frameHeight = m_frameHeight;
            }
            if (!frame.empty())
                m_graphics.DrawGamePixels(frame.data(), frameWidth, frameHeight, frameWidth * 4);

            std::deque<std::shared_ptr<Job>> jobs;
            {
                std::lock_guard lock(m_lock);
                jobs.swap(m_jobs);
            }
            for (auto const& job : jobs)
            {
                std::lock_guard lock(job->lock);
                if (!job->abandoned)
                    job->hr = Create(*job);
                job->finished = true;
                job->done.notify_all();
            }
        }

    private:
        struct Job
        {
            winrt::com_ptr<IUnknown> device;
            DXGI_SWAP_CHAIN_DESC1 desc{};
            winrt::com_ptr<IDXGISwapChain1> result;
            HRESULT hr = E_FAIL;
            std::mutex lock;
            std::condition_variable done;
            bool finished = false;
            bool abandoned = false;
        };

        HRESULT Create(Job& job)
        {
            // Only one swapchain can exist on the CoreWindow, so the shell lets go of its own first.
            if (!m_graphics.ReleaseForGame() && !m_graphics.HandedOver())
                return E_FAIL;
            auto dxgiDevice = job.device.as<IDXGIDevice>();
            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));
            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
            return factory->CreateSwapChainForCoreWindow(job.device.get(), winrt::get_unknown(m_graphics.Window()),
                &job.desc, nullptr, job.result.put());
        }

        Graphics& m_graphics;
        std::mutex m_lock;
        std::deque<std::shared_ptr<Job>> m_jobs;
        std::vector<uint32_t> m_frame; // newest software frame not yet drawn
        UINT m_frameWidth = 0, m_frameHeight = 0;
    };

    // Null where there is no CoreWindow, like the desktop test harness.
    winrt::Windows::UI::Core::CoreWindow CurrentCoreWindow()
    {
        try { return winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread(); }
        catch (...) { return nullptr; }
    }

    // Maps the game's exe through the Win32 layer and runs its entry point on
    // its own thread. Status text is built on demand because the thread
    // changes the state while the shell is drawing.
    class LoaderRuntime final : public IGameRuntime
    {
    public:
        ~LoaderRuntime() override
        {
            if (m_thread)
            {
                // Can't stop game code safely; keep its image mapped for good.
                if (State() == RunState::Running || m_leakImage)
                    (void)m_image.release();
                CloseHandle(m_thread);
            }
        }

        bool Start(GameEntry const& game, HostServices& host) override
        {
            if (State() == RunState::Running)
                return true;
            if (m_thread)
            {
                CloseHandle(m_thread);
                m_thread = nullptr;
            }
            m_name = game.name;
            m_input = &host.input;
            SetGameAudio(host.audio.Engine()); // null when the console has no audio, which waveOut reports as no driver
            m_lastTick = std::chrono::steady_clock::now();
            m_screen = std::make_unique<GraphicsDisplay>(host.graphics);
            SetGameDisplay(m_screen.get());
            AttachGameWindow(CurrentCoreWindow());
            if (m_leakImage)
                (void)m_image.release();
            m_leakImage = false;
            m_image.reset();
            SetState(RunState::Idle);

            fs::path exe;
            if (!PrepareFolder(game, host, exe))
                return Fail(L"can't copy the game to LocalState");
            m_profile = ReadInputProfile(exe.parent_path());
            if (m_profile == InputProfile::Pointer)
                HOST_LOG(L"controller: pointer profile (sticks move the pointer, A/B click)");

            auto image = std::make_unique<PeImage>();
            std::wstring error;
            if (!image->Load(exe, m_win32, error))
                return Fail(error);
            m_win32.LogUnresolved();
            if (!image->EntryPoint() || image->IsDll())
                return Fail(L"not an exe with an entry point");
            if (!InstallGameTls(*image, error))
                return Fail(error);
            m_image = std::move(image);

            SetState(RunState::Running);
            // 8 MB: the app partition has no STACK_SIZE_PARAM_IS_RESERVATION, so this is committed up front.
            m_thread = CreateThread(nullptr, 8 * 1024 * 1024, &LoaderRuntime::ThreadMain, this, 0, nullptr);
            if (!m_thread)
            {
                SetState(RunState::Idle);
                return Fail(L"can't start the game thread");
            }
            return true;
        }

        // The game draws on its own thread; this only runs what it needs from the UI thread.
        void Tick() override
        {
            if (m_screen) m_screen->Pump();
            if (!m_input) return;
            auto now = std::chrono::steady_clock::now();
            float seconds = std::min(0.1f, std::chrono::duration<float>(now - m_lastTick).count());
            m_lastTick = now;
            FeedGameInput(MapInput(m_input->KeyboardMouse(), m_input->Pad(), seconds, m_profile));
        }
        void Stop() override { DetachGameWindow(); }
        bool IsRunning() const override { return State() == RunState::Running; }

        std::wstring const& LastMessage() const override
        {
            std::lock_guard lock(m_lock);
            switch (m_state)
            {
            case RunState::Running: m_display = m_name + L" running"; break;
            case RunState::Exited: m_display = m_name + L" exited with code " + std::to_wstring(static_cast<int>(m_exitCode)); break;
            case RunState::Crashed: m_display = m_name + L" " + m_crashText; break;
            default: m_display = m_name + L": " + m_error; break;
            }
            return m_display;
        }

    private:
        enum class RunState { Idle, Running, Exited, Crashed };

        RunState State() const
        {
            std::lock_guard lock(m_lock);
            return m_state;
        }

        void SetState(RunState state)
        {
            std::lock_guard lock(m_lock);
            m_state = state;
        }

        bool Fail(std::wstring error)
        {
            {
                std::lock_guard lock(m_lock);
                m_error = std::move(error);
            }
            HOST_LOG(L"%s", LastMessage().c_str());
            return false;
        }

        static DWORD WINAPI ThreadMain(void* self)
        {
            static_cast<LoaderRuntime*>(self)->Run();
            return 0;
        }

        void Run()
        {
            uint8_t* base = m_image->Base();
            SetGameProcess(base, m_image->Path());
            HOST_LOG(L"game thread started at entry +0x%llx",
                static_cast<unsigned long long>(static_cast<uint8_t*>(m_image->EntryPoint()) - base));

            DWORD exitCode = 0;
            EXCEPTION_RECORD crash{};
            EnterGameThread(DLL_PROCESS_ATTACH);
            bool returned = CallEntry(m_image->EntryPoint(), exitCode, crash);

            if (m_screen) m_screen->GameEnded();
            if (size_t alive = GameThreadsAlive(200))
            {
                // No TerminateThread in the app partition, so these keep running; their code must stay mapped.
                HOST_LOG(L"%zu game thread(s) still running; keeping the image mapped", alive);
                m_leakImage = true;
            }

            // ExitProcess unwinds here with its code, which is a clean exit.
            if (!returned && crash.ExceptionCode == kGameExitException)
            {
                returned = true;
                exitCode = GameExitCode();
            }

            std::lock_guard lock(m_lock);
            if (returned)
            {
                m_exitCode = exitCode;
                m_state = RunState::Exited;
                HOST_LOG(L"exited with code %d", static_cast<int>(exitCode));
                return;
            }
            auto at = static_cast<uint8_t*>(crash.ExceptionAddress);
            wchar_t buf[200];
            swprintf_s(buf, L"crashed: 0x%08lx at +0x%llx (%s)", crash.ExceptionCode,
                static_cast<unsigned long long>(at - base),
                m_image->Contains(at) ? m_image->Path().filename().c_str() : L"outside image");
            m_crashText = buf;
            m_state = RunState::Crashed;
            HOST_LOG(L"%s", buf);
        }

        // The bundled folder is read-only and games write next to their exe,
        // so bundled games are copied into LocalState\Games once and run there.
        static bool PrepareFolder(GameEntry const& game, HostServices& host, fs::path& exe)
        {
            if (game.writable)
            {
                exe = game.exe;
                return true;
            }

            fs::path dest = host.library.UserGamesRoot() / game.name;
            exe = dest / game.exe.lexically_relative(game.folder); // fs::relative opens the read-only package folder and throws

            std::error_code ec;
            fs::create_directories(dest, ec);
            fs::copy(game.folder, dest, fs::copy_options::recursive | fs::copy_options::update_existing, ec);
            if (ec)
                HOST_LOG(L"copy to %s failed: %S", dest.c_str(), ec.message().c_str());
            return !ec;
        }

        std::unique_ptr<GraphicsDisplay> m_screen;
        Input* m_input = nullptr;
        InputProfile m_profile = InputProfile::Default;
        std::chrono::steady_clock::time_point m_lastTick;
        Win32Layer m_win32;
        std::unique_ptr<PeImage> m_image;
        HANDLE m_thread = nullptr;
        bool m_leakImage = false; // game threads outlived the game
        std::wstring m_name;

        mutable std::mutex m_lock;
        mutable std::wstring m_display;
        RunState m_state = RunState::Idle;
        DWORD m_exitCode = 0;
        std::wstring m_crashText;
        std::wstring m_error;
    };
}

std::unique_ptr<IGameRuntime> CreateGameRuntime()
{
    return std::make_unique<LoaderRuntime>();
}
