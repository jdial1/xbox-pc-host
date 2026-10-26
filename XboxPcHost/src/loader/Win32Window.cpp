#include "pch.h"
#include <map>
#include "loader/Win32Layer.h"
#include "loader/GameDisplay.h"
#include "loader/Win32Text.h"
#include "Log.h"

// user32's window calls. A game gets real window handles of its own (SDL makes three: a helper,
// a message-only window and the game's), each with its class, window procedure, window longs and
// props, and one queue of messages that knows which window each is for. Every size or metric
// query answers with the output size, and the CoreWindow's events go to the game window: the
// newest window that isn't message-only. The structs are declared here because the app partition
// hides them; the layouts match the x64 Windows ones.
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

    template <typename C> struct MonitorInfo // MONITORINFOEXW / MONITORINFOEXA; plain MONITORINFO stops before device
    {
        DWORD size;
        RECT monitor, work;
        DWORD flags;
        C device[32];
    };
    static_assert(sizeof(MonitorInfo<wchar_t>) == 104 && sizeof(MonitorInfo<char>) == 72);

    template <typename C> struct DevMode // DEVMODEW / DEVMODEA, display fields only
    {
        C deviceName[32];
        WORD specVersion, driverVersion, size, driverExtra;
        DWORD fields;
        BYTE unionPart[16];
        short color, duplex, yResolution, ttOption, collate;
        C formName[32];
        WORD logPixels;
        DWORD bitsPerPel, pelsWidth, pelsHeight, displayFlags, displayFrequency;
    };
    static_assert(offsetof(DevMode<wchar_t>, bitsPerPel) == 168 && offsetof(DevMode<wchar_t>, displayFrequency) == 184);
    static_assert(offsetof(DevMode<char>, bitsPerPel) == 104);

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

    constexpr int kSmCxScreen = 0, kSmCyScreen = 1, kSmCxFullScreen = 16, kSmCyFullScreen = 17,
        kSmCxVirtualScreen = 78, kSmCyVirtualScreen = 79, kSmCMonitors = 80;
    constexpr int kGwlpWndProc = -4, kGwlStyle = -16, kGwlExStyle = -20;
    constexpr UINT kWsPopup = 0x80000000, kWsVisible = 0x10000000;
    constexpr UINT kWmCreate = 0x0001, kWmDestroy = 0x0002, kWmSize = 0x0005, kWmActivate = 0x0006, kWmSetFocus = 0x0007,
        kWmKillFocus = 0x0008, kWmClose = 0x0010, kWmActivateApp = 0x001C, kWmNcCreate = 0x0081, kWmNcDestroy = 0x0082,
        kWmChar = 0x0102;
    HWND const kHwndMessage = reinterpret_cast<HWND>(static_cast<intptr_t>(-3)); // HWND_MESSAGE

    HDC const kDc = reinterpret_cast<HDC>(static_cast<uintptr_t>(0x00010020));
    HWND const kDesktop = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x00010011));
    HMONITOR const kMonitor = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x00010060));

    struct ClassEntry
    {
        std::wstring name; // lowercase
        WndProc proc;
        WndClassEx info;   // as registered, for GetClassInfoEx
    };

    struct Window
    {
        std::wstring className;
        WndProc proc = nullptr;
        LONG style = static_cast<LONG>(kWsPopup | kWsVisible), exStyle = 0;
        HWND parent = nullptr;
        bool messageOnly = false;
        std::map<int, LONG_PTR> longs;          // GWLP_USERDATA and the rest
        std::map<std::wstring, HANDLE> props;   // atoms as "#n"
    };

    struct Queued
    {
        HWND hwnd; // null: a thread message
        UINT message;
        WPARAM wParam;
        LPARAM lParam;
    };

    std::recursive_mutex g_lock; // window procedures call back in (SetWindowLongPtr inside WM_CREATE)
    GameDisplay* g_display = nullptr;
    std::vector<ClassEntry> g_classes; // atom = 0xC000 + index
    std::map<HWND, Window> g_windows;
    HWND g_main = nullptr;             // the game window: CoreWindow events go here
    uintptr_t g_nextHwnd = 0x00010100;

    std::deque<Queued> g_queue;
    constexpr size_t kMaxQueued = 512; // a game that never pumps messages mustn't grow this forever

    winrt::event_token g_sizeToken, g_activatedToken, g_closedToken, g_charToken;
    winrt::Windows::UI::Core::CoreWindow g_coreWindow{ nullptr };

    UINT Width() { return g_display ? g_display->Width() : 1280; }
    UINT Height() { return g_display ? g_display->Height() : 720; }
    RECT ScreenRect() { return { 0, 0, static_cast<LONG>(Width()), static_cast<LONG>(Height()) }; }

    void PostTo(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        std::lock_guard lock(g_lock);
        if (g_queue.size() < kMaxQueued)
            g_queue.push_back({ hwnd, message, wParam, lParam });
    }

    // CoreWindow events and input are for the game window, whichever it is when they're dispatched.
    void Post(UINT message, WPARAM wParam, LPARAM lParam) { PostTo(nullptr, message, wParam, lParam); }

    void PostSize(UINT width, UINT height)
    {
        Post(kWmSize, 0 /* SIZE_RESTORED */, MAKELPARAM(width, height));
    }

    // What Windows sends a window as it gains or loses the foreground: SDL takes keyboard (and so
    // controller) focus from WM_SETFOCUS, and some games pause on WM_ACTIVATEAPP.
    void PostActivation(HWND hwnd, bool active)
    {
        if (active)
        {
            PostTo(hwnd, kWmActivateApp, TRUE, 0);
            PostTo(hwnd, kWmActivate, 1 /* WA_ACTIVE */, 0);
            PostTo(hwnd, kWmSetFocus, 0, 0);
        }
        else
        {
            PostTo(hwnd, kWmActivate, 0 /* WA_INACTIVE */, 0);
            PostTo(hwnd, kWmKillFocus, 0, 0);
            PostTo(hwnd, kWmActivateApp, FALSE, 0);
        }
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    // A window the game names, or the game window for a handle we never made (or null).
    Window* Find(HWND hwnd)
    {
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end()) return &it->second;
        it = g_windows.find(g_main);
        return it != g_windows.end() ? &it->second : nullptr;
    }

    // WH_CALLWNDPROC hooks (Mesa's OpenGL watches its windows this way), per thread or for all (thread 0).
    // CWPSTRUCT and HOOKPROC are desktop-only in the headers; the layouts are the same.
    struct CallWndProcParams { LPARAM lParam; WPARAM wParam; UINT message; HWND hwnd; };
    using CallWndProcHook = LRESULT(CALLBACK*)(int, WPARAM, LPARAM);
    struct Hook { HHOOK handle; CallWndProcHook proc; DWORD thread; };
    std::mutex g_hookLock;
    std::vector<Hook> g_hooks;
    std::atomic<uintptr_t> g_nextHook{ 0x00030000 };

    void CallHooks(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        std::vector<CallWndProcHook> procs;
        {
            std::lock_guard lock(g_hookLock);
            DWORD self = GetCurrentThreadId();
            for (auto const& h : g_hooks)
                if (!h.thread || h.thread == self) procs.push_back(h.proc);
        }
        CallWndProcParams params{ lParam, wParam, message, hwnd };
        for (auto proc : procs)
            proc(0 /* HC_ACTION */, 1 /* sent by this thread */, reinterpret_cast<LPARAM>(&params));
    }

    // Window procedures run on the calling thread, without the lock: they call back in freely.
    LRESULT Call(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        WndProc proc = nullptr;
        {
            std::lock_guard lock(g_lock);
            if (!hwnd) hwnd = g_main;
            auto it = g_windows.find(hwnd);
            if (it != g_windows.end()) proc = it->second.proc;
        }
        // The messages that end a game, so a quit can be traced to what asked for it.
        if (message == 0x0002 || message == 0x0010 || message == 0x0011 || message == 0x0016 ||
            (message == 0x0112 && (wParam & 0xFFF0) == 0xF060))
            HOST_LOG(L"window %p gets message 0x%04X (wParam 0x%llx)", static_cast<void*>(hwnd), message,
                static_cast<unsigned long long>(wParam));
        CallHooks(hwnd, message, wParam, lParam);
        return proc ? proc(hwnd, message, wParam, lParam) : 0;
    }

    ATOM Register(std::wstring name, WndProc proc, WndClassEx info)
    {
        std::lock_guard lock(g_lock);
        name = Lower(std::move(name));
        info.name = nullptr; // the caller's string doesn't outlive the call
        for (size_t i = 0; i < g_classes.size(); ++i)
            if (g_classes[i].name == name)
            {
                g_classes[i].proc = proc;
                g_classes[i].info = info;
                return static_cast<ATOM>(0xC000 + i);
            }
        g_classes.push_back({ name, proc, info });
        return static_cast<ATOM>(0xC000 + g_classes.size() - 1);
    }

    WndClassEx FromClass(const WndClass* c)
    {
        return { sizeof(WndClassEx), c->style, c->proc, c->clsExtra, c->wndExtra, c->instance, c->icon, c->cursor,
            c->background, c->menu, nullptr, c->icon };
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

    ATOM WINAPI Shim_RegisterClassExW(const WndClassEx* c) { return Register(static_cast<const wchar_t*>(c->name), c->proc, *c); }
    ATOM WINAPI Shim_RegisterClassExA(const WndClassEx* c) { return Register(Widen(static_cast<const char*>(c->name)), c->proc, *c); }
    ATOM WINAPI Shim_RegisterClassA(const WndClass* c) { return Register(Widen(static_cast<const char*>(c->name)), c->proc, FromClass(c)); }
    ATOM WINAPI Shim_RegisterClassW(const WndClass* c) { return Register(static_cast<const wchar_t*>(c->name), c->proc, FromClass(c)); }

    BOOL WINAPI Shim_GetClassInfoExW(HINSTANCE, LPCWSTR cls, WndClassEx* out)
    {
        std::wstring name = Lower(ClassName(cls, true));
        std::lock_guard lock(g_lock);
        for (auto const& c : g_classes)
            if (c.name == name)
            {
                if (out) { *out = c.info; out->cbSize = sizeof(WndClassEx); out->name = cls; }
                return TRUE;
            }
        SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
        return FALSE;
    }

    HWND MakeWindow(const void* cls, bool wide, DWORD exStyle, DWORD style, HWND parent, HINSTANCE instance, LPVOID param)
    {
        std::wstring name = Lower(ClassName(cls, wide)); // class names are case-insensitive, and stored lowercase
        HWND hwnd;
        bool messageOnly = parent == kHwndMessage;
        {
            std::lock_guard lock(g_lock);
            hwnd = reinterpret_cast<HWND>(g_nextHwnd);
            g_nextHwnd += 0x10;
            Window w;
            w.className = name;
            w.style = static_cast<LONG>(style);
            w.exStyle = static_cast<LONG>(exStyle);
            w.parent = messageOnly ? nullptr : parent;
            w.messageOnly = messageOnly;
            for (auto const& c : g_classes)
                if (c.name == name)
                    w.proc = c.proc;
            g_windows[hwnd] = std::move(w);
            if (!messageOnly && !parent)
                g_main = hwnd; // the newest top-level window is the one on screen
        }
        HOST_LOG(L"CreateWindowEx class=%s -> hwnd %p%s", name.c_str(), static_cast<void*>(hwnd),
            messageOnly ? L" (message-only)" : L"");
        // Before CreateWindowEx returns, the window procedure sees WM_NCCREATE and WM_CREATE on this
        // thread; games keep their per-window object from lpCreateParams.
        CreateStruct cs{ param, instance, nullptr, parent, static_cast<int>(Height()), static_cast<int>(Width()), 0, 0,
            static_cast<LONG>(style), nullptr, name.c_str(), exStyle };
        Call(hwnd, kWmNcCreate, 0, reinterpret_cast<LPARAM>(&cs));
        Call(hwnd, kWmCreate, 0, reinterpret_cast<LPARAM>(&cs));
        // A real window announces its size and activation as it is created.
        if (!messageOnly && !parent)
        {
            PostTo(hwnd, kWmSize, 0, MAKELPARAM(Width(), Height()));
            PostActivation(hwnd, true);
        }
        return hwnd;
    }

    HWND WINAPI Shim_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR, DWORD style, int, int, int, int, HWND parent, HMENU,
        HINSTANCE instance, LPVOID param)
    {
        return MakeWindow(cls, true, exStyle, style, parent, instance, param);
    }

    HWND WINAPI Shim_CreateWindowExA(DWORD exStyle, LPCSTR cls, LPCSTR, DWORD style, int, int, int, int, HWND parent, HMENU,
        HINSTANCE instance, LPVOID param)
    {
        return MakeWindow(cls, false, exStyle, style, parent, instance, param);
    }

    BOOL WINAPI Shim_DestroyWindow(HWND hwnd)
    {
        {
            std::lock_guard lock(g_lock);
            if (!g_windows.count(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
        }
        Call(hwnd, kWmDestroy, 0, 0);
        Call(hwnd, kWmNcDestroy, 0, 0);
        std::lock_guard lock(g_lock);
        g_windows.erase(hwnd);
        if (g_main == hwnd)
        {
            g_main = nullptr;
            for (auto const& [h, w] : g_windows)
                if (!w.messageOnly && !w.parent) g_main = h;
        }
        return TRUE;
    }

    BOOL WINAPI Shim_IsWindow(HWND hwnd)
    {
        std::lock_guard lock(g_lock);
        return g_windows.count(hwnd) ? TRUE : FALSE;
    }

    // ------------------------------------------------------------------ the message queue
    bool Matches(Queued const& q, HWND filter, UINT min, UINT max)
    {
        if (max != 0 && (q.message < min || q.message > max)) return false;
        if (!filter) return true;
        HWND target = q.hwnd ? q.hwnd : g_main;
        if (filter == reinterpret_cast<HWND>(static_cast<intptr_t>(-1))) return !q.hwnd; // thread messages only
        return target == filter;
    }

    BOOL PeekMessage(Msg* msg, HWND hwnd, UINT min, UINT max, UINT flags)
    {
        std::lock_guard lock(g_lock);
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
        {
            if (!Matches(*it, hwnd, min, max))
                continue;
            // A thread message posted with PostThreadMessage keeps its null window; CoreWindow events
            // go to the game window as it is now.
            HWND target = it->hwnd ? it->hwnd : (it->message >= 0x0400 ? nullptr : g_main);
            *msg = { target, it->message, it->wParam, it->lParam, GetTickCount(), {} };
            if (flags & 0x0001 /* PM_REMOVE */)
                g_queue.erase(it);
            return TRUE;
        }
        return FALSE;
    }

    BOOL WINAPI Shim_PeekMessageW(Msg* msg, HWND hwnd, UINT min, UINT max, UINT flags) { return PeekMessage(msg, hwnd, min, max, flags); }

    // Waits for a message; WM_QUIT (0x0012) ends a GetMessage loop.
    BOOL WINAPI Shim_GetMessageW(Msg* msg, HWND hwnd, UINT min, UINT max)
    {
        while (!PeekMessage(msg, hwnd, min, max, 0x0001))
            Sleep(1);
        return msg->message != 0x0012;
    }

    DWORD WINAPI Shim_MsgWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms, DWORD)
    {
        // Any queued message ends the wait, as on Windows; otherwise wait on the handles in slices.
        ULONGLONG until = ms == INFINITE ? ~0ull : GetTickCount64() + ms;
        for (;;)
        {
            {
                std::lock_guard lock(g_lock);
                if (!g_queue.empty()) return WAIT_OBJECT_0 + count;
            }
            DWORD slice = 5;
            DWORD r = count ? WaitForMultipleObjects(count, handles, waitAll, slice) : (Sleep(slice), WAIT_TIMEOUT);
            if (r != WAIT_TIMEOUT) return r;
            if (GetTickCount64() >= until) return WAIT_TIMEOUT;
        }
    }

    // The game's own window procedure runs on the game's thread, like it would on Windows.
    LRESULT WINAPI Shim_DispatchMessage(const Msg* msg)
    {
        if (!msg->hwnd) return 0; // thread messages have no window to go to
        return Call(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    }

    LRESULT WINAPI Shim_SendMessageW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) { return Call(hwnd, message, wParam, lParam); }
    BOOL WINAPI Shim_PostMessageW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        PostTo(hwnd ? hwnd : g_main, message, wParam, lParam);
        return TRUE;
    }
    BOOL WINAPI Shim_PostThreadMessageW(DWORD, UINT message, WPARAM wParam, LPARAM lParam)
    {
        PostTo(nullptr, message, wParam, lParam);
        return TRUE;
    }

    LRESULT WINAPI Shim_CallWindowProcW(WndProc proc, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        return proc ? proc(hwnd, message, wParam, lParam) : 0;
    }

    BOOL WINAPI Shim_TranslateMessage(const Msg*) { return FALSE; }

    // WM_NCCREATE has to answer TRUE or Windows would abandon the window; nothing else needs an answer.
    LRESULT WINAPI Shim_DefWindowProc(HWND, UINT message, WPARAM, LPARAM) { return message == kWmNcCreate; }
    BOOL WINAPI Shim_True() { return TRUE; }
    LONG_PTR WINAPI Shim_Zero() { return 0; }

    // ------------------------------------------------------------------ window state
    BOOL WINAPI Shim_GetClientRect(HWND, RECT* rect)
    {
        *rect = ScreenRect();
        return TRUE;
    }

    // The window is the whole screen, so the frame adds nothing.
    BOOL WINAPI Shim_GetWindowRect(HWND hwnd, RECT* rect) { return Shim_GetClientRect(hwnd, rect); }
    BOOL WINAPI Shim_AdjustWindowRect(RECT*, DWORD, BOOL) { return TRUE; }
    BOOL WINAPI Shim_AdjustWindowRectEx(RECT*, DWORD, BOOL, DWORD) { return TRUE; }
    BOOL WINAPI Shim_ClientToScreen(HWND, POINT*) { return TRUE; }

    BOOL WINAPI Shim_GetWindowPlacement(HWND, WindowPlacement* p)
    {
        *p = {};
        p->length = sizeof(*p);
        p->showCmd = 1; // SW_SHOWNORMAL
        p->normalPosition = ScreenRect();
        return TRUE;
    }

    int WINAPI Shim_GetSystemMetrics(int index)
    {
        switch (index)
        {
        case kSmCxScreen: case kSmCxFullScreen: case kSmCxVirtualScreen: return static_cast<int>(Width());
        case kSmCyScreen: case kSmCyFullScreen: case kSmCyVirtualScreen: return static_cast<int>(Height());
        case kSmCMonitors: return 1;
        default: return 0;
        }
    }

    LONG_PTR WINAPI Shim_GetWindowLongPtr(HWND hwnd, int index)
    {
        std::lock_guard lock(g_lock);
        Window* w = Find(hwnd);
        if (!w) return 0;
        switch (index)
        {
        case kGwlpWndProc: return reinterpret_cast<LONG_PTR>(w->proc);
        case kGwlStyle: return w->style;
        case kGwlExStyle: return w->exStyle;
        default:
        {
            auto it = w->longs.find(index);
            return it != w->longs.end() ? it->second : 0;
        }
        }
    }

    // GWLP_WNDPROC replaces the window procedure: SDL subclasses its windows this way.
    LONG_PTR WINAPI Shim_SetWindowLongPtr(HWND hwnd, int index, LONG_PTR value)
    {
        std::lock_guard lock(g_lock);
        SetLastError(ERROR_SUCCESS); // 0 is a valid old value
        Window* w = Find(hwnd);
        if (!w) return 0;
        LONG_PTR old;
        switch (index)
        {
        case kGwlpWndProc:
            old = reinterpret_cast<LONG_PTR>(w->proc);
            w->proc = reinterpret_cast<WndProc>(value);
            break;
        case kGwlStyle:
            old = w->style;
            w->style = static_cast<LONG>(value);
            break;
        case kGwlExStyle:
            old = w->exStyle;
            w->exStyle = static_cast<LONG>(value);
            break;
        default:
            old = w->longs[index];
            w->longs[index] = value;
        }
        return old;
    }

    LONG WINAPI Shim_GetWindowLong(HWND hwnd, int index) { return static_cast<LONG>(Shim_GetWindowLongPtr(hwnd, index)); }
    LONG WINAPI Shim_SetWindowLong(HWND hwnd, int index, LONG value) { return static_cast<LONG>(Shim_SetWindowLongPtr(hwnd, index, value)); }

    // Props by name, or by atom (a small integer in the pointer).
    std::wstring PropKey(const void* name, bool wide)
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(name);
        if (v < 0x10000) return L"#" + std::to_wstring(v);
        return wide ? std::wstring(static_cast<const wchar_t*>(name)) : Widen(static_cast<const char*>(name));
    }
    BOOL WINAPI Shim_SetPropW(HWND hwnd, LPCWSTR name, HANDLE data)
    {
        std::lock_guard lock(g_lock);
        Window* w = Find(hwnd);
        if (!w) return FALSE;
        w->props[PropKey(name, true)] = data;
        return TRUE;
    }
    HANDLE WINAPI Shim_GetPropW(HWND hwnd, LPCWSTR name)
    {
        std::lock_guard lock(g_lock);
        Window* w = Find(hwnd);
        if (!w) return nullptr;
        auto it = w->props.find(PropKey(name, true));
        return it != w->props.end() ? it->second : nullptr;
    }
    HANDLE WINAPI Shim_RemovePropW(HWND hwnd, LPCWSTR name)
    {
        std::lock_guard lock(g_lock);
        Window* w = Find(hwnd);
        if (!w) return nullptr;
        auto it = w->props.find(PropKey(name, true));
        if (it == w->props.end()) return nullptr;
        HANDLE data = it->second;
        w->props.erase(it);
        return data;
    }
    BOOL WINAPI Shim_SetPropA(HWND hwnd, LPCSTR name, HANDLE data)
    {
        return Shim_SetPropW(hwnd, reinterpret_cast<uintptr_t>(name) < 0x10000 ? reinterpret_cast<LPCWSTR>(name) : Widen(name).c_str(), data);
    }
    HANDLE WINAPI Shim_GetPropA(HWND hwnd, LPCSTR name)
    {
        return Shim_GetPropW(hwnd, reinterpret_cast<uintptr_t>(name) < 0x10000 ? reinterpret_cast<LPCWSTR>(name) : Widen(name).c_str());
    }

    HWND WINAPI Shim_GetParent(HWND hwnd)
    {
        std::lock_guard lock(g_lock);
        auto it = g_windows.find(hwnd);
        return it != g_windows.end() ? it->second.parent : nullptr;
    }

    DWORD WINAPI Shim_GetWindowThreadProcessId(HWND, LPDWORD process)
    {
        if (process) *process = GetCurrentProcessId();
        return GetCurrentThreadId();
    }

    // The game window always has the focus and the foreground; asking for it changes nothing.
    HWND WINAPI Shim_GetForegroundWindow() { std::lock_guard lock(g_lock); return g_main; }
    HWND WINAPI Shim_SetFocus(HWND) { return Shim_GetForegroundWindow(); }

    // ------------------------------------------------------------------ the one monitor and display mode
    HMONITOR WINAPI Shim_MonitorFromWindow(HWND, DWORD) { return kMonitor; }
    HMONITOR WINAPI Shim_MonitorFromRect(LPCRECT, DWORD) { return kMonitor; }
    HMONITOR WINAPI Shim_MonitorFromPoint(POINT, DWORD) { return kMonitor; }

    template <typename C, size_t N> void Copy(C (&to)[N], const wchar_t* from)
    {
        for (size_t i = 0; i < N; ++i)
        {
            to[i] = static_cast<C>(from[i]);
            if (!from[i]) break;
        }
        to[N - 1] = 0;
    }

    template <typename C> BOOL GetMonitorInfoT(MonitorInfo<C>* info)
    {
        if (!info || info->size < offsetof(MonitorInfo<C>, device))
            return FALSE;
        info->monitor = info->work = ScreenRect();
        info->flags = 1; // MONITORINFOF_PRIMARY
        if (info->size >= sizeof(MonitorInfo<C>))
            Copy(info->device, L"\\\\.\\DISPLAY1");
        return TRUE;
    }
    BOOL WINAPI Shim_GetMonitorInfoW(HMONITOR, MonitorInfo<wchar_t>* info) { return GetMonitorInfoT(info); }
    BOOL WINAPI Shim_GetMonitorInfoA(HMONITOR, MonitorInfo<char>* info) { return GetMonitorInfoT(info); }

    using MonitorEnumProc = BOOL(CALLBACK*)(HMONITOR, HDC, LPRECT, LPARAM);
    BOOL WINAPI Shim_EnumDisplayMonitors(HDC, LPCRECT, MonitorEnumProc proc, LPARAM data)
    {
        RECT r = ScreenRect();
        if (proc)
            proc(kMonitor, nullptr, &r, data);
        return TRUE;
    }

    // One display mode: the output's size, 32-bit, 60 Hz.
    template <typename C> BOOL EnumDisplaySettingsT(DWORD mode, DevMode<C>* dm)
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
    BOOL WINAPI Shim_EnumDisplaySettingsW(LPCWSTR, DWORD mode, DevMode<wchar_t>* dm) { return EnumDisplaySettingsT(mode, dm); }
    BOOL WINAPI Shim_EnumDisplaySettingsA(LPCSTR, DWORD mode, DevMode<char>* dm) { return EnumDisplaySettingsT(mode, dm); }

    // Asking for the one mode there is succeeds; there is nothing to change.
    LONG WINAPI Shim_ChangeDisplaySettings() { return 0; } // DISP_CHANGE_SUCCESSFUL

    // One display adapter with one monitor: the output.
    template <typename C> struct DisplayDevice // DISPLAY_DEVICEW / DISPLAY_DEVICEA
    {
        DWORD cb;
        C name[32], description[128];
        DWORD stateFlags;
        C id[128], key[128];
    };
    static_assert(sizeof(DisplayDevice<wchar_t>) == 840 && sizeof(DisplayDevice<char>) == 424);

    template <typename C> BOOL EnumDisplayDevicesT(const C* device, DWORD index, DisplayDevice<C>* dd)
    {
        if (!dd || index != 0)
            return FALSE;
        DWORD cb = dd->cb;
        memset(reinterpret_cast<uint8_t*>(dd) + sizeof(DWORD), 0, std::min<size_t>(cb, sizeof(*dd)) - sizeof(DWORD));
        if (!device) // the adapter
        {
            Copy(dd->name, L"\\\\.\\DISPLAY1");
            Copy(dd->description, L"Xbox display");
            dd->stateFlags = 0x1 | 0x4; // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE
        }
        else // its monitor
        {
            Copy(dd->name, L"\\\\.\\DISPLAY1\\Monitor0");
            Copy(dd->description, L"Generic PnP Monitor");
            dd->stateFlags = 0x1 | 0x2; // DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED
        }
        return TRUE;
    }
    BOOL WINAPI Shim_EnumDisplayDevicesW(LPCWSTR device, DWORD index, DisplayDevice<wchar_t>* dd, DWORD)
    {
        return EnumDisplayDevicesT(device, index, dd);
    }
    BOOL WINAPI Shim_EnumDisplayDevicesA(LPCSTR device, DWORD index, DisplayDevice<char>* dd, DWORD)
    {
        return EnumDisplayDevicesT(device, index, dd);
    }

    // The settings a game reads to size its window and tune its input, as a default Windows has them.
    BOOL WINAPI Shim_SystemParametersInfo(UINT action, UINT, PVOID value, UINT)
    {
        auto setInt = [&](int v) { if (value) *static_cast<int*>(value) = v; return TRUE; };
        switch (action)
        {
        case 0x0030: // SPI_GETWORKAREA
            if (value) *static_cast<RECT*>(value) = ScreenRect();
            return TRUE;
        case 0x0003: // SPI_GETMOUSE: thresholds and acceleration
            if (value) { auto v = static_cast<int*>(value); v[0] = 6; v[1] = 10; v[2] = 1; }
            return TRUE;
        case 0x0070: return setInt(10); // SPI_GETMOUSESPEED
        case 0x0016: return setInt(1);  // SPI_GETKEYBOARDDELAY
        case 0x000A: return setInt(31); // SPI_GETKEYBOARDSPEED
        case 0x0068: return setInt(3);  // SPI_GETWHEELSCROLLLINES
        case 0x006C: return setInt(3);  // SPI_GETWHEELSCROLLCHARS
        case 0x0010: case 0x0024: case 0x0042: // SPI_GETSCREENSAVEACTIVE, SPI_GETDRAGFULLWINDOWS, SPI_GETHIGHCONTRAST (as "off")
            return setInt(0);
        default:
            // Setters succeed (there is nothing to change); unknown getters fail, as for an old Windows.
            return (action & 1) || action >= 0x2000 ? TRUE : FALSE;
        }
    }

    // ------------------------------------------------------------------ small things
    BOOL WINAPI Shim_SetRect(RECT* r, int left, int top, int right, int bottom)
    {
        if (!r) return FALSE;
        *r = { left, top, right, bottom };
        return TRUE;
    }
    BOOL WINAPI Shim_SetRectEmpty(RECT* r) { return Shim_SetRect(r, 0, 0, 0, 0); }
    BOOL WINAPI Shim_IntersectRect(RECT* out, const RECT* a, const RECT* b)
    {
        RECT r{ std::max(a->left, b->left), std::max(a->top, b->top), std::min(a->right, b->right), std::min(a->bottom, b->bottom) };
        bool empty = r.left >= r.right || r.top >= r.bottom;
        *out = empty ? RECT{} : r;
        return !empty;
    }
    BOOL WINAPI Shim_PtInRect(const RECT* r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
    BOOL WINAPI Shim_GetClipCursor(RECT* r) { if (r) *r = ScreenRect(); return TRUE; }

    // The whole window is always up for repainting.
    BOOL WINAPI Shim_GetUpdateRect(HWND, RECT* r, BOOL)
    {
        if (r) *r = ScreenRect();
        return TRUE;
    }

    HWND WINAPI Shim_GetDesktopWindow() { return kDesktop; }
    HWND WINAPI Shim_SetCapture(HWND) { return nullptr; } // nobody had the mouse before
    HKL WINAPI Shim_GetKeyboardLayout(DWORD) { return reinterpret_cast<HKL>(static_cast<uintptr_t>(0x04090409)); } // en-US

    // A US layout's mappings between virtual keys, scan codes and the characters keys type.
    UINT WINAPI Shim_MapVirtualKeyW(UINT code, UINT type)
    {
        switch (type)
        {
        case 0: return VirtualKeyToScanCode(code); // MAPVK_VK_TO_VSC
        case 1: return ScanCodeToVirtualKey(code & 0xFF, (code & 0xFF00) == 0xE000, false); // MAPVK_VSC_TO_VK
        case 3: return ScanCodeToVirtualKey(code & 0xFF, (code & 0xFF00) == 0xE000, true); // MAPVK_VSC_TO_VK_EX
        case 4: // MAPVK_VK_TO_VSC_EX
        {
            bool extended = false;
            UINT scan = VirtualKeyToScanCode(code, &extended);
            return scan && extended ? 0xE000 | scan : scan;
        }
        case 2: // MAPVK_VK_TO_CHAR: the unshifted character (letters as capitals)
        {
            if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ')
                return code;
            static const std::pair<UINT, char> kOem[] = { { 0xBD, '-' }, { 0xBB, '=' }, { 0xDB, '[' }, { 0xDD, ']' },
                { 0xBA, ';' }, { 0xDE, '\'' }, { 0xC0, '`' }, { 0xDC, '\\' }, { 0xBC, ',' }, { 0xBE, '.' }, { 0xBF, '/' },
                { 0x0D, '\r' }, { 0x08, '\b' }, { 0x09, '\t' }, { 0x1B, 0x1B } };
            for (auto const& [vk, c] : kOem)
                if (vk == code)
                    return static_cast<UINT>(c);
            return 0;
        }
        default: return 0;
        }
    }

    // The character a key types on a US layout: letters (shifted or not), digits, space.
    int WINAPI Shim_ToUnicode(UINT vk, UINT, const BYTE* state, LPWSTR out, int size, UINT)
    {
        if (!out || size < 1) return 0;
        bool shift = state && ((state[0x10] | state[0xA0] | state[0xA1]) & 0x80);
        wchar_t c = 0;
        if (vk >= 'A' && vk <= 'Z') c = static_cast<wchar_t>(shift ? vk : vk + 32);
        else if (vk >= '0' && vk <= '9' && !shift) c = static_cast<wchar_t>(vk);
        else if (vk == ' ') c = L' ';
        if (!c) return 0;
        out[0] = c;
        return 1;
    }

    std::atomic<UINT> g_nextMessage{ 0xC100 };
    std::mutex g_messageLock;
    std::map<std::wstring, UINT> g_messages; // same name, same id, as Windows does
    UINT RegisterMessage(std::wstring name)
    {
        std::lock_guard lock(g_messageLock);
        auto it = g_messages.find(Lower(name));
        if (it != g_messages.end()) return it->second;
        UINT id = g_nextMessage++;
        g_messages.emplace(Lower(std::move(name)), id);
        return id;
    }
    UINT WINAPI Shim_RegisterWindowMessageW(LPCWSTR name) { return RegisterMessage(name ? name : L""); }
    UINT WINAPI Shim_RegisterWindowMessageA(LPCSTR name) { return RegisterMessage(Widen(name ? name : "")); }

    UINT WINAPI Shim_GetDoubleClickTime() { return 500; }
    LONG WINAPI Shim_GetMessageTime() { return static_cast<LONG>(GetTickCount()); }

    // Icons, cursors and device notifications are never acted on; handles that can be freed are enough.
    std::atomic<uintptr_t> g_nextHandle{ 0x00020000 };
    HANDLE WINAPI Shim_NewHandle() { return reinterpret_cast<HANDLE>(g_nextHandle += 0x10); }

    // Timers: the id the game asked for, or a new one. WM_TIMER isn't delivered (no game here needs it yet).
    UINT_PTR WINAPI Shim_SetTimer(HWND, UINT_PTR id, UINT, void*) { return id ? id : reinterpret_cast<UINT_PTR>(Shim_NewHandle()); }

    // Raw input: registering works, and there are no raw devices; mouse and keyboard come as messages.
    UINT WINAPI Shim_GetRawInputDeviceList(void*, PUINT count, UINT) { if (count) *count = 0; return 0; }
    UINT WINAPI Shim_RawInputNone() { return static_cast<UINT>(-1); } // GetRawInputData, GetRawInputDeviceInfo

    // WH_CALLWNDPROC hooks are kept and called before a game window's procedure (see CallHooks). Other
    // hooks (keyboard, mouse, other processes) aren't possible in the app partition; games carry on without.
    HHOOK WINAPI Shim_SetWindowsHookEx(int id, void* proc, HINSTANCE, DWORD thread)
    {
        constexpr int kWhCallWndProc = 4;
        if (id != kWhCallWndProc || !proc)
        {
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        HHOOK handle = reinterpret_cast<HHOOK>(g_nextHook += 0x10);
        std::lock_guard lock(g_hookLock);
        g_hooks.push_back({ handle, reinterpret_cast<CallWndProcHook>(proc), thread });
        if (g_hooks.size() <= 4)
            HOST_LOG(L"WH_CALLWNDPROC hook for thread %lu", thread);
        return handle;
    }
    BOOL WINAPI Shim_UnhookWindowsHookEx(HHOOK handle)
    {
        std::lock_guard lock(g_hookLock);
        auto it = std::find_if(g_hooks.begin(), g_hooks.end(), [&](Hook const& h) { return h.handle == handle; });
        if (it != g_hooks.end()) g_hooks.erase(it);
        return TRUE;
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

    // A window's DC is its handle with bit 3 set (handles are 16 apart); no window is kDc.
    HDC WINAPI Shim_GetDC(HWND hwnd)
    {
        return hwnd ? reinterpret_cast<HDC>(reinterpret_cast<uintptr_t>(hwnd) | 8) : kDc;
    }
    HWND WINAPI Shim_WindowFromDC(HDC dc)
    {
        auto v = reinterpret_cast<uintptr_t>(dc);
        if (dc == kDc)
            return g_main;
        if ((v & 0xF) == 8 && v >= 0x00010100 && v < g_nextHwnd)
            return reinterpret_cast<HWND>(v & ~uintptr_t(0xF));
        return nullptr;
    }
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
        LogGameStack(L"MessageBox from");
        return 1; // IDOK
    }

    int WINAPI Shim_MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT)
    {
        HOST_LOG(L"MessageBox [%s]: %s", title ? title : L"", text ? text : L"");
        LogGameStack(L"MessageBox from");
        return 1; // IDOK
    }
}

