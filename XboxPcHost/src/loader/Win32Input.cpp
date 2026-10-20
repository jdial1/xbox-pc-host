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

    // The high bit of each byte is "down"; toggle state (caps lock) isn't tracked.
    BOOL WINAPI Shim_GetKeyboardState(PBYTE keys)
    {
        if (!keys) return FALSE;
        std::lock_guard lock(g_lock);
        for (int vk = 0; vk < 256; ++vk)
            keys[vk] = KeyDown(vk) ? 0x80 : 0;
        return TRUE;
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

namespace
{
    // ------------------------------------------------------------------ XInput, from the host's pad
    // Layouts from xinput.h, which the app partition doesn't offer.
    struct XInputGamepad
    {
        WORD buttons;
        BYTE leftTrigger, rightTrigger;
        SHORT thumbLX, thumbLY, thumbRX, thumbRY;
    };
    struct XInputState
    {
        DWORD packetNumber;
        XInputGamepad gamepad;
    };
    struct XInputVibration
    {
        WORD leftMotorSpeed, rightMotorSpeed;
    };
    struct XInputCapabilities
    {
        BYTE type, subType;
        WORD flags;
        XInputGamepad gamepad;
        XInputVibration vibration;
    };
    struct XInputBattery
    {
        BYTE batteryType, batteryLevel;
    };
    static_assert(sizeof(XInputState) == 16 && sizeof(XInputCapabilities) == 20);

    GameInputFrame g_pad;              // the latest frame's raw pad (under g_lock)
    DWORD g_padPacket = 0;
    XInputGamepad g_lastReported{};
    std::atomic<ULONGLONG> g_xinputPolled{ 0 }; // GetTickCount64 of the game's last XInput poll

    SHORT Axis(float v) { return static_cast<SHORT>(std::lround(std::clamp(v, -1.0f, 1.0f) * (v < 0 ? 32768.0f : 32767.0f))); }
    BYTE Trigger(float v) { return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

    WORD XInputButtons(uint32_t b)
    {
        // Windows.Gaming.Input bit -> XINPUT_GAMEPAD_* bit
        static const std::pair<uint32_t, WORD> map[] = {
            { 0x0040, 0x0001 }, { 0x0080, 0x0002 }, { 0x0100, 0x0004 }, { 0x0200, 0x0008 }, // d-pad up/down/left/right
            { 0x0001, 0x0010 }, { 0x0002, 0x0020 },                                       // Menu = START, View = BACK
            { 0x1000, 0x0040 }, { 0x2000, 0x0080 },                                       // thumbsticks
            { 0x0400, 0x0100 }, { 0x0800, 0x0200 },                                       // shoulders
            { 0x0004, 0x1000 }, { 0x0008, 0x2000 }, { 0x0010, 0x4000 }, { 0x0020, 0x8000 }, // A B X Y
        };
        WORD w = 0;
        for (auto const& [from, to] : map)
            if (b & from) w |= to;
        return w;
    }

    constexpr DWORD kNotConnected = ERROR_DEVICE_NOT_CONNECTED;

    DWORD WINAPI Shim_XInputGetState(DWORD user, XInputState* state)
    {
        g_xinputPolled = GetTickCount64();
        std::lock_guard lock(g_lock);
        if (user != 0 || !g_pad.padConnected)
            return kNotConnected;
        XInputGamepad now{ XInputButtons(g_pad.padButtons), Trigger(g_pad.padLeftTrigger), Trigger(g_pad.padRightTrigger),
            Axis(g_pad.padLeftX), Axis(g_pad.padLeftY), Axis(g_pad.padRightX), Axis(g_pad.padRightY) };
        if (memcmp(&now, &g_lastReported, sizeof(now)) != 0)
        {
            g_lastReported = now;
            ++g_padPacket; // games skip work when the packet number hasn't moved
        }
        state->packetNumber = g_padPacket;
        state->gamepad = now;
        return ERROR_SUCCESS;
    }

    // Rumble isn't wired up yet; accepting it keeps games from treating the pad as broken.
    DWORD WINAPI Shim_XInputSetState(DWORD user, XInputVibration*)
    {
        std::lock_guard lock(g_lock);
        return user == 0 && g_pad.padConnected ? ERROR_SUCCESS : kNotConnected;
    }

    DWORD WINAPI Shim_XInputGetCapabilities(DWORD user, DWORD, XInputCapabilities* caps)
    {
        std::lock_guard lock(g_lock);
        if (user != 0 || !g_pad.padConnected)
            return kNotConnected;
        *caps = {};
        caps->type = 1;    // XINPUT_DEVTYPE_GAMEPAD
        caps->subType = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        caps->gamepad = { 0xF3FF, 255, 255, -64, -64, -64, -64 }; // every control present
        caps->vibration = { 65535, 65535 };
        return ERROR_SUCCESS;
    }

    DWORD WINAPI Shim_XInputGetBatteryInformation(DWORD user, BYTE, XInputBattery* battery)
    {
        std::lock_guard lock(g_lock);
        if (user != 0 || !g_pad.padConnected)
            return kNotConnected;
        *battery = { 1 /* BATTERY_TYPE_WIRED */, 3 /* BATTERY_LEVEL_FULL */ };
        return ERROR_SUCCESS;
    }

    VOID WINAPI Shim_XInputEnable(BOOL) {}
    DWORD WINAPI Shim_XInputGetKeystroke(DWORD, DWORD, void*) { return ERROR_EMPTY; }
}

bool GameReadsXInput()
{
    ULONGLONG polled = g_xinputPolled;
    return polled != 0 && GetTickCount64() - polled < 3000;
}

void FeedGameInput(GameInputFrame const& frame)
{
    std::lock_guard lock(g_lock);
    g_pad = frame;
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
    layer.Register("user32!GetKeyboardState", reinterpret_cast<void*>(&Shim_GetKeyboardState));
    // Every XInput DLL name games load: 1.4 (Windows 8+), 1.3 (DirectX SDK), 9.1.0 (Vista).
    for (const char* dll : { "xinput1_4", "xinput1_3", "xinput9_1_0", "xinputuap" })
    {
        std::string d = std::string(dll) + "!";
        layer.Register((d + "XInputGetState").c_str(), reinterpret_cast<void*>(&Shim_XInputGetState));
        layer.Register((d + "#100").c_str(), reinterpret_cast<void*>(&Shim_XInputGetState)); // XInputGetStateEx (adds Guide)
        layer.Register((d + "XInputSetState").c_str(), reinterpret_cast<void*>(&Shim_XInputSetState));
        layer.Register((d + "XInputGetCapabilities").c_str(), reinterpret_cast<void*>(&Shim_XInputGetCapabilities));
        layer.Register((d + "XInputGetBatteryInformation").c_str(), reinterpret_cast<void*>(&Shim_XInputGetBatteryInformation));
        layer.Register((d + "XInputEnable").c_str(), reinterpret_cast<void*>(&Shim_XInputEnable));
        layer.Register((d + "XInputGetKeystroke").c_str(), reinterpret_cast<void*>(&Shim_XInputGetKeystroke));
    }
    // Only the high "down now" bit is ever set, which is what both report.
    layer.Register("user32!GetAsyncKeyState", reinterpret_cast<void*>(&Shim_GetKeyState));
    layer.Register("user32!GetCursorPos", reinterpret_cast<void*>(&Shim_GetCursorPos));
    layer.Register("user32!SetCursorPos", reinterpret_cast<void*>(&Shim_SetCursorPos));
}
