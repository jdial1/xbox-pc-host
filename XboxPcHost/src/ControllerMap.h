#pragma once

#include "Input.h"
#include "loader/GameInput.h"

// A game's own controller mapping, from host-controls.txt in its folder (the format is described at
// the top of ControllerMap.cpp). Returns false when there is no such file or nothing in it is usable.
bool LoadControllerMap(std::filesystem::path const& file);

// Turns this frame's pad into keys, mouse buttons, wheel and pointer movement by the loaded mapping.
void MapControllerFrame(PadState const& pad, float seconds, GameInputFrame& frame);
