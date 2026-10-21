#include "pch.h"
#include "ControllerMap.h"
#include "Log.h"
#include <fstream>
#include <sstream>

// host-controls.txt: a game's controller mapping, one binding per line.
//
//   <control> = <action>          the control does this
//   <layer>+<control> = <action>  ... while a control bound to "layer" is held
//
// Controls: A B X Y LB RB LT RT View Menu LS RS (stick clicks) DPadUp DPadDown DPadLeft DPadRight,
//   LeftStick RightStick (the whole stick), LeftStickX/Y RightStickX/Y (one axis),
//   LeftStickUp/Down/Left/Right RightStickUp/Down/Left/Right (a stick pushed that way, like a button).
// Actions:
//   key <K>[+<K>...]     hold keys while the control is held (Ctrl+1, Shift, F10, Space, Up, ...)
//   mouse left|right|middle
//   wheel up|down        one notch on press, repeating while held
//   layer                hold for the second set of bindings
//   pointer [speed]      a stick moves the mouse pointer (pixels a second at full tilt, default 900)
//   arrows               a stick holds the arrow keys it points to (camera pan in most RTS games)
//   keys U D L R         a stick holds these four keys for up, down, left, right
//   drag left|right|middle [speed]   an axis or stick holds that button and moves the pointer while
//                        pushed, then lets go and puts the pointer back (camera rotation)
//   wheel [rate]         an axis scrolls the wheel, faster the further it's pushed (zoom)
//   none
// "#" starts a comment. Unknown names are logged and skipped.
namespace
{
    using GB = winrt::Windows::Gaming::Input::GamepadButtons;

    enum class Control
    {
        A, B, X, Y, LB, RB, LT, RT, View, Menu, LS, RS, DPadUp, DPadDown, DPadLeft, DPadRight,
        LeftStick, RightStick, LeftStickX, LeftStickY, RightStickX, RightStickY,
        LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
        RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
    };
    enum class Kind { None, Keys, Mouse, WheelStep, Layer, Pointer, Arrows, Drag, WheelAxis };

    struct Action
    {
        Kind kind = Kind::None;
        std::vector<uint8_t> keys; // Keys: held together; Arrows: up, down, left, right
        int button = 0;            // Mouse, Drag
        float value = 0;           // WheelStep: +1/-1; Pointer/Drag: speed; WheelAxis: notches a second
    };
    struct Binding
    {
        bool layered = false;
        Control control;
        Action action;
    };

