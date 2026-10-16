#include "pch.h"
#include "InputMapping.h"

GameInputFrame MapInput(KeyboardMouseState const& kbm, PadState const&, float)
{
    GameInputFrame frame;
    frame.keys = kbm.keys;
    frame.mouseButtons = kbm.mouseButtons;
    frame.cursorDx = kbm.mouseDeltaX;
    frame.cursorDy = kbm.mouseDeltaY;
    frame.wheel = kbm.wheel;
    return frame;
}
