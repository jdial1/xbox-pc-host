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
            if (g_padPacket <= 64) // enough to see a game read the first presses
                HOST_LOG(L"XInput user %lu: buttons 0x%04X left %d,%d", user, now.buttons, now.thumbLX, now.thumbLY);
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

namespace
{
    // A US keyboard's set-1 scan codes by virtual key; "extended" keys come with an E0 prefix.
    struct ScanCode
    {
        BYTE vk, scan;
        bool extended;
    };
    constexpr ScanCode kScanCodes[] = {
        { 0x1B, 0x01, false }, { '1', 0x02, false }, { '2', 0x03, false }, { '3', 0x04, false }, { '4', 0x05, false },
        { '5', 0x06, false }, { '6', 0x07, false }, { '7', 0x08, false }, { '8', 0x09, false }, { '9', 0x0A, false },
        { '0', 0x0B, false }, { 0xBD, 0x0C, false }, { 0xBB, 0x0D, false }, { 0x08, 0x0E, false }, { 0x09, 0x0F, false },
        { 'Q', 0x10, false }, { 'W', 0x11, false }, { 'E', 0x12, false }, { 'R', 0x13, false }, { 'T', 0x14, false },
        { 'Y', 0x15, false }, { 'U', 0x16, false }, { 'I', 0x17, false }, { 'O', 0x18, false }, { 'P', 0x19, false },
        { 0xDB, 0x1A, false }, { 0xDD, 0x1B, false }, { 0x0D, 0x1C, false }, { 0xA2, 0x1D, false }, { 0x11, 0x1D, false },
        { 'A', 0x1E, false }, { 'S', 0x1F, false }, { 'D', 0x20, false }, { 'F', 0x21, false }, { 'G', 0x22, false },
        { 'H', 0x23, false }, { 'J', 0x24, false }, { 'K', 0x25, false }, { 'L', 0x26, false }, { 0xBA, 0x27, false },
        { 0xDE, 0x28, false }, { 0xC0, 0x29, false }, { 0xA0, 0x2A, false }, { 0x10, 0x2A, false }, { 0xDC, 0x2B, false },
        { 'Z', 0x2C, false }, { 'X', 0x2D, false }, { 'C', 0x2E, false }, { 'V', 0x2F, false }, { 'B', 0x30, false },
        { 'N', 0x31, false }, { 'M', 0x32, false }, { 0xBC, 0x33, false }, { 0xBE, 0x34, false }, { 0xBF, 0x35, false },
        { 0xA1, 0x36, false }, { 0x6A, 0x37, false }, { 0xA4, 0x38, false }, { 0x12, 0x38, false }, { 0x20, 0x39, false },
        { 0x14, 0x3A, false }, { 0x70, 0x3B, false }, { 0x71, 0x3C, false }, { 0x72, 0x3D, false }, { 0x73, 0x3E, false },
        { 0x74, 0x3F, false }, { 0x75, 0x40, false }, { 0x76, 0x41, false }, { 0x77, 0x42, false }, { 0x78, 0x43, false },
        { 0x79, 0x44, false }, { 0x13, 0x45, false }, { 0x91, 0x46, false }, { 0x67, 0x47, false }, { 0x68, 0x48, false },
        { 0x69, 0x49, false }, { 0x6D, 0x4A, false }, { 0x64, 0x4B, false }, { 0x65, 0x4C, false }, { 0x66, 0x4D, false },
        { 0x6B, 0x4E, false }, { 0x61, 0x4F, false }, { 0x62, 0x50, false }, { 0x63, 0x51, false }, { 0x60, 0x52, false },
        { 0x6E, 0x53, false }, { 0xE2, 0x56, false }, { 0x7A, 0x57, false }, { 0x7B, 0x58, false },
        { 0xA3, 0x1D, true }, { 0xA5, 0x38, true }, { 0x6F, 0x35, true }, { 0x2C, 0x37, true }, { 0x90, 0x45, true },
        { 0x24, 0x47, true }, { 0x26, 0x48, true }, { 0x21, 0x49, true }, { 0x25, 0x4B, true }, { 0x27, 0x4D, true },
        { 0x23, 0x4F, true }, { 0x28, 0x50, true }, { 0x22, 0x51, true }, { 0x2D, 0x52, true }, { 0x2E, 0x53, true },
        { 0x5B, 0x5B, true }, { 0x5C, 0x5C, true }, { 0x5D, 0x5D, true },
    };
}

