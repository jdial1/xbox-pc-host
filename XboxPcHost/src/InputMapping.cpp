#include "pch.h"
#include "InputMapping.h"

using winrt::Windows::Gaming::Input::GamepadButtons;

namespace
{
    // The one mapping every game gets until games bring their own.
    constexpr float kStickThreshold = 0.5f;  // how far a stick must go to count as a key
    constexpr float kTriggerThreshold = 0.5f;
    constexpr float kCursorSpeed = 900.0f;   // pixels per second with the right stick fully over

    struct ButtonKey
    {
        GamepadButtons button;
        uint8_t key;
    };
    constexpr ButtonKey kButtonKeys[] = {
        { GamepadButtons::A, VK_SPACE },
        { GamepadButtons::Menu, VK_ESCAPE },
    };

    // Squaring the stick value makes small movements fine and big ones fast.
    float Curve(float v) { return v * std::abs(v); }
}

GameInputFrame MapInput(KeyboardMouseState const& kbm, PadState const& pad, float seconds)
{
    GameInputFrame frame;
    frame.keys = kbm.keys;
    frame.mouseButtons = kbm.mouseButtons;
    frame.cursorDx = kbm.mouseDeltaX;
    frame.cursorDy = kbm.mouseDeltaY;
    frame.wheel = kbm.wheel;
    if (!pad.connected)
        return frame;

    // Left stick is WASD (stick up is +Y).
    if (pad.leftY > kStickThreshold) frame.keys['W'] = true;
    if (pad.leftY < -kStickThreshold) frame.keys['S'] = true;
    if (pad.leftX < -kStickThreshold) frame.keys['A'] = true;
    if (pad.leftX > kStickThreshold) frame.keys['D'] = true;

    for (auto const& map : kButtonKeys)
        if (pad.Down(map.button))
            frame.keys[map.key] = true;

    // Right stick is the mouse (stick up moves the cursor up, which is -Y on screen).
    frame.cursorDx += Curve(pad.rightX) * kCursorSpeed * seconds;
    frame.cursorDy -= Curve(pad.rightY) * kCursorSpeed * seconds;

    // Right trigger is the left button, left trigger the right button.
    if (pad.rightTrigger > kTriggerThreshold) frame.mouseButtons[0] = true;
    if (pad.leftTrigger > kTriggerThreshold) frame.mouseButtons[1] = true;

    // Bumpers step the wheel, which hotbar-style games use to change slot.
    if (pad.Pressed(GamepadButtons::LeftShoulder)) frame.wheel += 1;
    if (pad.Pressed(GamepadButtons::RightShoulder)) frame.wheel -= 1;
    return frame;
}
