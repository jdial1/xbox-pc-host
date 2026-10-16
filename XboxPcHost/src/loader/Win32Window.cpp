#include "pch.h"
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

    constexpr int kSmCxScreen = 0, kSmCyScreen = 1, kSmCxFullScreen = 16, kSmCyFullScreen = 17,
        kSmCxVirtualScreen = 78, kSmCyVirtualScreen = 79;
    constexpr int kGwlStyle = -16;
    constexpr UINT kWsPopup = 0x80000000, kWsVisible = 0x10000000;

    // The only window: nothing else needs to tell windows apart.
    HWND const kHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x00010010));
    HDC const kDc = reinterpret_cast<HDC>(static_cast<uintptr_t>(0x00010020));

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

    constexpr UINT kWmSize = 0x0005, kWmActivate = 0x0006, kWmClose = 0x0010;

    struct ClassEntry
    {
        std::wstring name; // lowercase
        WndProc proc;
    };

    std::mutex g_lock;
    GameDisplay* g_display = nullptr;
    std::vector<ClassEntry> g_classes; // atom = 0xC000 + index
    LONG g_style = kWsPopup | kWsVisible;
    WndProc g_windowProc = nullptr; // the class the game made its window from

    // Messages from CoreWindow events and window creation, waiting for the game's message loop.
    std::deque<std::array<uintptr_t, 3>> g_queue;
    constexpr size_t kMaxQueued = 256; // a game that never pumps messages mustn't grow this forever

    winrt::event_token g_sizeToken, g_activatedToken, g_closedToken;
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

    HWND MakeWindow(const void* cls, bool wide)
    {
        std::wstring name = ClassName(cls, wide);
        HOST_LOG(L"CreateWindowEx class=%s -> fake hwnd", name.c_str());
        {
            std::lock_guard lock(g_lock);
            for (auto const& c : g_classes)
                if (c.name == name)
                    g_windowProc = c.proc;
        }
        // A real window announces its size and activation as it is created.
        PostSize(Width(), Height());
        Post(kWmActivate, 1 /* WA_ACTIVE */, 0);
        return kHwnd;
    }

    HWND WINAPI Shim_CreateWindowExW(DWORD, LPCWSTR cls, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
    {
        return MakeWindow(cls, true);
    }

    HWND WINAPI Shim_CreateWindowExA(DWORD, LPCSTR cls, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID)
    {
        return MakeWindow(cls, false);
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

    LRESULT WINAPI Shim_DefWindowProc(HWND, UINT, WPARAM, LPARAM) { return 0; }
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

    LONG WINAPI Shim_GetWindowLongA(HWND, int index) { return index == kGwlStyle ? g_style : 0; }

    LONG WINAPI Shim_SetWindowLongA(HWND, int index, LONG value)
    {
        if (index != kGwlStyle)
            return 0;
        LONG old = g_style;
        g_style = value;
        return old;
    }

    BOOL WINAPI Shim_GetCursorPos(POINT* p)
    {
        *p = { static_cast<LONG>(Width() / 2), static_cast<LONG>(Height() / 2) };
        return TRUE;
    }

    HWND WINAPI Shim_GetForegroundWindow() { return kHwnd; }
    HDC WINAPI Shim_GetDC(HWND) { return kDc; }
    int WINAPI Shim_ReleaseDC(HWND, HDC) { return 1; }
    int WINAPI Shim_ShowCursor(BOOL) { return 0; }
    BOOL WINAPI Shim_ShowWindow(HWND, int) { return FALSE; } // "was not visible before"

    // Non-null stand-ins for resources the game loads and never inspects.
    HANDLE WINAPI Shim_LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00010030)); }
    HANDLE WINAPI Shim_LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x00010040)); }

    // There is nobody to click OK, so say what the game wanted to show and press it for them.
    int WINAPI Shim_MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT)
    {
        HOST_LOG(L"MessageBox [%S]: %S", title ? title : "", text ? text : "");
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
    g_closedToken = window.Closed([](CoreWindow const&, CoreWindowEventArgs const&) { Post(kWmClose, 0, 0); });
}

void DetachGameWindow()
{
    if (g_coreWindow)
    {
        g_coreWindow.SizeChanged(g_sizeToken);
        g_coreWindow.Activated(g_activatedToken);
        g_coreWindow.Closed(g_closedToken);
        g_coreWindow = nullptr;
    }
    std::lock_guard lock(g_lock);
    g_queue.clear();
}

void SetGameDisplay(GameDisplay* display)
{
    {
        std::lock_guard lock(g_lock);
        g_display = display;
    }
    SetGameDisplayForD3D(display);
}

void RegisterWindow(Win32Layer& layer)
{
#define SHIM(fn) layer.Register("user32!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    SHIM(RegisterClassExA);
    SHIM(RegisterClassExW);
    SHIM(RegisterClassA);
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
    SHIM(GetWindowLongA);
    SHIM(SetWindowLongA);
    SHIM(GetCursorPos);
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
    for (const char* name : { "BringWindowToTop", "ClipCursor", "DestroyWindow", "SetCursorPos", "SetForegroundWindow",
        "SetWindowPos", "SetWindowTextA", "SetWindowTextW", "ValidateRect" })
        layer.Register((std::string("user32!") + name).c_str(), reinterpret_cast<void*>(&Shim_True));
}
