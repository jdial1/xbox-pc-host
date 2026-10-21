#pragma once

// One frame of keyboard and mouse state as the game should see it, whether it
// came from real devices or was made from the controller (InputMapping.h).
struct GameInputFrame
{
    std::array<bool, 256> keys{};       // Win32 virtual-key codes
    std::array<bool, 3> mouseButtons{}; // left, right, middle
    float cursorDx = 0, cursorDy = 0;   // pixels moved this frame
    float wheel = 0;                    // notches this frame, positive away from the user
    bool cursorSave = false;            // remember where the pointer is (before this frame's moves)
    bool cursorRestore = false;         // put it back there (after this frame's button changes)

    // The controller as it is, for games that read it themselves through XInput.
    bool padConnected = false;
    float padLeftX = 0, padLeftY = 0, padRightX = 0, padRightY = 0; // -1..1, up is +Y
    float padLeftTrigger = 0, padRightTrigger = 0;                  // 0..1
    uint32_t padButtons = 0;                                        // Windows.Gaming.Input GamepadButtons bits
};

// Turns the change since the last frame into WM_KEY*, WM_*BUTTON*, WM_MOUSEMOVE and
// WM_MOUSEWHEEL messages and updates what GetKeyState and GetCursorPos report.
// Call once per frame from the UI thread while a game runs.
void FeedGameInput(GameInputFrame const& frame);

// True once the game has polled XInput (in the last few seconds). The host then stops turning
// the controller into keyboard and mouse input, which the game would otherwise get twice.
bool GameReadsXInput();