UINT VirtualKeyToScanCode(UINT vk, bool* extended)
{
    for (auto const& k : kScanCodes)
        if (k.vk == vk)
        {
            if (extended) *extended = k.extended;
            return k.scan;
        }
    if (extended) *extended = false;
    return 0;
}

// The first match is the generic key (VK_SHIFT) unless left and right are asked for.
UINT ScanCodeToVirtualKey(UINT scan, bool extended, bool leftRight)
{
    for (auto const& k : kScanCodes)
    {
        if (k.scan != scan || k.extended != extended)
            continue;
        bool sided = k.vk >= 0xA0 && k.vk <= 0xA5;
        bool generic = k.vk == 0x10 || k.vk == 0x11 || k.vk == 0x12;
        if ((leftRight && generic) || (!leftRight && sided))
            continue;
        return k.vk;
    }
    return 0;
}

LPARAM KeyMessageLParam(UINT vk, bool down)
{
    bool extended = false;
    UINT scan = VirtualKeyToScanCode(vk, &extended);
    LPARAM lParam = 1 | (static_cast<LPARAM>(scan) << 16) | (extended ? (1 << 24) : 0); // repeat count 1
    if (!down)
        lParam |= 0xC0000000; // was down, being released
    return lParam;
}

bool GameReadsXInput()
{
    ULONGLONG polled = g_xinputPolled;
    return polled != 0 && GetTickCount64() - polled < 3000;
}

namespace
{
    // WM_DEVICECHANGE's lParam for a device interface arriving or leaving: an XInput pad's HID
    // interface ("IG_" marks XInput devices). Posted, so it has to outlive the call.
    struct DeviceInterfaceBroadcast
    {
        DWORD size, deviceType, reserved;
        GUID classGuid;
        wchar_t name[128];
    };
    const DeviceInterfaceBroadcast kPadInterface = { sizeof(DeviceInterfaceBroadcast), 5 /* DBT_DEVTYP_DEVICEINTERFACE */, 0,
        { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } }, // GUID_DEVINTERFACE_HID
        L"\\\\?\\HID#VID_045E&PID_02FF&IG_00#0&0&0&0#{4d1e55b2-f16f-11cf-88cb-001111000030}" };
    bool g_padWasConnected = false;

    // Games that enumerate controllers (SDL) look again when Windows says devices changed.
    void AnnouncePad(bool connected)
    {
        constexpr UINT kWmDeviceChange = 0x0219;
        BroadcastGameMessage(kWmDeviceChange, connected ? 0x8000 /* DBT_DEVICEARRIVAL */ : 0x8004 /* DBT_DEVICEREMOVECOMPLETE */,
            reinterpret_cast<LPARAM>(&kPadInterface));
        BroadcastGameMessage(kWmDeviceChange, 0x0007 /* DBT_DEVNODES_CHANGED */, 0);
        HOST_LOG(L"controller %s", connected ? L"connected" : L"disconnected");
    }
}

void FeedGameInput(GameInputFrame const& frame)
{
    std::lock_guard lock(g_lock);
    if (frame.padConnected != g_padWasConnected)
    {
        g_padWasConnected = frame.padConnected;
        AnnouncePad(frame.padConnected);
    }
    g_pad = frame;
    PlaceCursor();

    for (int vk = 1; vk < 256; ++vk)
    {
        if (frame.keys[vk] == g_state.keys[vk])
            continue;
        g_state.keys[vk] = frame.keys[vk];
        // lParam: repeat count 1 and the key's scan code, which SDL reads the key from.
        PostGameMessage(frame.keys[vk] ? kWmKeyDown : kWmKeyUp, static_cast<WPARAM>(vk), KeyMessageLParam(vk, frame.keys[vk]));
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
