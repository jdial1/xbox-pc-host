#include "pch.h"
#include "GameRuntime.h"
#include "Log.h"
#include "Graphics.h"
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
    class GraphicsDisplay final : public GameDisplay
    {
    public:
        explicit GraphicsDisplay(Graphics& graphics) : m_graphics(graphics) {}
        UINT Width() const override { return m_graphics.Width() ? m_graphics.Width() : 1920; }
        UINT Height() const override { return m_graphics.Height() ? m_graphics.Height() : 1080; }

    private:
        Graphics& m_graphics;
    };

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
                if (State() == RunState::Running)
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
            m_screen = std::make_unique<GraphicsDisplay>(host.graphics);
            SetGameDisplay(m_screen.get());
            m_image.reset();
            SetState(RunState::Idle);

            fs::path exe;
            if (!PrepareFolder(game, host, exe))
                return Fail(L"can't copy the game to LocalState");

            auto image = std::make_unique<PeImage>();
            std::wstring error;
            if (!image->Load(exe, m_win32, error))
                return Fail(error);
            m_win32.LogUnresolved();
            if (!image->EntryPoint() || image->IsDll())
                return Fail(L"not an exe with an entry point");
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

        // Nothing to draw until the game owns the swapchain.
        void Tick() override {}
        void Stop() override {}
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
            bool returned = CallEntry(m_image->EntryPoint(), exitCode, crash);

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
            exe = dest / fs::relative(game.exe, game.folder);

            std::error_code ec;
            if (fs::exists(exe, ec))
                return true;
            fs::create_directories(dest, ec);
            fs::copy(game.folder, dest, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            if (ec)
                HOST_LOG(L"copy to %s failed: %S", dest.c_str(), ec.message().c_str());
            return !ec;
        }

        std::unique_ptr<GraphicsDisplay> m_screen;
        Win32Layer m_win32;
        std::unique_ptr<PeImage> m_image;
        HANDLE m_thread = nullptr;
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
