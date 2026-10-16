#pragma once

#include "Input.h"
#include "loader/GameInput.h"

// What the game gets to see of the input devices this frame.
GameInputFrame MapInput(KeyboardMouseState const& kbm, PadState const& pad, float seconds);