    const std::pair<const char*, Control> kControls[] = {
        { "a", Control::A }, { "b", Control::B }, { "x", Control::X }, { "y", Control::Y },
        { "lb", Control::LB }, { "rb", Control::RB }, { "lt", Control::LT }, { "rt", Control::RT },
        { "view", Control::View }, { "back", Control::View }, { "menu", Control::Menu }, { "start", Control::Menu },
        { "ls", Control::LS }, { "rs", Control::RS },
        { "dpadup", Control::DPadUp }, { "dpaddown", Control::DPadDown }, { "dpadleft", Control::DPadLeft },
        { "dpadright", Control::DPadRight },
        { "leftstick", Control::LeftStick }, { "rightstick", Control::RightStick },
        { "leftstickx", Control::LeftStickX }, { "leftsticky", Control::LeftStickY },
        { "rightstickx", Control::RightStickX }, { "rightsticky", Control::RightStickY },
        { "leftstickup", Control::LeftStickUp }, { "leftstickdown", Control::LeftStickDown },
        { "leftstickleft", Control::LeftStickLeft }, { "leftstickright", Control::LeftStickRight },
        { "rightstickup", Control::RightStickUp }, { "rightstickdown", Control::RightStickDown },
        { "rightstickleft", Control::RightStickLeft }, { "rightstickright", Control::RightStickRight },
    };

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Key names: letters, digits, F1-F24, and the usual names (Unity-style "Alpha1" works for digits).
    int KeyCode(std::string n)
    {
        n = Lower(n);
        if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return std::toupper(n[0]);
        if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return n[0];
        if (n.rfind("alpha", 0) == 0 && n.size() == 6 && std::isdigit(static_cast<unsigned char>(n[5]))) return n[5];
        if (n.rfind("keypad", 0) == 0 && n.size() == 7 && std::isdigit(static_cast<unsigned char>(n[6]))) return VK_NUMPAD0 + (n[6] - '0');
        if (n.size() >= 2 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1])))
        {
            int f = std::atoi(n.c_str() + 1);
            if (f >= 1 && f <= 24) return VK_F1 + f - 1;
        }
        static const std::pair<const char*, int> kNames[] = {
            { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE }, { "space", VK_SPACE }, { "tab", VK_TAB }, { "enter", VK_RETURN },
            { "return", VK_RETURN }, { "backspace", VK_BACK }, { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT },
            { "right", VK_RIGHT }, { "uparrow", VK_UP }, { "downarrow", VK_DOWN }, { "leftarrow", VK_LEFT },
            { "rightarrow", VK_RIGHT }, { "shift", VK_SHIFT }, { "leftshift", VK_LSHIFT }, { "rightshift", VK_RSHIFT },
            { "ctrl", VK_CONTROL }, { "control", VK_CONTROL }, { "leftcontrol", VK_LCONTROL }, { "rightcontrol", VK_RCONTROL },
            { "alt", VK_MENU }, { "leftalt", VK_LMENU }, { "rightalt", VK_RMENU }, { "home", VK_HOME }, { "end", VK_END },
            { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "insert", VK_INSERT }, { "delete", VK_DELETE },
            { "backquote", VK_OEM_3 }, { "pause", VK_PAUSE }, { "minus", VK_OEM_MINUS }, { "equals", VK_OEM_PLUS },
            { "comma", VK_OEM_COMMA }, { "period", VK_OEM_PERIOD },
        };
        for (auto const& [name, vk] : kNames)
            if (n == name) return vk;
        return 0;
    }

    int MouseButton(std::string const& n)
    {
        std::string l = Lower(n);
        return l == "left" ? 0 : l == "right" ? 1 : l == "middle" ? 2 : -1;
    }

    bool ParseControl(std::string n, Control& out)
    {
        n = Lower(n);
        for (auto const& [name, c] : kControls)
            if (n == name) { out = c; return true; }
        return false;
    }

    std::vector<uint8_t> ParseKeys(std::string const& text, bool& ok)
    {
        std::vector<uint8_t> keys;
        std::stringstream parts(text);
        std::string part;
        ok = true;
        while (std::getline(parts, part, '+'))
        {
            int vk = KeyCode(part);
            if (!vk) ok = false;
            else keys.push_back(static_cast<uint8_t>(vk));
        }
        return keys;
    }

    bool ParseAction(std::istringstream& in, Action& a)
    {
        std::string word;
        if (!(in >> word)) return false;
        word = Lower(word);
        std::string arg;
        if (word == "none") { a.kind = Kind::None; return true; }
        if (word == "layer") { a.kind = Kind::Layer; return true; }
        if (word == "key")
        {
            bool ok = false;
            in >> arg;
            a.kind = Kind::Keys;
            a.keys = ParseKeys(arg, ok);
            return ok && !a.keys.empty();
        }
        if (word == "mouse")
        {
            in >> arg;
            a.kind = Kind::Mouse;
            a.button = MouseButton(arg);
            return a.button >= 0;
        }
        if (word == "pointer")
        {
            a.kind = Kind::Pointer;
            a.value = 900;
            in >> a.value;
            return true;
        }
        if (word == "arrows")
        {
            a.kind = Kind::Arrows;
            a.keys = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
            return true;
        }
        if (word == "keys")
        {
            a.kind = Kind::Arrows;
            for (int i = 0; i < 4; ++i)
            {
                if (!(in >> arg)) return false;
                int vk = KeyCode(arg);
                if (!vk) return false;
                a.keys.push_back(static_cast<uint8_t>(vk));
            }
            return true;
        }
        if (word == "drag")
        {
            in >> arg;
            a.kind = Kind::Drag;
            a.button = MouseButton(arg);
            a.value = 600;
            in >> a.value;
            return a.button >= 0;
        }
        if (word == "wheel")
        {
            if (in >> arg)
            {
                std::string l = Lower(arg);
                if (l == "up" || l == "down")
                {
                    a.kind = Kind::WheelStep;
                    a.value = l == "up" ? 1.0f : -1.0f;
                    return true;
                }
                a.kind = Kind::WheelAxis;
                a.value = static_cast<float>(std::atof(arg.c_str()));
                return a.value > 0;
            }
            a.kind = Kind::WheelAxis;
            a.value = 12;
            return true;
        }
        return false;
    }

    // ---- The loaded mapping, and what it remembers between frames
    std::vector<Binding> g_bindings;
    float g_wheelCarry = 0;           // wheel notches not yet sent
    std::map<int, float> g_repeat;    // per binding index: seconds until a held wheel step repeats
    std::map<int, bool> g_wasActive;

    constexpr float kDeadZone = 0.15f, kPress = 0.5f;

    // 1 for any part of the left stick, 2 for the right, 0 for everything else.
    int StickOf(Control c)
    {
        switch (c)
        {
        case Control::LeftStick: case Control::LeftStickX: case Control::LeftStickY: case Control::LeftStickUp:
        case Control::LeftStickDown: case Control::LeftStickLeft: case Control::LeftStickRight: return 1;
        case Control::RightStick: case Control::RightStickX: case Control::RightStickY: case Control::RightStickUp:
        case Control::RightStickDown: case Control::RightStickLeft: case Control::RightStickRight: return 2;
        default: return 0;
        }
    }

    float Curve(float v) { return std::abs(v) < kDeadZone ? 0.0f : v * std::abs(v); }

    // A control's value: buttons 0 or 1, triggers 0..1, axes -1..1 (up and right positive).
    void Read(Control c, PadState const& p, float& x, float& y, bool& down)
    {
        x = y = 0;
        auto button = [&](GB b) { down = p.Down(b); x = down ? 1.0f : 0.0f; };
        switch (c)
        {
        case Control::A: button(GB::A); return;
        case Control::B: button(GB::B); return;
        case Control::X: button(GB::X); return;
        case Control::Y: button(GB::Y); return;
        case Control::LB: button(GB::LeftShoulder); return;
        case Control::RB: button(GB::RightShoulder); return;
        case Control::View: button(GB::View); return;
        case Control::Menu: button(GB::Menu); return;
        case Control::LS: button(GB::LeftThumbstick); return;
        case Control::RS: button(GB::RightThumbstick); return;
        case Control::DPadUp: button(GB::DPadUp); return;
        case Control::DPadDown: button(GB::DPadDown); return;
        case Control::DPadLeft: button(GB::DPadLeft); return;
        case Control::DPadRight: button(GB::DPadRight); return;
        case Control::LT: x = p.leftTrigger; down = x > kPress; return;
        case Control::RT: x = p.rightTrigger; down = x > kPress; return;
        case Control::LeftStick: x = p.leftX; y = p.leftY; break;
        case Control::RightStick: x = p.rightX; y = p.rightY; break;
        case Control::LeftStickX: x = p.leftX; break;
        case Control::LeftStickY: y = p.leftY; break;
        case Control::RightStickX: x = p.rightX; break;
        case Control::RightStickY: y = p.rightY; break;
        case Control::LeftStickUp: down = p.leftY > kPress; break;
        case Control::LeftStickDown: down = p.leftY < -kPress; break;
        case Control::LeftStickLeft: down = p.leftX < -kPress; break;
        case Control::LeftStickRight: down = p.leftX > kPress; break;
        case Control::RightStickUp: down = p.rightY > kPress; break;
        case Control::RightStickDown: down = p.rightY < -kPress; break;
        case Control::RightStickLeft: down = p.rightX < -kPress; break;
        case Control::RightStickRight: down = p.rightX > kPress; break;
        }
        if (c >= Control::LeftStick && c <= Control::RightStickY)
            down = std::abs(x) > kDeadZone || std::abs(y) > kDeadZone;
    }
}

