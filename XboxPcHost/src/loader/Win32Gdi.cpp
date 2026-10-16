#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/GameDisplay.h"
#include "Log.h"

// Just enough gdi32 for a game that draws its menus into a software bitmap and
// copies that to its window (ClassiCube's launcher does). A DIB section is
// plain memory; a memory DC only remembers which bitmap is selected into it;
// copying a bitmap to the window DC sends its pixels to the screen.
// The window DC itself is the fake one user32's GetDC hands out.
namespace
{
    struct BmiHeader // BITMAPINFOHEADER
    {
        DWORD size;
        LONG width;
        LONG height; // negative when the rows run top to bottom
        WORD planes, bitCount;
        DWORD compression, sizeImage;
        LONG xPixelsPerMeter, yPixelsPerMeter;
        DWORD colorsUsed, colorsImportant;
    };
    static_assert(sizeof(BmiHeader) == 40);

    constexpr DWORD kSrcCopy = 0x00CC0020;
    constexpr int kBitsPixel = 12, kPlanes = 14, kHorzRes = 8, kVertRes = 10, kLogPixelsX = 88, kLogPixelsY = 90;

    struct Bitmap
    {
        int width = 0, height = 0; // height is positive; topDown says how rows are stored
        bool topDown = true;
        uint32_t* bits = nullptr;
        size_t Bytes() const { return static_cast<size_t>(width) * height * 4; }
        size_t Pitch() const { return static_cast<size_t>(width) * 4; }
    };

    struct MemoryDc
    {
        Bitmap* selected = nullptr;
    };

    std::mutex g_lock;
    GameDisplay* g_display = nullptr;
    std::set<Bitmap*> g_bitmaps;
    std::set<MemoryDc*> g_dcs;
    std::atomic<bool> g_presented{ false };

    // What SelectObject reports as the previous object when there was none.
    HGDIOBJ const kStockObject = reinterpret_cast<HGDIOBJ>(static_cast<uintptr_t>(0x00010050));

    Bitmap* AsBitmap(void* handle)
    {
        auto bitmap = static_cast<Bitmap*>(handle);
        return g_bitmaps.count(bitmap) ? bitmap : nullptr;
    }

    MemoryDc* AsMemoryDc(HDC handle)
    {
        auto dc = reinterpret_cast<MemoryDc*>(handle);
        return g_dcs.count(dc) ? dc : nullptr;
    }

    // Sends rows (top to bottom, 32-bit) to the screen.
    void Show(const uint32_t* top, int width, int height, size_t pitchBytes)
    {
        if (!g_display)
            return;
        if (!g_presented.exchange(true))
            HOST_LOG(L"first GDI frame %dx%d", width, height);
        g_display->PresentPixels(top, static_cast<UINT>(width), static_cast<UINT>(height), static_cast<UINT>(pitchBytes));
    }

    void ShowBitmap(Bitmap const& bitmap)
    {
        if (bitmap.topDown)
        {
            Show(bitmap.bits, bitmap.width, bitmap.height, bitmap.Pitch());
            return;
        }
        // Bottom-up: the first row in memory is the last on screen, so a negative pitch walks it backwards.
        auto last = reinterpret_cast<const uint8_t*>(bitmap.bits) + bitmap.Pitch() * (bitmap.height - 1);
        std::vector<uint32_t> flipped(static_cast<size_t>(bitmap.width) * bitmap.height);
        for (int y = 0; y < bitmap.height; ++y)
            memcpy(&flipped[static_cast<size_t>(y) * bitmap.width], last - bitmap.Pitch() * y, bitmap.Pitch());
        Show(flipped.data(), bitmap.width, bitmap.height, bitmap.Pitch());
    }

    HDC WINAPI Shim_CreateCompatibleDC(HDC)
    {
        auto dc = new MemoryDc;
        std::lock_guard lock(g_lock);
        g_dcs.insert(dc);
        return reinterpret_cast<HDC>(dc);
    }

    BOOL WINAPI Shim_DeleteDC(HDC handle)
    {
        std::lock_guard lock(g_lock);
        MemoryDc* dc = AsMemoryDc(handle);
        if (!dc)
            return FALSE;
        g_dcs.erase(dc);
        delete dc;
        return TRUE;
    }

    HBITMAP WINAPI Shim_CreateDIBSection(HDC, const BmiHeader* header, UINT, void** bits, HANDLE section, DWORD)
    {
        if (!header || header->size < sizeof(BmiHeader) || header->width <= 0 || header->height == 0 || section)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        // Compression 0 (BI_RGB) or 3 (BI_BITFIELDS) at 32 bits is the only layout games use for framebuffers.
        if (header->bitCount != 32 || (header->compression != 0 && header->compression != 3))
        {
            HOST_LOG(L"CreateDIBSection: unsupported format %u-bit, compression %lu", header->bitCount, header->compression);
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        auto bitmap = std::make_unique<Bitmap>();
        bitmap->width = header->width;
        bitmap->height = std::abs(header->height);
        bitmap->topDown = header->height < 0;
        bitmap->bits = static_cast<uint32_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bitmap->Bytes()));
        if (!bitmap->bits)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        if (bits)
            *bits = bitmap->bits;
        HOST_LOG(L"CreateDIBSection %dx%d %s", bitmap->width, bitmap->height, bitmap->topDown ? L"top-down" : L"bottom-up");

