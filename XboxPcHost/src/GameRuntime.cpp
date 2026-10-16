#include "pch.h"
#include "GameRuntime.h"
#include "Log.h"
#include "loader/PeImage.h"
#include "loader/Win32Layer.h"

namespace fs = std::filesystem;

namespace
{
    // Maps the game's exe through the Win32 layer. Running it comes next.
    class LoaderRuntime final : public IGameRuntime
    {
    public:
        bool Start(GameEntry const& game, HostServices& host) override
        {
            m_message.clear();
            m_image.reset();

            fs::path exe;
            if (!PrepareFolder(game, host, exe))
                return Fail(game, L"can't copy the game to LocalState");

            auto image = std::make_unique<PeImage>();
            if (!image->Load(exe, m_win32, m_message))
                return Fail(game, m_message);
            m_win32.LogUnresolved();

            m_image = std::move(image);
            wchar_t buf[64];
            swprintf_s(buf, L"mapped at %p", m_image->Base());
            m_message = buf;
            HOST_LOG(L"%s: %s", game.name.c_str(), m_message.c_str());
            return false;
        }
        void Tick() override {}
        void Stop() override {}
        bool IsRunning() const override { return false; }
        std::wstring const& LastMessage() const override { return m_message; }

    private:
        bool Fail(GameEntry const& game, std::wstring message)
        {
            m_message = std::move(message);
            HOST_LOG(L"%s: %s", game.name.c_str(), m_message.c_str());
            return false;
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

        Win32Layer m_win32;
        std::unique_ptr<PeImage> m_image;
        std::wstring m_message;
    };
}

std::unique_ptr<IGameRuntime> CreateGameRuntime()
{
    return std::make_unique<LoaderRuntime>();
}