bool LoadControllerMap(std::filesystem::path const& file)
{
    g_bindings.clear();
    g_wheelCarry = 0;
    g_repeat.clear();
    g_wasActive.clear();
    std::ifstream in(file);
    if (!in)
        return false;
    std::string line;
    int n = 0;
    while (std::getline(in, line))
    {
        ++n;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string left = line.substr(0, eq);
        left.erase(std::remove_if(left.begin(), left.end(), [](unsigned char c) { return std::isspace(c); }), left.end());
        if (left.empty()) continue;
        Binding b;
        // "LT+X": the part before "+" names the layer control, which must itself be bound to "layer".
        if (auto plus = left.find('+'); plus != std::string::npos)
        {
            b.layered = true;
            left = left.substr(plus + 1);
        }
        std::istringstream right(line.substr(eq + 1));
        if (!ParseControl(left, b.control) || !ParseAction(right, b.action))
        {
            HOST_LOG(L"host-controls.txt line %d not understood: %S", n, line.c_str());
            continue;
        }
        g_bindings.push_back(b);
    }
    HOST_LOG(L"controller: %zu bindings from %s", g_bindings.size(), file.filename().c_str());
    return !g_bindings.empty();
}

void MapControllerFrame(PadState const& pad, float seconds, GameInputFrame& frame)
{
    // Is a layer held? Layered bindings then replace the plain ones for the same control.
    bool layer = false;
    for (auto const& b : g_bindings)
    {
        float x, y;
        bool down = false;
        Read(b.control, pad, x, y, down);
        if (b.action.kind == Kind::Layer && !b.layered && down) layer = true;
    }
    // While a layer is held, its bindings replace the plain ones for the same control, and for any
    // part of the same stick (LT+RightStickUp silences RightStickX, so the camera doesn't turn too).
    std::set<Control> overridden;
    std::set<int> sticks;
    if (layer)
        for (auto const& b : g_bindings)
            if (b.layered)
            {
                overridden.insert(b.control);
                if (int s = StickOf(b.control)) sticks.insert(s);
            }

    for (int i = 0; i < static_cast<int>(g_bindings.size()); ++i)
    {
        auto const& b = g_bindings[i];
        bool applies = b.layered ? layer : !(layer && (overridden.count(b.control) || sticks.count(StickOf(b.control))));
        float x = 0, y = 0;
        bool down = false;
        Read(b.control, pad, x, y, down);
        bool active = applies && down;
        bool was = g_wasActive[i];
        g_wasActive[i] = active;
        Action const& a = b.action;
        switch (a.kind)
        {
        case Kind::Keys:
            if (active) for (uint8_t vk : a.keys) frame.keys[vk] = true;
            break;
        case Kind::Mouse:
            if (active) frame.mouseButtons[a.button] = true;
            break;
        case Kind::WheelStep:
            if (active && !was) { frame.wheel += a.value; g_repeat[i] = 0.35f; }
            else if (active && (g_repeat[i] -= seconds) <= 0) { frame.wheel += a.value; g_repeat[i] = 0.12f; }
            break;
        case Kind::Pointer:
            if (applies)
            {
                frame.cursorDx += Curve(x) * a.value * seconds;
                frame.cursorDy -= Curve(y) * a.value * seconds;
            }
            break;
        case Kind::Arrows:
            if (applies)
            {
                if (y > kPress) frame.keys[a.keys[0]] = true;
                if (y < -kPress) frame.keys[a.keys[1]] = true;
                if (x < -kPress) frame.keys[a.keys[2]] = true;
                if (x > kPress) frame.keys[a.keys[3]] = true;
            }
            break;
        case Kind::Drag:
            if (active)
            {
                frame.mouseButtons[a.button] = true;
                if (!was)
                    frame.cursorSave = true; // the press goes out first, where the pointer is
                else
                {
                    frame.cursorDx += Curve(x) * a.value * seconds;
                    frame.cursorDy -= Curve(y) * a.value * seconds;
                }
            }
            else if (was)
                frame.cursorRestore = true; // the button comes up, then the pointer goes back
            break;
        case Kind::WheelAxis:
            if (applies)
            {
                g_wheelCarry += Curve(x + y) * a.value * seconds;
                while (g_wheelCarry >= 1) { frame.wheel += 1; g_wheelCarry -= 1; }
                while (g_wheelCarry <= -1) { frame.wheel -= 1; g_wheelCarry += 1; }
            }
            break;
        case Kind::Layer:
        case Kind::None:
            break;
        }
    }
}
