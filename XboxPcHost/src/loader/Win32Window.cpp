#include "pch.h"
#include <map>
#include "loader/Win32Layer.h"
#include "loader/GameDisplay.h"
#include "loader/Win32Text.h"
#include "Log.h"

// user32's window calls for a game that thinks it owns one window. There is
// one fake HWND, and every size or metric query answers with the output size.
// The window-class and message structs are declared here because the app
// partition hides them; the layouts match the x64 Windows ones.
namespace
{
    using WndProc = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

    struct WndClassEx
    {
        UINT cbSize;
        UINT style;
        WndProc proc;
        int clsExtra, wndExtra;
        HINSTANCE instance;
        HICON icon;
        HCURSOR cursor;
        HBRUSH background;
        const void* menu;
        const void* name; // char* or wchar_t*, depending on which function was called
        HICON iconSmall;
    };
    static_assert(sizeof(WndClassEx) == 80);

    struct WndClass // RegisterClassA: the same without cbSize and iconSmall
    {
        UINT style;
        WndProc proc;
        int clsExtra, wndExtra;
        HINSTANCE instance;
        HICON icon;
        HCURSOR cursor;
        HBRUSH background;
        const void* menu;
        const void* name;
    };
    static_assert(sizeof(WndClass) == 72);

    struct WindowPlacement
    {
        UINT length, flags, showCmd;
        POINT minPosition, maxPosition;
        RECT normalPosition;
    };
    static_assert(sizeof(WindowPlacement) == 44);

    struct CreateStruct // CREATESTRUCTW
    {
        LPVOID createParams;
        HINSTANCE instance;
        HMENU menu;
        HWND parent;
        int cy, cx, y, x;
        LONG style;
        LPCWSTR name;
        LPCWSTR className;
        DWORD exStyle;
    };
    static_assert(sizeof(CreateStruct) == 80);

    struct MonitorInfo // MONITORINFOEXW; plain MONITORINFO stops before device
    {
        DWORD size;
        RECT monitor, work;
        DWORD flags;
        WCHAR device[32];
    };
    static_assert(sizeof(MonitorInfo) == 104);

    struct DevMode // DEVMODEW, display fields only
    {
        WCHAR deviceName[32];
        WORD specVersion, driverVersion, size, driverExtra;
        DWORD fields;
        BYTE unionPart[16];
        short color, duplex, yResolution, ttOption, collate;
        WCHAR formName[32];
        WORD logPixels;
        DWORD bitsPerPel, pelsWidth, pelsHeight, displayFlags, displayFrequency;
    };
    static_assert(offsetof(DevMode, bitsPerPel) == 168 && offsetof(DevMode, displayFrequency) == 184);

    constexpr int kSmCxScreen = 0, kSmCyScreen = 1, kSmCxFullScreen = 16, kSmCyFullScreen = 17,
        kSmCxVirtualScreen = 78, kSmCyVirtualScreen = 79;
    constexpr int kGwlStyle = -16, kGwlExStyle = -20, kGwlpUserData = -21;
    constexpr UINT kWsPopup = 0x80000000, kWsVisible = 0x10000000;

