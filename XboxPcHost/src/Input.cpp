#include "pch.h"
#include "Input.h"
#include "Log.h"

using namespace winrt;
using namespace winrt::Windows::Devices::Input;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::UI::Core;

namespace
{
    float Deadzone(double v)
    {
        constexpr double dz = 0.15;
        if (std::abs(v) < dz) return 0.0f;
        return static_cast<float>((v - std::copysign(dz, v)) / (1.0 - dz));
    }
}

void Input::Initialize(CoreWindow const& window)
{
    {
        std::lock_guard guard(m_lock);
        for (auto const& pad : Gamepad::Gamepads()) m_gamepads.push_back(pad);
    }
    Gamepad::GamepadAdded([this](auto&&, Gamepad const& pad)
    {
        std::lock_guard guard(m_lock);
        if (std::find(m_gamepads.begin(), m_gamepads.end(), pad) == m_gamepads.end())
            m_gamepads.push_back(pad);
        HOST_LOG(L"Gamepad connected (%zu total)", m_gamepads.size());
    });
    Gamepad::GamepadRemoved([this](auto&&, Gamepad const& pad)
    {
        std::lock_guard guard(m_lock);
        auto it = std::find(m_gamepads.begin(), m_gamepads.end(), pad);
        if (it != m_gamepads.end()) m_gamepads.erase(it);
        HOST_LOG(L"Gamepad disconnected (%zu total)", m_gamepads.size());
    });

    window.KeyDown([this](auto&&, KeyEventArgs const& args)
    {
        auto vk = static_cast<uint32_t>(args.VirtualKey());
        if (vk < m_kbm.keys.size()) m_kbm.keys[vk] = true;
    });
    window.KeyUp([this](auto&&, KeyEventArgs const& args)
    {
        auto vk = static_cast<uint32_t>(args.VirtualKey());
        if (vk < m_kbm.keys.size()) m_kbm.keys[vk] = false;
    });
    auto updatePointer = [this](PointerEventArgs const& args)
    {
        auto point = args.CurrentPoint();
        auto p = point.Position();
        m_kbm.pointerX = p.X;
        m_kbm.pointerY = p.Y;
        auto props = point.Properties();
        m_kbm.mouseButtons = { props.IsLeftButtonPressed(), props.IsRightButtonPressed(), props.IsMiddleButtonPressed() };
    };
    window.PointerMoved([updatePointer](auto&&, PointerEventArgs const& args) { updatePointer(args); });
    window.PointerPressed([updatePointer](auto&&, PointerEventArgs const& args) { updatePointer(args); });
    window.PointerReleased([updatePointer](auto&&, PointerEventArgs const& args) { updatePointer(args); });
    window.PointerWheelChanged([this](auto&&, PointerEventArgs const& args)
    {
        m_pendingWheel += args.CurrentPoint().Properties().MouseWheelDelta() / 120.0f; // 120 units per notch
    });
    // Relative motion for mouse-look games; raised on the UI thread.
    MouseDevice::GetForCurrentView().MouseMoved([this](auto&&, MouseEventArgs const& args)
    {
        m_pendingDx += static_cast<float>(args.MouseDelta().X);
        m_pendingDy += static_cast<float>(args.MouseDelta().Y);
    });
}

void Input::Update()
{
    Gamepad pad{ nullptr };
    {
        std::lock_guard guard(m_lock);
        if (!m_gamepads.empty()) pad = m_gamepads.front();
    }

    uint32_t previous = m_pad.buttons;
    if (pad)
    {
        GamepadReading r = pad.GetCurrentReading();
        m_pad.connected = true;
        m_pad.leftX = Deadzone(r.LeftThumbstickX);
        m_pad.leftY = Deadzone(r.LeftThumbstickY);
        m_pad.rightX = Deadzone(r.RightThumbstickX);
        m_pad.rightY = Deadzone(r.RightThumbstickY);
        m_pad.leftTrigger = static_cast<float>(r.LeftTrigger);
        m_pad.rightTrigger = static_cast<float>(r.RightTrigger);
        m_pad.buttons = static_cast<uint32_t>(r.Buttons);
    }
    else
    {
        m_pad = PadState{};
    }
    m_pad.pressed = m_pad.buttons & ~previous;

    m_kbm.mouseDeltaX = m_pendingDx;
    m_kbm.mouseDeltaY = m_pendingDy;
    m_pendingDx = m_pendingDy = 0;
    m_kbm.wheel = m_pendingWheel;
    m_pendingWheel = 0;
}
