#pragma once

// One frame of keyboard and mouse state as the game should see it, whether it
// came from real devices or was made from the controller (InputMapping.h).
struct GameInputFrame
{
    std::array<bool, 256> keys{};       // Win32 virtual-key codes
    std::array<bool, 3> mouseButtons{}; // left, right, middle
    float cursorDx = 0, cursorDy = 0;   // pixels moved this frame
    float wheel = 0;                    // notches this frame, positive away from the user
};

// Turns the change since the last frame into WM_KEY*, WM_*BUTTON*, WM_MOUSEMOVE and
// WM_MOUSEWHEEL messages and updates what GetKeyState and GetCursorPos report.
// Call once per frame from the UI thread while a game runs.
void FeedGameInput(GameInputFrame const& frame);
