#pragma once

// Snapshot of the first connected controller, with edge-detected presses.
struct PadState
{
    bool connected = false;
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    float leftTrigger = 0, rightTrigger = 0;
    uint32_t buttons = 0; // winrt::Windows::Gaming::Input::GamepadButtons bits
    uint32_t pressed = 0; // buttons that went down this frame

    bool Down(winrt::Windows::Gaming::Input::GamepadButtons b) const { return (buttons & static_cast<uint32_t>(b)) != 0; }
    bool Pressed(winrt::Windows::Gaming::Input::GamepadButtons b) const { return (pressed & static_cast<uint32_t>(b)) != 0; }
};

// Real keyboard/mouse, if one is plugged into the console. Stage 4 will also
// synthesize these from the controller for games that expect them.
struct KeyboardMouseState
{
    std::array<bool, 256> keys{}; // indexed by Win32 virtual-key code
    float mouseDeltaX = 0, mouseDeltaY = 0; // accumulated since last Update()
    float pointerX = 0, pointerY = 0;       // view pixels
    std::array<bool, 3> mouseButtons{};     // left, right, middle
    float wheel = 0;                        // notches since last Update(), positive away from the user
};

class Input
{
public:
    void Initialize(winrt::Windows::UI::Core::CoreWindow const& window);
    void Update(); // call once per frame

    PadState const& Pad() const { return m_pad; }
    KeyboardMouseState const& KeyboardMouse() const { return m_kbm; }

private:
    std::mutex m_lock; // gamepad add/remove events arrive on other threads
    std::vector<winrt::Windows::Gaming::Input::Gamepad> m_gamepads;
    PadState m_pad;
    KeyboardMouseState m_kbm;
    float m_pendingDx = 0, m_pendingDy = 0, m_pendingWheel = 0;
};
