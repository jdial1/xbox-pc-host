#include "pch.h"
#include "InputMapping.h"
#include "ControllerMap.h"
#include <fstream>

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

    // Mouse-driven games: the pointer is the main control, so both sticks move it.
    constexpr ButtonKey kPointerKeys[] = {
        { GamepadButtons::DPadUp, VK_UP },
        { GamepadButtons::DPadDown, VK_DOWN },
        { GamepadButtons::DPadLeft, VK_LEFT },
        { GamepadButtons::DPadRight, VK_RIGHT },
        { GamepadButtons::Menu, VK_ESCAPE },
    };

    // A resting stick reads a little off center; without this the pointer creeps and every frame is a move.
    constexpr float kPointerDeadZone = 0.12f;
    float Stick(float v) { return std::abs(v) < kPointerDeadZone ? 0.0f : Curve(v); }

    void MapPointer(PadState const& pad, float seconds, GameInputFrame& frame)
    {
        // Stick up moves the cursor up, which is -Y on screen.
        frame.cursorDx += (Stick(pad.leftX) + Stick(pad.rightX)) * kCursorSpeed * seconds;
        frame.cursorDy -= (Stick(pad.leftY) + Stick(pad.rightY)) * kCursorSpeed * seconds;

        if (pad.Down(GamepadButtons::A) || pad.rightTrigger > kTriggerThreshold) frame.mouseButtons[0] = true;
        if (pad.Down(GamepadButtons::B) || pad.leftTrigger > kTriggerThreshold) frame.mouseButtons[1] = true;
        for (auto const& map : kPointerKeys)
            if (pad.Down(map.button))
                frame.keys[map.key] = true;

        if (pad.Pressed(GamepadButtons::LeftShoulder)) frame.wheel += 1;
        if (pad.Pressed(GamepadButtons::RightShoulder)) frame.wheel -= 1;
    }
}

InputProfile ReadInputProfile(std::filesystem::path const& gameFolder)
{
    if (LoadControllerMap(gameFolder / L"host-controls.txt"))
        return InputProfile::Custom;
    std::ifstream file(gameFolder / L"host-input.txt");
    std::string word;
    file >> word;
    std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return word == "pointer" ? InputProfile::Pointer : InputProfile::Default;
}

GameInputFrame MapInput(KeyboardMouseState const& kbm, PadState const& pad, float seconds, InputProfile profile,
    bool padIsNative)
{
    GameInputFrame frame;
    frame.keys = kbm.keys;
    frame.mouseButtons = kbm.mouseButtons;
    frame.cursorDx = kbm.mouseDeltaX;
    frame.cursorDy = kbm.mouseDeltaY;
    frame.wheel = kbm.wheel;
    frame.padConnected = pad.connected;
    frame.padLeftX = pad.leftX;
    frame.padLeftY = pad.leftY;
    frame.padRightX = pad.rightX;
    frame.padRightY = pad.rightY;
    frame.padLeftTrigger = pad.leftTrigger;
    frame.padRightTrigger = pad.rightTrigger;
    frame.padButtons = pad.buttons;
    if (!pad.connected || padIsNative)
        return frame;
    if (profile == InputProfile::Pointer)
    {
        MapPointer(pad, seconds, frame);
        return frame;
    }
    if (profile == InputProfile::Custom)
    {
        MapControllerFrame(pad, seconds, frame);
        return frame;
    }

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
