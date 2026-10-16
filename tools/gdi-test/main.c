// Freestanding test program for the gdi32 shims: draws into a DIB section and
// copies it to the window both ways. Exits with a code saying how far it got.
//   1 DIB made   2 memory DC made   4 bitmap selected   8 BitBlt ok
//   16 SetDIBitsToDevice drew all rows   32 GetDeviceCaps says 32-bit   64 cleaned up
#include <windows.h>

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void start(void)
{
    static WNDCLASSEXA wc;      // static so these are zeroed without a memset call
    static BITMAPINFO bmi;
    unsigned int *bits = NULL;
    HWND hwnd;
    HDC hdc, mem;
    HBITMAP bmp;
    int mask = 0, i;

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "gdi_test";
    RegisterClassExA(&wc);
    hwnd = CreateWindowExA(0, "gdi_test", "gdi-test", WS_POPUP, 0, 0, 64, 32, NULL, NULL, NULL, NULL);
    hdc = GetDC(hwnd);

    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 64;
    bmi.bmiHeader.biHeight = -32; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp && bits)
    {
        mask |= 1;
        for (i = 0; i < 64 * 32; i++)
            bits[i] = 0x00336699;
    }
    mem = CreateCompatibleDC(hdc);
    if (mem) mask |= 2;
    if (mem && bmp && SelectObject(mem, bmp)) mask |= 4;
    if (BitBlt(hdc, 0, 0, 64, 32, mem, 0, 0, SRCCOPY)) mask |= 8;
    if (SetDIBitsToDevice(hdc, 0, 0, 64, 32, 0, 0, 0, 32, bits, &bmi, DIB_RGB_COLORS) == 32) mask |= 16;
    if (GetDeviceCaps(hdc, BITSPIXEL) == 32) mask |= 32;
    if (DeleteObject(bmp) && DeleteDC(mem)) mask |= 64;
    ReleaseDC(hwnd, hdc);
    ExitProcess(mask);
}
