#pragma once

#include <functional>

#include "GameLibrary.h"

class Graphics;
class Input;
class Audio;

// Everything the host shell owns, handed to whatever runs a game.
struct HostServices
{
    Graphics& graphics;
    Input& input;
    Audio& audio;
    GameLibrary& library;
};

// The seam where stage 2 (PE loader) and stage 3 (Win32 call layer) plug in.
// The shell calls Start() when a game is picked, then Tick() once per frame
// while IsRunning(); the runtime draws into graphics.BackBuffer() and presents.
class IGameRuntime
{
public:
    virtual ~IGameRuntime() = default;
    virtual bool Start(GameEntry const& game, HostServices& host) = 0;
    virtual void Tick() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual std::wstring const& LastMessage() const = 0;
};

std::unique_ptr<IGameRuntime> CreateGameRuntime();

// For test harnesses: sees every software (GDI) frame the game presents, on the game's thread.
// Set it before Start(); the app never sets one.
using GameFrameTap = std::function<void(const uint32_t* bgra, UINT width, UINT height, UINT pitchBytes)>;
void SetGameFrameTap(GameFrameTap tap);