    // The only window: nothing else needs to tell windows apart.
    HWND const kHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x00010010));
    HDC const kDc = reinterpret_cast<HDC>(static_cast<uintptr_t>(0x00010020));
    HWND const kDesktop = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x00010011));
    HMONITOR const kMonitor = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x00010060));

    struct Msg
    {
        HWND hwnd;
        UINT message;
        WPARAM wParam;
        LPARAM lParam;
        DWORD time;
        POINT pt;
    };
    static_assert(sizeof(Msg) == 48);

    constexpr UINT kWmCreate = 0x0001, kWmSize = 0x0005, kWmActivate = 0x0006, kWmClose = 0x0010, kWmNcCreate = 0x0081,
        kWmChar = 0x0102;

    struct ClassEntry
    {
        std::wstring name; // lowercase
        WndProc proc;
    };

    std::mutex g_lock;
    GameDisplay* g_display = nullptr;
    std::vector<ClassEntry> g_classes; // atom = 0xC000 + index
    LONG g_style = kWsPopup | kWsVisible;
    std::map<int, LONG_PTR> g_longs; // the window's other GetWindowLong values (GWLP_USERDATA, ...)
    WndProc g_windowProc = nullptr; // the class the game made its window from

    // Messages from CoreWindow events and window creation, waiting for the game's message loop.
    std::deque<std::array<uintptr_t, 3>> g_queue;
    constexpr size_t kMaxQueued = 256; // a game that never pumps messages mustn't grow this forever

    winrt::event_token g_sizeToken, g_activatedToken, g_closedToken, g_charToken;
    winrt::Windows::UI::Core::CoreWindow g_coreWindow{ nullptr };

    UINT Width() { return g_display ? g_display->Width() : 1280; }
    UINT Height() { return g_display ? g_display->Height() : 720; }

    void Post(UINT message, WPARAM wParam, LPARAM lParam)
    {
        std::lock_guard lock(g_lock);
        if (g_queue.size() < kMaxQueued)
            g_queue.push_back({ message, wParam, static_cast<uintptr_t>(lParam) });
    }

    void PostSize(UINT width, UINT height)
    {
        Post(kWmSize, 0 /* SIZE_RESTORED */, MAKELPARAM(width, height));
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    ATOM Register(std::wstring name, WndProc proc)
    {
        std::lock_guard lock(g_lock);
        name = Lower(std::move(name));
        for (size_t i = 0; i < g_classes.size(); ++i)
            if (g_classes[i].name == name)
            {
                g_classes[i].proc = proc;
                return static_cast<ATOM>(0xC000 + i);
            }
        g_classes.push_back({ name, proc });
        return static_cast<ATOM>(0xC000 + g_classes.size() - 1);
    }

    // The class argument is either a name or an atom stuffed into the pointer.
    std::wstring ClassName(const void* cls, bool wide)
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(cls);
        if (v < 0x10000)
        {
            std::lock_guard lock(g_lock);
            size_t i = v - 0xC000;
            return (v >= 0xC000 && i < g_classes.size()) ? g_classes[i].name : L"(atom)";
        }
        return wide ? std::wstring(static_cast<const wchar_t*>(cls)) : Widen(static_cast<const char*>(cls));
    }

    ATOM WINAPI Shim_RegisterClassExW(const WndClassEx* c) { return Register(static_cast<const wchar_t*>(c->name), c->proc); }
    ATOM WINAPI Shim_RegisterClassExA(const WndClassEx* c) { return Register(Widen(static_cast<const char*>(c->name)), c->proc); }
    ATOM WINAPI Shim_RegisterClassA(const WndClass* c) { return Register(Widen(static_cast<const char*>(c->name)), c->proc); }
    ATOM WINAPI Shim_RegisterClassW(const WndClass* c) { return Register(static_cast<const wchar_t*>(c->name), c->proc); }

    HWND MakeWindow(const void* cls, bool wide, DWORD exStyle, DWORD style, HINSTANCE instance, LPVOID param)
    {
        std::wstring name = Lower(ClassName(cls, wide)); // class names are case-insensitive, and stored lowercase
        HOST_LOG(L"CreateWindowEx class=%s -> fake hwnd", name.c_str());
        WndProc proc = nullptr;
        {
            std::lock_guard lock(g_lock);
            for (auto const& c : g_classes)
                if (c.name == name)
                    proc = g_windowProc = c.proc;
            g_longs.clear();
        }
        // Before CreateWindowEx returns, the window procedure sees WM_NCCREATE and WM_CREATE on this
        // thread; games keep their per-window object from lpCreateParams.
        if (proc)
        {
            CreateStruct cs{ param, instance, nullptr, nullptr, static_cast<int>(Height()), static_cast<int>(Width()), 0, 0,
                static_cast<LONG>(style), nullptr, name.c_str(), exStyle };
            proc(kHwnd, kWmNcCreate, 0, reinterpret_cast<LPARAM>(&cs));
            proc(kHwnd, kWmCreate, 0, reinterpret_cast<LPARAM>(&cs));
        }
        // A real window announces its size and activation as it is created.
        PostSize(Width(), Height());
        Post(kWmActivate, 1 /* WA_ACTIVE */, 0);
        return kHwnd;
    }

    HWND WINAPI Shim_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR, DWORD style, int, int, int, int, HWND, HMENU,
        HINSTANCE instance, LPVOID param)
    {
        return MakeWindow(cls, true, exStyle, style, instance, param);
    }

    HWND WINAPI Shim_CreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR, DWORD style, int, int, int, int, HWND, HMENU,
        HINSTANCE instance, LPVOID param)
    {
        return MakeWindow(cls, false, exStyle, style, instance, param);
    }

    BOOL PeekMessage(Msg* msg, HWND, UINT min, UINT max, UINT flags)
    {
        std::lock_guard lock(g_lock);
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        {
            UINT message = static_cast<UINT>((*it)[0]);
            if (max != 0 && (message < min || message > max))
                continue;
            *msg = { kHwnd, message, (*it)[1], static_cast<LPARAM>((*it)[2]), GetTickCount(), {} };
            if (flags & 0x0001 /* PM_REMOVE */)
                g_queue.erase(it);
            return TRUE;
        }
        return FALSE;
    }

    BOOL WINAPI Shim_PeekMessageA(Msg* msg, HWND hwnd, UINT min, UINT max, UINT flags) { return PeekMessage(msg, hwnd, min, max, flags); }
    BOOL WINAPI Shim_PeekMessageW(Msg* msg, HWND hwnd, UINT min, UINT max, UINT flags) { return PeekMessage(msg, hwnd, min, max, flags); }

    // The game's own window procedure runs on the game's thread, like it would on Windows.
    LRESULT WINAPI Shim_DispatchMessage(const Msg* msg)
    {
        WndProc proc;
        {
            std::lock_guard lock(g_lock);
            proc = g_windowProc;
        }
        return proc ? proc(msg->hwnd, msg->message, msg->wParam, msg->lParam) : 0;
    }

    BOOL WINAPI Shim_TranslateMessage(const Msg*) { return FALSE; }

    // WM_NCCREATE has to answer TRUE or Windows would abandon the window; nothing else needs an answer.
    LRESULT WINAPI Shim_DefWindowProc(HWND, UINT message, WPARAM, LPARAM) { return message == kWmNcCreate; }
    BOOL WINAPI Shim_True() { return TRUE; }

    BOOL WINAPI Shim_GetClientRect(HWND, RECT* rect)
    {
        *rect = { 0, 0, static_cast<LONG>(Width()), static_cast<LONG>(Height()) };
        return TRUE;
    }

    // The window is the whole screen, so the frame adds nothing.
    BOOL WINAPI Shim_GetWindowRect(HWND hwnd, RECT* rect) { return Shim_GetClientRect(hwnd, rect); }
    BOOL WINAPI Shim_AdjustWindowRect(RECT*, DWORD, BOOL) { return TRUE; }
    BOOL WINAPI Shim_ClientToScreen(HWND, POINT*) { return TRUE; }

    BOOL WINAPI Shim_GetWindowPlacement(HWND, WindowPlacement* p)
    {
        *p = {};
        p->length = sizeof(*p);
        p->showCmd = 1; // SW_SHOWNORMAL
        p->normalPosition = { 0, 0, static_cast<LONG>(Width()), static_cast<LONG>(Height()) };
        return TRUE;
    }

    int WINAPI Shim_GetSystemMetrics(int index)
    {
        switch (index)
        {
        case kSmCxScreen: case kSmCxFullScreen: case kSmCxVirtualScreen: return static_cast<int>(Width());
        case kSmCyScreen: case kSmCyFullScreen: case kSmCyVirtualScreen: return static_cast<int>(Height());
        default: return 0;
        }
    }

    LONG_PTR WINAPI Shim_GetWindowLongPtr(HWND, int index)
    {
        std::lock_guard lock(g_lock);
        if (index == kGwlStyle)
            return g_style;
        auto it = g_longs.find(index);
        return it != g_longs.end() ? it->second : 0;
    }

    LONG_PTR WINAPI Shim_SetWindowLongPtr(HWND, int index, LONG_PTR value)
    {
        std::lock_guard lock(g_lock);
        LONG_PTR old;
        if (index == kGwlStyle)
        {
            old = g_style;
            g_style = static_cast<LONG>(value);
        }
        else
        {
            old = g_longs[index];
            g_longs[index] = value;
        }
        SetLastError(ERROR_SUCCESS); // 0 is a valid old value
        return old;
    }

    LONG WINAPI Shim_GetWindowLong(HWND hwnd, int index) { return static_cast<LONG>(Shim_GetWindowLongPtr(hwnd, index)); }
    LONG WINAPI Shim_SetWindowLong(HWND hwnd, int index, LONG value) { return static_cast<LONG>(Shim_SetWindowLongPtr(hwnd, index, value)); }

    RECT ScreenRect() { return { 0, 0, static_cast<LONG>(Width()), static_cast<LONG>(Height()) }; }

    // One monitor, the output.
    HMONITOR WINAPI Shim_MonitorFromWindow(HWND, DWORD) { return kMonitor; }

    BOOL WINAPI Shim_GetMonitorInfoW(HMONITOR, MonitorInfo* info)
    {
        if (!info || info->size < offsetof(MonitorInfo, device))
            return FALSE;
        info->monitor = info->work = ScreenRect();
        info->flags = 1; // MONITORINFOF_PRIMARY
        if (info->size >= sizeof(MonitorInfo))
            wcscpy_s(info->device, L"\\\\.\\DISPLAY1");
        return TRUE;
    }

    using MonitorEnumProc = BOOL(CALLBACK*)(HMONITOR, HDC, LPRECT, LPARAM);
    BOOL WINAPI Shim_EnumDisplayMonitors(HDC, LPCRECT, MonitorEnumProc proc, LPARAM data)
    {
        RECT r = ScreenRect();
        if (proc)
            proc(kMonitor, nullptr, &r, data);
        return TRUE;
    }

    // One display mode: the output's size, 32-bit, 60 Hz.
    BOOL WINAPI Shim_EnumDisplaySettingsW(LPCWSTR, DWORD mode, DevMode* dm)
    {
        if (!dm || (mode != 0 && mode != 0xFFFFFFFF /* ENUM_CURRENT_SETTINGS */ && mode != 0xFFFFFFFE /* ENUM_REGISTRY_SETTINGS */))
            return FALSE;
        dm->fields = 0x00040000 | 0x00080000 | 0x00100000 | 0x00400000; // bits per pel, width, height, frequency
        dm->bitsPerPel = 32;
        dm->pelsWidth = Width();
        dm->pelsHeight = Height();
        dm->displayFrequency = 60;
        return TRUE;
    }

    // Asking for the one mode there is succeeds; there is nothing to change.
    LONG WINAPI Shim_ChangeDisplaySettingsW(DevMode*, DWORD) { return 0; } // DISP_CHANGE_SUCCESSFUL

    BOOL WINAPI Shim_SetRect(RECT* r, int left, int top, int right, int bottom)
    {
        if (!r) return FALSE;
        *r = { left, top, right, bottom };
        return TRUE;
    }
    BOOL WINAPI Shim_SetRectEmpty(RECT* r) { return Shim_SetRect(r, 0, 0, 0, 0); }

    // The whole window is always up for repainting.
    BOOL WINAPI Shim_GetUpdateRect(HWND, RECT* r, BOOL)
    {
        if (r) *r = ScreenRect();
        return TRUE;
    }

    HWND WINAPI Shim_GetDesktopWindow() { return kDesktop; }
    HWND WINAPI Shim_SetCapture(HWND) { return nullptr; } // nobody had the mouse before
    HKL WINAPI Shim_GetKeyboardLayout(DWORD) { return reinterpret_cast<HKL>(static_cast<uintptr_t>(0x04090409)); } // en-US

    // Only the mapping games use to see whether a key types a character: letters, digits, space.
    UINT WINAPI Shim_MapVirtualKeyW(UINT code, UINT type)
    {
        if (type != 2 /* MAPVK_VK_TO_CHAR */)
            return 0;
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ')
            return code;
        return 0;
    }

    // There is no clipboard.
    BOOL WINAPI Shim_OpenClipboard(HWND) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    BOOL WINAPI Shim_IsClipboardFormatAvailable(UINT) { return FALSE; }
    HANDLE WINAPI Shim_GetClipboardData(UINT) { return nullptr; }

    // Dialogs have nobody to answer them; the game sees one that couldn't be created.
    INT_PTR WINAPI Shim_DialogBoxParamW(HINSTANCE, LPCWSTR, HWND, LPVOID, LPARAM)
    {
        HOST_LOG(L"DialogBoxParam: no dialogs on the console");
        return -1;
    }
    HWND WINAPI Shim_GetDlgItem(HWND, int) { return nullptr; }
    LRESULT WINAPI Shim_SendDlgItemMessageW(HWND, int, UINT, WPARAM, LPARAM) { return 0; }

    HWND WINAPI Shim_GetForegroundWindow() { return kHwnd; }
    HDC WINAPI Shim_GetDC(HWND) { return kDc; }
    int WINAPI Shim_ReleaseDC(HWND, HDC) { return 1; }
    int WINAPI Shim_ShowCursor(BOOL) { return 0; }
    BOOL WINAPI Shim_ShowWindow(HWND, int) { return FALSE; } // "was not visible before"

    // Non-null stand-ins for resources the game loads and never inspects.
    HANDLE WINAPI Shim_LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00010030)); }
    HANDLE WINAPI Shim_LoadIconW(HINSTANCE, LPCWSTR) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00010070)); }
    HANDLE WINAPI Shim_LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00010040)); }

    // There is nobody to click OK, so say what the game wanted to show and press it for them.
    int WINAPI Shim_MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT)
    {
        HOST_LOG(L"MessageBox [%S]: %S", title ? title : "", text ? text : "");
        return 1; // IDOK
    }

    int WINAPI Shim_MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT)
    {
        HOST_LOG(L"MessageBox [%s]: %s", title ? title : L"", text ? text : L"");
        return 1; // IDOK
    }
}

