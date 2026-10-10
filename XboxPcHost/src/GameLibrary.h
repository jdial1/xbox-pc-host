#pragma once

struct GameEntry
{
    std::wstring name;            // folder name, shown in the list
    std::filesystem::path folder; // the game's root; becomes its working directory
    std::filesystem::path exe;    // the executable the loader will map
    bool writable = false;        // false for games bundled read-only in the package
};

// Finds games. A game is a folder holding an .exe (plus whatever DLLs and data
// it ships with). Two places are scanned:
//   <package>\Games\<name>\   bundled with the app at build time (read-only)
//   LocalState\Games\<name>\  copied on later via Device Portal (writable)
class GameLibrary
{
public:
    void Initialize();
    void Rescan();

    std::vector<GameEntry> const& Games() const { return m_games; }
    std::filesystem::path const& UserGamesRoot() const { return m_userRoot; }
    std::filesystem::path const& BundledGamesRoot() const { return m_bundledRoot; }

private:
    void ScanRoot(std::filesystem::path const& root, bool writable);

    std::filesystem::path m_userRoot;
    std::filesystem::path m_bundledRoot;
    std::vector<GameEntry> m_games;
};