        Bitmap* raw = bitmap.release();
        std::lock_guard lock(g_lock);
        g_bitmaps.insert(raw);
        return reinterpret_cast<HBITMAP>(raw);
    }

    HGDIOBJ WINAPI Shim_SelectObject(HDC hdc, HGDIOBJ object)
    {
        std::lock_guard lock(g_lock);
        MemoryDc* dc = AsMemoryDc(hdc);
        Bitmap* bitmap = AsBitmap(object);
        if (!dc || !bitmap)
            return kStockObject; // fonts, brushes and the like: accepted and ignored
        Bitmap* previous = dc->selected;
        dc->selected = bitmap;
        return previous ? reinterpret_cast<HGDIOBJ>(previous) : kStockObject;
    }

    BOOL WINAPI Shim_DeleteObject(HGDIOBJ object)
    {
        std::lock_guard lock(g_lock);
        if (Bitmap* bitmap = AsBitmap(object))
        {
            for (MemoryDc* dc : g_dcs)
                if (dc->selected == bitmap)
                    dc->selected = nullptr;
            g_bitmaps.erase(bitmap);
            HeapFree(GetProcessHeap(), 0, bitmap->bits);
            delete bitmap;
        }
        return TRUE;
    }

    BOOL WINAPI Shim_BitBlt(HDC destination, int x, int y, int width, int height, HDC source, int sourceX, int sourceY, DWORD rop)
    {
        std::lock_guard lock(g_lock);
        MemoryDc* from = AsMemoryDc(source);
        if (!from || !from->selected)
            return FALSE;
        if (rop != kSrcCopy)
            HOST_LOG(L"BitBlt: raster op 0x%08lx treated as a plain copy", rop);

        if (destination == GameWindowDc())
        {
            ShowBitmap(*from->selected);
            return TRUE;
        }
        MemoryDc* to = AsMemoryDc(destination);
        if (!to || !to->selected)
            return FALSE;

        // Copy between two bitmaps, clipped to both.
        Bitmap const& src = *from->selected;
        Bitmap& dst = *to->selected;
        for (int row = 0; row < height; ++row)
        {
            int sy = sourceY + row, dy = y + row;
            if (sy < 0 || sy >= src.height || dy < 0 || dy >= dst.height)
                continue;
            int count = std::min({ width, src.width - sourceX, dst.width - x });
            if (count <= 0 || sourceX < 0 || x < 0)
                continue;
            int srcRow = src.topDown ? sy : src.height - 1 - sy;
            int dstRow = dst.topDown ? dy : dst.height - 1 - dy;
            memcpy(dst.bits + static_cast<size_t>(dstRow) * dst.width + x, src.bits + static_cast<size_t>(srcRow) * src.width + sourceX,
                static_cast<size_t>(count) * 4);
        }
        return TRUE;
    }

    // Draws the caller's pixels straight to the window. The whole image is shown, which is what a
    // game wants when it hands over its complete framebuffer; the destination and source
    // rectangles are ignored.
    int WINAPI Shim_SetDIBitsToDevice(HDC destination, int, int, DWORD, DWORD, int, int, UINT, UINT lines,
        const void* bits, const BmiHeader* header, UINT)
    {
        if (destination != GameWindowDc() || !bits || !header || header->bitCount != 32 || header->width <= 0 || lines == 0)
            return 0;
        Bitmap view;
        view.width = header->width;
        view.height = static_cast<int>(lines);
        view.topDown = header->height < 0;
        view.bits = const_cast<uint32_t*>(static_cast<const uint32_t*>(bits));
        ShowBitmap(view);
        return static_cast<int>(lines);
    }

    int WINAPI Shim_GetDeviceCaps(HDC, int index)
    {
        switch (index)
        {
        case kBitsPixel: return 32;
        case kPlanes: return 1;
        case kHorzRes: return static_cast<int>(GameScreenWidth());
        case kVertRes: return static_cast<int>(GameScreenHeight());
        case kLogPixelsX: case kLogPixelsY: return 96;
        default: return 0;
        }
    }
}

void SetGameDisplayForGdi(GameDisplay* display)
{
    g_display = display;
    g_presented = false;
}

void RegisterGdi(Win32Layer& layer)
{
#define SHIM(fn) layer.Register("gdi32!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    SHIM(CreateCompatibleDC);
    SHIM(DeleteDC);
    SHIM(CreateDIBSection);
    SHIM(SelectObject);
    SHIM(DeleteObject);
    SHIM(BitBlt);
    SHIM(SetDIBitsToDevice);
    SHIM(GetDeviceCaps);
#undef SHIM
}