void AttachGameWindow(winrt::Windows::UI::Core::CoreWindow const& window)
{
    using namespace winrt::Windows::UI::Core;
    DetachGameWindow();
    if (!window)
        return;
    g_coreWindow = window;
    // These run on the UI thread; the game's thread picks the messages up from the queue.
    g_sizeToken = window.SizeChanged([](CoreWindow const&, WindowSizeChangedEventArgs const& args) {
        double scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
        PostSize(static_cast<UINT>(std::lround(args.Size().Width * scale)), static_cast<UINT>(std::lround(args.Size().Height * scale)));
    });
    g_activatedToken = window.Activated([](CoreWindow const&, WindowActivatedEventArgs const& args) {
        Post(kWmActivate, args.WindowActivationState() == CoreWindowActivationState::Deactivated ? 0 : 1, 0);
    });
    g_charToken = window.CharacterReceived([](CoreWindow const&, CharacterReceivedEventArgs const& args) {
        Post(kWmChar, args.KeyCode(), 1);
    });
    g_closedToken = window.Closed([](CoreWindow const&, CoreWindowEventArgs const&) { Post(kWmClose, 0, 0); });
}

void DetachGameWindow()
{
    if (g_coreWindow)
    {
        g_coreWindow.SizeChanged(g_sizeToken);
        g_coreWindow.Activated(g_activatedToken);
        g_coreWindow.Closed(g_closedToken);
        g_coreWindow.CharacterReceived(g_charToken);
        g_coreWindow = nullptr;
    }
    std::lock_guard lock(g_lock);
    g_queue.clear();
}