void AttachGameWindow(winrt::Windows::UI::Core::CoreWindow const& window)
{
    using namespace winrt::Windows::UI::Core;
    NoteHostUiThread();
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
        PostActivation(nullptr, args.WindowActivationState() != CoreWindowActivationState::Deactivated);
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
    g_windows.clear();
    g_main = nullptr;
}

void PostGameMessage(UINT message, WPARAM wParam, LPARAM lParam) { Post(message, wParam, lParam); }

void BroadcastGameMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    std::lock_guard lock(g_lock);
    for (auto const& entry : g_windows)
        PostTo(entry.first, message, wParam, lParam);
}
UINT GameScreenWidth() { return Width(); }
UINT GameScreenHeight() { return Height(); }
HDC GameWindowDc() { return kDc; }
bool IsWindowDc(HDC dc) { return dc == kDc || Shim_WindowFromDC(dc) != nullptr; }
HMONITOR GameMonitor() { return kMonitor; }

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
    auto reg = [&](std::initializer_list<const char*> names, void* fn) {
        for (const char* name : names)
            layer.Register((std::string("user32!") + name).c_str(), fn);
    };
#define FN(f) reinterpret_cast<void*>(&f)
    reg({ "RegisterClassExA" }, FN(Shim_RegisterClassExA));
    reg({ "RegisterClassExW" }, FN(Shim_RegisterClassExW));
    reg({ "RegisterClassA" }, FN(Shim_RegisterClassA));
    reg({ "RegisterClassW" }, FN(Shim_RegisterClassW));
    reg({ "GetClassInfoExW" }, FN(Shim_GetClassInfoExW));
    reg({ "CreateWindowExA" }, FN(Shim_CreateWindowExA));
    reg({ "CreateWindowExW" }, FN(Shim_CreateWindowExW));
    reg({ "DestroyWindow" }, FN(Shim_DestroyWindow));
    reg({ "IsWindow" }, FN(Shim_IsWindow));
    reg({ "PeekMessageA", "PeekMessageW" }, FN(Shim_PeekMessageW));
    reg({ "GetMessageA", "GetMessageW" }, FN(Shim_GetMessageW));
    reg({ "MsgWaitForMultipleObjects" }, FN(Shim_MsgWaitForMultipleObjects));
    reg({ "DispatchMessageA", "DispatchMessageW" }, FN(Shim_DispatchMessage));
    reg({ "SendMessageA", "SendMessageW" }, FN(Shim_SendMessageW));
    reg({ "PostMessageA", "PostMessageW" }, FN(Shim_PostMessageW));
    reg({ "PostThreadMessageA", "PostThreadMessageW" }, FN(Shim_PostThreadMessageW));
    reg({ "CallWindowProcA", "CallWindowProcW" }, FN(Shim_CallWindowProcW));
    reg({ "TranslateMessage" }, FN(Shim_TranslateMessage));
    reg({ "DefWindowProcA", "DefWindowProcW" }, FN(Shim_DefWindowProc));
    reg({ "GetClientRect" }, FN(Shim_GetClientRect));
    reg({ "GetWindowRect" }, FN(Shim_GetWindowRect));
    reg({ "AdjustWindowRect" }, FN(Shim_AdjustWindowRect));
    reg({ "AdjustWindowRectEx" }, FN(Shim_AdjustWindowRectEx));
    reg({ "ClientToScreen", "ScreenToClient" }, FN(Shim_ClientToScreen));
    reg({ "GetWindowPlacement" }, FN(Shim_GetWindowPlacement));
    reg({ "GetSystemMetrics" }, FN(Shim_GetSystemMetrics));
    reg({ "GetWindowLongA", "GetWindowLongW" }, FN(Shim_GetWindowLong));
    reg({ "SetWindowLongA", "SetWindowLongW" }, FN(Shim_SetWindowLong));
    reg({ "GetWindowLongPtrA", "GetWindowLongPtrW" }, FN(Shim_GetWindowLongPtr));
    reg({ "SetWindowLongPtrA", "SetWindowLongPtrW" }, FN(Shim_SetWindowLongPtr));
    reg({ "SetPropW" }, FN(Shim_SetPropW));
    reg({ "GetPropW" }, FN(Shim_GetPropW));
    reg({ "RemovePropW" }, FN(Shim_RemovePropW));
    reg({ "SetPropA" }, FN(Shim_SetPropA));
    reg({ "GetPropA" }, FN(Shim_GetPropA));
    reg({ "GetParent" }, FN(Shim_GetParent));
    reg({ "GetWindowThreadProcessId" }, FN(Shim_GetWindowThreadProcessId));
    reg({ "GetForegroundWindow", "GetActiveWindow", "GetFocus" }, FN(Shim_GetForegroundWindow));
    reg({ "SetFocus", "SetActiveWindow" }, FN(Shim_SetFocus));
    reg({ "MonitorFromWindow" }, FN(Shim_MonitorFromWindow));
    reg({ "MonitorFromRect" }, FN(Shim_MonitorFromRect));
    reg({ "MonitorFromPoint" }, FN(Shim_MonitorFromPoint));
    reg({ "GetMonitorInfoW" }, FN(Shim_GetMonitorInfoW));
    reg({ "GetMonitorInfoA" }, FN(Shim_GetMonitorInfoA));
    reg({ "EnumDisplayMonitors" }, FN(Shim_EnumDisplayMonitors));
    reg({ "EnumDisplaySettingsW", "EnumDisplaySettingsExW" }, FN(Shim_EnumDisplaySettingsW));
    reg({ "EnumDisplaySettingsA", "EnumDisplaySettingsExA" }, FN(Shim_EnumDisplaySettingsA));
    reg({ "ChangeDisplaySettingsW", "ChangeDisplaySettingsA", "ChangeDisplaySettingsExW", "ChangeDisplaySettingsExA" },
        FN(Shim_ChangeDisplaySettings));
    reg({ "EnumDisplayDevicesW" }, FN(Shim_EnumDisplayDevicesW));
    reg({ "EnumDisplayDevicesA" }, FN(Shim_EnumDisplayDevicesA));
    reg({ "SystemParametersInfoW", "SystemParametersInfoA" }, FN(Shim_SystemParametersInfo));
    reg({ "SetRect" }, FN(Shim_SetRect));
    reg({ "SetRectEmpty" }, FN(Shim_SetRectEmpty));
    reg({ "IntersectRect" }, FN(Shim_IntersectRect));
    reg({ "PtInRect" }, FN(Shim_PtInRect));
    reg({ "GetClipCursor" }, FN(Shim_GetClipCursor));
    reg({ "GetUpdateRect" }, FN(Shim_GetUpdateRect));
    reg({ "GetDesktopWindow" }, FN(Shim_GetDesktopWindow));
    reg({ "SetCapture" }, FN(Shim_SetCapture));
    reg({ "GetKeyboardLayout" }, FN(Shim_GetKeyboardLayout));
    reg({ "MapVirtualKeyW", "MapVirtualKeyA" }, FN(Shim_MapVirtualKeyW));
    reg({ "ToUnicode" }, FN(Shim_ToUnicode));
    reg({ "RegisterWindowMessageW" }, FN(Shim_RegisterWindowMessageW));
    reg({ "RegisterWindowMessageA" }, FN(Shim_RegisterWindowMessageA));
    reg({ "GetDoubleClickTime" }, FN(Shim_GetDoubleClickTime));
    reg({ "GetMessageTime" }, FN(Shim_GetMessageTime));
    reg({ "CreateIconIndirect", "CreateIconFromResource", "CreateIconFromResourceEx", "CopyImage", "CopyIcon",
        "RegisterDeviceNotificationW", "RegisterDeviceNotificationA" }, FN(Shim_NewHandle));
    reg({ "SetTimer" }, FN(Shim_SetTimer));
    reg({ "GetRawInputDeviceList" }, FN(Shim_GetRawInputDeviceList));
    reg({ "GetRawInputData", "GetRawInputDeviceInfoA", "GetRawInputDeviceInfoW" }, FN(Shim_RawInputNone));
    reg({ "SetWindowsHookExW", "SetWindowsHookExA" }, FN(Shim_SetWindowsHookEx));
    reg({ "UnhookWindowsHookEx" }, FN(Shim_UnhookWindowsHookEx));
    reg({ "OpenClipboard" }, FN(Shim_OpenClipboard));
    reg({ "IsClipboardFormatAvailable" }, FN(Shim_IsClipboardFormatAvailable));
    reg({ "GetClipboardData", "SetClipboardData" }, FN(Shim_GetClipboardData));
    reg({ "DialogBoxParamW", "DialogBoxIndirectParamW" }, FN(Shim_DialogBoxParamW));
    reg({ "GetDlgItem" }, FN(Shim_GetDlgItem));
    reg({ "SendDlgItemMessageW" }, FN(Shim_SendDlgItemMessageW));
    reg({ "LoadIconW", "LoadIconA" }, FN(Shim_LoadIconW));
    reg({ "WindowFromPoint" }, FN(Shim_GetForegroundWindow)); // the game window covers the screen
    reg({ "LoadCursorA", "LoadCursorW" }, FN(Shim_LoadCursorA));
    reg({ "LoadImageA" }, FN(Shim_LoadImageA));
    reg({ "GetDC", "GetWindowDC" }, FN(Shim_GetDC));
    reg({ "ReleaseDC" }, FN(Shim_ReleaseDC));
    reg({ "WindowFromDC" }, FN(Shim_WindowFromDC));
    reg({ "ShowCursor" }, FN(Shim_ShowCursor));
    reg({ "ShowWindow" }, FN(Shim_ShowWindow));
    reg({ "MessageBoxA" }, FN(Shim_MessageBoxA));
    reg({ "MessageBoxW" }, FN(Shim_MessageBoxW));
    // Accepted and nothing to do on the console.
    reg({ "BringWindowToTop", "ClipCursor", "SetForegroundWindow", "SetWindowPos", "SetWindowTextA", "SetWindowTextW",
        "ValidateRect", "ReleaseCapture", "TrackMouseEvent", "CloseClipboard", "SetDlgItemTextW", "UpdateWindow",
        "InvalidateRect", "UnregisterClassW", "UnregisterClassA", "DestroyIcon", "DestroyCursor", "KillTimer",
        "UnregisterDeviceNotification", "RegisterRawInputDevices", "ChangeWindowMessageFilter",
        "ChangeWindowMessageFilterEx", "FlashWindowEx", "SetLayeredWindowAttributes", "SetWindowRgn", "AttachThreadInput",
        "EndDialog", "MoveWindow" }, FN(Shim_True));
    // Answered "nothing": no text, no menu, not minimized, no previous cursor, no hook chain, no clipboard.
    reg({ "GetWindowTextW", "GetWindowTextA", "GetWindowTextLengthW", "GetWindowTextLengthA", "GetMenu", "IsIconic",
        "SetCursor", "CallNextHookEx", "GetMessageExtraInfo", "EmptyClipboard", "GetClipboardSequenceNumber", "FillRect",
        "DrawTextW", "DrawTextA", "IsZoomed" }, FN(Shim_Zero));
#undef FN
}
