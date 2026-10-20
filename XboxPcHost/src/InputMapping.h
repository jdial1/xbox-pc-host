#pragma once

#include "Input.h"
#include "loader/GameInput.h"

// How the controller drives a game. A game picks one with a host-input.txt next to its exe.
enum class InputProfile
{
    Default, // left stick WASD, right stick pointer, triggers click, A space
    Pointer, // "pointer": either stick moves the pointer, A/B click, d-pad arrows (mouse-driven games)
};
InputProfile ReadInputProfile(std::filesystem::path const& gameFolder);

// What the game gets to see of the input devices this frame.
// padIsNative: the game reads the controller itself (XInput), so it only gets the raw pad.
GameInputFrame MapInput(KeyboardMouseState const& kbm, PadState const& pad, float seconds,
    InputProfile profile = InputProfile::Default, bool padIsNative = false);