void PostGameMessage(UINT message, WPARAM wParam, LPARAM lParam) { Post(message, wParam, lParam); }
UINT GameScreenWidth() { return Width(); }
UINT GameScreenHeight() { return Height(); }
HDC GameWindowDc() { return kDc; }

void SetGameDisplay(GameDisplay* display)
{
    {
        std::lock_guard lock(g_lock);
        g_display = display;
    }
    SetGameDisplayForD3D(display);
    SetGameDisplayForGdi(display);
}

void RegisterWindow(Win32Layer& layer)
{
#define SHIM(fn) layer.Register("user32!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    SHIM(RegisterClassExA);
    SHIM(RegisterClassExW);
    SHIM(RegisterClassA);
    SHIM(RegisterClassW);
    SHIM(PeekMessageA);
    SHIM(PeekMessageW);
    SHIM(TranslateMessage);
    layer.Register("user32!DispatchMessageA", reinterpret_cast<void*>(&Shim_DispatchMessage));
    layer.Register("user32!DispatchMessageW", reinterpret_cast<void*>(&Shim_DispatchMessage));
    SHIM(CreateWindowExA);
    SHIM(CreateWindowExW);
    SHIM(GetClientRect);
    SHIM(GetWindowRect);
    SHIM(AdjustWindowRect);
    SHIM(ClientToScreen);
    SHIM(GetWindowPlacement);
    SHIM(GetSystemMetrics);
    for (const char* name : { "GetWindowLongA", "GetWindowLongW" })
        layer.Register((std::string("user32!") + name).c_str(), reinterpret_cast<void*>(&Shim_GetWindowLong));
    for (const char* name : { "SetWindowLongA", "SetWindowLongW" })
        layer.Register((std::string("user32!") + name).c_str(), reinterpret_cast<void*>(&Shim_SetWindowLong));
    for (const char* name : { "GetWindowLongPtrA", "GetWindowLongPtrW" })
        layer.Register((std::string("user32!") + name).c_str(), reinterpret_cast<void*>(&Shim_GetWindowLongPtr));
    for (const char* name : { "SetWindowLongPtrA", "SetWindowLongPtrW" })
        layer.Register((std::string("user32!") + name).c_str(), reinterpret_cast<void*>(&Shim_SetWindowLongPtr));
    SHIM(MonitorFromWindow);
    SHIM(GetMonitorInfoW);
    SHIM(EnumDisplayMonitors);
    SHIM(EnumDisplaySettingsW);
    SHIM(ChangeDisplaySettingsW);
    SHIM(SetRect);
    SHIM(SetRectEmpty);
    SHIM(GetUpdateRect);
    SHIM(GetDesktopWindow);
    SHIM(SetCapture);
    SHIM(GetKeyboardLayout);
    SHIM(MapVirtualKeyW);
    SHIM(OpenClipboard);
    SHIM(IsClipboardFormatAvailable);
    SHIM(GetClipboardData);
    SHIM(DialogBoxParamW);
    SHIM(GetDlgItem);
    SHIM(SendDlgItemMessageW);
    SHIM(LoadIconW);
    SHIM(MessageBoxW);
    layer.Register("user32!LoadCursorW", reinterpret_cast<void*>(&Shim_LoadCursorA));
    layer.Register("user32!GetWindowDC", reinterpret_cast<void*>(&Shim_GetDC));
    layer.Register("user32!GetActiveWindow", reinterpret_cast<void*>(&Shim_GetForegroundWindow));
    SHIM(GetForegroundWindow);
    SHIM(GetDC);
    SHIM(ReleaseDC);
    SHIM(ShowCursor);
    SHIM(ShowWindow);
    SHIM(LoadCursorA);
    SHIM(LoadImageA);
    SHIM(MessageBoxA);
#undef SHIM
    layer.Register("user32!DefWindowProcA", reinterpret_cast<void*>(&Shim_DefWindowProc));
    layer.Register("user32!DefWindowProcW", reinterpret_cast<void*>(&Shim_DefWindowProc));
    for (const char* name : { "BringWindowToTop", "ClipCursor", "DestroyWindow", "SetForegroundWindow",
        "SetWindowPos", "SetWindowTextA", "SetWindowTextW", "ValidateRect", "ReleaseCapture", "TrackMouseEvent",
        "CloseClipboard", "SetDlgItemTextW", "UpdateWindow", "InvalidateRect" })
        layer.Register((std::string("user32!") + name).c_str(), reinterpret_cast<void*>(&Shim_True));
}
