#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/GameInput.h"
#include "loader/GameDisplay.h"
#include "Log.h"

// Keyboard and mouse as the game sees them: messages for changes, GetKeyState
// for the current state, and a virtual cursor for GetCursorPos/SetCursorPos.
// Games that capture the mouse re-centre the cursor every frame and read how
// far it moved, so SetCursorPos just moves the virtual cursor.
namespace
{
    constexpr UINT kWmKeyDown = 0x0100, kWmKeyUp = 0x0101, kWmMouseMove = 0x0200, kWmLButtonDown = 0x0201,
        kWmLButtonUp = 0x0202, kWmRButtonDown = 0x0204, kWmRButtonUp = 0x0205, kWmMButtonDown = 0x0207,
        kWmMButtonUp = 0x0208, kWmMouseWheel = 0x020A;
    constexpr WPARAM kMkLButton = 0x0001, kMkRButton = 0x0002, kMkMButton = 0x0010;
    constexpr int kVkLButton = 0x01, kVkRButton = 0x02, kVkMButton = 0x04, kVkShift = 0x10, kVkControl = 0x11,
        kVkMenu = 0x12, kVkLShift = 0xA0, kVkRShift = 0xA1, kVkLControl = 0xA2, kVkRControl = 0xA3,
        kVkLMenu = 0xA4, kVkRMenu = 0xA5;

    std::mutex g_lock;
    GameInputFrame g_state; // what is down now
    float g_cursorX = 0, g_cursorY = 0;
    bool g_cursorPlaced = false;

    LONG ClampTo(float v, UINT size) { return std::clamp(static_cast<LONG>(v), 0L, static_cast<LONG>(size) - 1); }

    // The cursor starts in the middle of the screen, once we know how big it is.
    void PlaceCursor()
    {
        if (g_cursorPlaced)
            return;
        g_cursorX = GameScreenWidth() / 2.0f;
        g_cursorY = GameScreenHeight() / 2.0f;
        g_cursorPlaced = true;
    }

    WPARAM ButtonFlags()
    {
        return (g_state.mouseButtons[0] ? kMkLButton : 0) | (g_state.mouseButtons[1] ? kMkRButton : 0) |
            (g_state.mouseButtons[2] ? kMkMButton : 0);
    }

    LPARAM CursorLParam()
    {
        return MAKELPARAM(ClampTo(g_cursorX, GameScreenWidth()), ClampTo(g_cursorY, GameScreenHeight()));
    }

    bool KeyDown(int vk)
    {
        auto const& k = g_state.keys;
        switch (vk)
        {
        case kVkLButton: return g_state.mouseButtons[0];
        case kVkRButton: return g_state.mouseButtons[1];
        case kVkMButton: return g_state.mouseButtons[2];
        // The generic modifier keys are down when either side is.
        case kVkShift: return k[kVkShift] || k[kVkLShift] || k[kVkRShift];
        case kVkControl: return k[kVkControl] || k[kVkLControl] || k[kVkRControl];
        case kVkMenu: return k[kVkMenu] || k[kVkLMenu] || k[kVkRMenu];
        default: return vk > 0 && vk < 256 && k[vk];
        }
    }

    SHORT WINAPI Shim_GetKeyState(int vk)
    {
        std::lock_guard lock(g_lock);
        return KeyDown(vk) ? static_cast<SHORT>(0x8000) : 0;
    }

    BOOL WINAPI Shim_GetCursorPos(POINT* p)
    {
        std::lock_guard lock(g_lock);
        PlaceCursor();
        *p = { ClampTo(g_cursorX, GameScreenWidth()), ClampTo(g_cursorY, GameScreenHeight()) };
        return TRUE;
    }

    BOOL WINAPI Shim_SetCursorPos(int x, int y)
    {
        std::lock_guard lock(g_lock);
        g_cursorX = static_cast<float>(x);
        g_cursorY = static_cast<float>(y);
        g_cursorPlaced = true;
        return TRUE;
    }
}

void FeedGameInput(GameInputFrame const& frame)
{
    std::lock_guard lock(g_lock);
    PlaceCursor();

    for (int vk = 1; vk < 256; ++vk)
    {
        if (frame.keys[vk] == g_state.keys[vk])
            continue;
        g_state.keys[vk] = frame.keys[vk];
        // lParam: repeat count 1; the scan code and flags stay 0.
        PostGameMessage(frame.keys[vk] ? kWmKeyDown : kWmKeyUp, static_cast<WPARAM>(vk), frame.keys[vk] ? 1 : 0xC0000001);
    }

    if (frame.cursorDx != 0 || frame.cursorDy != 0)
    {
        g_cursorX = std::clamp(g_cursorX + frame.cursorDx, 0.0f, static_cast<float>(GameScreenWidth() - 1));
        g_cursorY = std::clamp(g_cursorY + frame.cursorDy, 0.0f, static_cast<float>(GameScreenHeight() - 1));
        PostGameMessage(kWmMouseMove, ButtonFlags(), CursorLParam());
    }

    static const UINT downMessages[3] = { kWmLButtonDown, kWmRButtonDown, kWmMButtonDown };
    static const UINT upMessages[3] = { kWmLButtonUp, kWmRButtonUp, kWmMButtonUp };
    for (int b = 0; b < 3; ++b)
    {
        if (frame.mouseButtons[b] == g_state.mouseButtons[b])
            continue;
        g_state.mouseButtons[b] = frame.mouseButtons[b];
        PostGameMessage(frame.mouseButtons[b] ? downMessages[b] : upMessages[b], ButtonFlags(), CursorLParam());
    }

    if (frame.wheel != 0)
    {
        // The high word of wParam is the distance in 120ths of a notch; the low word is button flags.
        auto distance = static_cast<short>(std::lround(frame.wheel * 120.0f));
        PostGameMessage(kWmMouseWheel, MAKEWPARAM(ButtonFlags(), distance), CursorLParam());
    }
}

void RegisterInput(Win32Layer& layer)
{
    layer.Register("user32!GetKeyState", reinterpret_cast<void*>(&Shim_GetKeyState));
    layer.Register("user32!GetCursorPos", reinterpret_cast<void*>(&Shim_GetCursorPos));
    layer.Register("user32!SetCursorPos", reinterpret_cast<void*>(&Shim_SetCursorPos));
}
