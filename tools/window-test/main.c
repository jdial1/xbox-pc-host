// Freestanding test program for the window and input shims: makes a window,
// pumps messages, and exits at WM_CLOSE with a code saying what it saw.
//   1 WM_SIZE   2 WM_ACTIVATE   4 W down (and GetKeyState agrees)   8 Space down
//   16 mouse move   32 left button down   64 wheel
#include <windows.h>

static int mask;
static int done;

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE: mask |= 1; return 0;
    case WM_ACTIVATE: mask |= 2; return 0;
    case WM_KEYDOWN:
        if (wp == 'W' && GetKeyState('W') < 0) mask |= 4;
        if (wp == VK_SPACE) mask |= 8;
        return 0;
    case WM_MOUSEMOVE: mask |= 16; return 0;
    case WM_LBUTTONDOWN: mask |= 32; return 0;
    case WM_MOUSEWHEEL: mask |= 64; return 0;
    case WM_CLOSE: done = 1; return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void start(void)
{
    static WNDCLASSEXA wc; // static so it is zeroed without a memset call
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "window_test";
    RegisterClassExA(&wc);
    CreateWindowExA(0, "window_test", "window-test", WS_POPUP, 0, 0, 640, 480, NULL, NULL, NULL, NULL);

    for (;;)
    {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        if (done) break;
        Sleep(5);
    }
    ExitProcess(mask);
}
