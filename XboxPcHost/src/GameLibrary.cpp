#include "pch.h"
#include "GameLibrary.h"
#include "Log.h"
#include <fstream>

namespace fs = std::filesystem;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Storage;

namespace
{
    bool IsExe(fs::path const& p)
    {
        return _wcsicmp(p.extension().c_str(), L".exe") == 0;
    }

    // Pick the main exe in a folder: host-exe.txt names it (for exes below the folder, like
    // bin\x64\Factorio.exe); otherwise skip obvious helpers and prefer the largest at the top.
    fs::path FindMainExe(fs::path const& folder)
    {
        std::error_code exists;
        if (std::wifstream named(folder / L"host-exe.txt"); named)
        {
            std::wstring relative;
            std::getline(named, relative);
            while (!relative.empty() && (relative.back() == L'\r' || relative.back() == L' ')) relative.pop_back();
            fs::path exe = (folder / relative).lexically_normal();
            if (!relative.empty() && fs::is_regular_file(exe, exists))
                return exe;
        }
        fs::path best;
        uintmax_t bestSize = 0;
        std::error_code ec;
        for (auto const& entry : fs::directory_iterator(folder, ec))
        {
            if (!entry.is_regular_file(ec) || !IsExe(entry.path())) continue;
            std::wstring stem = entry.path().stem().wstring();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
            if (stem.find(L"unins") != std::wstring::npos || stem.find(L"setup") != std::wstring::npos ||
                stem.find(L"crash") != std::wstring::npos || stem.find(L"redist") != std::wstring::npos)
                continue;
            uintmax_t size = entry.file_size(ec);
            if (best.empty() || size > bestSize)
            {
                best = entry.path();
                bestSize = size;
            }
        }
        return best;
    }
}

void GameLibrary::Initialize()
{
    m_userRoot = fs::path(ApplicationData::Current().LocalFolder().Path().c_str()) / L"Games";
    m_bundledRoot = fs::path(Package::Current().InstalledLocation().Path().c_str()) / L"Games";

    std::error_code ec;
    fs::create_directories(m_userRoot, ec);
    HOST_LOG(L"User games folder: %s", m_userRoot.c_str());
    HOST_LOG(L"Bundled games folder: %s", m_bundledRoot.c_str());
    Rescan();
}

void GameLibrary::Rescan()
{
    m_games.clear();
    ScanRoot(m_bundledRoot, false);
    ScanRoot(m_userRoot, true);
    HOST_LOG(L"Found %zu game(s)", m_games.size());
}

void GameLibrary::ScanRoot(fs::path const& root, bool writable)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;

    size_t first = m_games.size();
    for (auto const& entry : fs::directory_iterator(root, ec))
    {
        if (entry.is_directory(ec))
        {
            fs::path exe = FindMainExe(entry.path());
            if (exe.empty()) continue;
            m_games.push_back({ entry.path().filename().wstring(), entry.path(), exe, writable });
        }
        else if (entry.is_regular_file(ec) && IsExe(entry.path()))
        {
            // A loose exe dropped straight into Games\ counts too.
            m_games.push_back({ entry.path().stem().wstring(), root, entry.path(), writable });
        }
    }
    for (size_t i = first; i < m_games.size(); ++i)
    {
        auto const& g = m_games[i];
        HOST_LOG(L"  game: %s -> %s%s", g.name.c_str(), g.exe.c_str(), g.writable ? L"" : L" (read-only)");
    }
}
