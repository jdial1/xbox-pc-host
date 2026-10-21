#include "pch.h"
#include "loader/Win32Layer.h"
#include "loader/GameDisplay.h"
#include "Log.h"

// Just enough gdi32 for a game that draws into a software bitmap and copies
// that to its window (ClassiCube's launcher, OpenTTD's GDI driver). A DIB section is
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
        int bitCount = 32; // 32, or 8 with a color table
        uint8_t* bits = nullptr;
        std::array<uint32_t, 256> colors{}; // BGRX entries for 8-bit bitmaps
        size_t Pitch() const { return (static_cast<size_t>(width) * bitCount + 31) / 32 * 4; } // rows are DWORD aligned
        size_t Bytes() const { return Pitch() * height; }
        const uint8_t* Row(int y) const { return bits + Pitch() * (topDown ? y : height - 1 - y); }
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
        if (bitmap.bitCount == 32 && bitmap.topDown)
        {
            Show(reinterpret_cast<const uint32_t*>(bitmap.bits), bitmap.width, bitmap.height, bitmap.Pitch());
            return;
        }
        // Bottom-up rows are flipped, and 8-bit pixels looked up in the color table, into a top-down copy.
        const size_t w = static_cast<size_t>(bitmap.width);
        std::vector<uint32_t> out(w * bitmap.height);
        for (int y = 0; y < bitmap.height; ++y)
        {
            const uint8_t* row = bitmap.Row(y);
            uint32_t* dst = &out[static_cast<size_t>(y) * w];
            if (bitmap.bitCount == 32)
                memcpy(dst, row, w * 4);
            else
                for (size_t x = 0; x < w; ++x)
                    dst[x] = bitmap.colors[row[x]];
        }
        Show(out.data(), bitmap.width, bitmap.height, w * 4);
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
        // 32-bit BI_RGB/BI_BITFIELDS or 8-bit BI_RGB with a color table: the layouts games use for framebuffers.
        bool supported = (header->bitCount == 32 && (header->compression == 0 || header->compression == 3)) ||
            (header->bitCount == 8 && header->compression == 0);
        if (!supported)
        {
            HOST_LOG(L"CreateDIBSection: unsupported format %u-bit, compression %lu", header->bitCount, header->compression);
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        auto bitmap = std::make_unique<Bitmap>();
        bitmap->width = header->width;
        bitmap->height = std::abs(header->height);
        bitmap->topDown = header->height < 0;
        bitmap->bitCount = header->bitCount;
        if (bitmap->bitCount == 8)
        {
            // The color table follows the header, whichever header version it is.
            auto table = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(header) + header->size);
            size_t count = header->colorsUsed ? std::min<size_t>(header->colorsUsed, 256) : 256;
            for (size_t i = 0; i < count; ++i)
                bitmap->colors[i] = table[i] | 0xFF000000;
        }
        bitmap->bits = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bitmap->Bytes()));
        if (!bitmap->bits)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        if (bits)
            *bits = bitmap->bits;
        HOST_LOG(L"CreateDIBSection %dx%d %d-bit %s", bitmap->width, bitmap->height, bitmap->bitCount,
            bitmap->topDown ? L"top-down" : L"bottom-up");

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

        if (IsWindowDc(destination))
        {
            ShowBitmap(*from->selected);
            return TRUE;
        }
        MemoryDc* to = AsMemoryDc(destination);
        if (!to || !to->selected)
            return FALSE;

        // Copy between two bitmaps of the same depth, clipped to both.
        Bitmap const& src = *from->selected;
        Bitmap& dst = *to->selected;
        if (src.bitCount != dst.bitCount)
            return FALSE;
        const size_t bytes = static_cast<size_t>(src.bitCount / 8);
        for (int row = 0; row < height; ++row)
        {
            int sy = sourceY + row, dy = y + row;
            if (sy < 0 || sy >= src.height || dy < 0 || dy >= dst.height)
                continue;
            int count = std::min({ width, src.width - sourceX, dst.width - x });
            if (count <= 0 || sourceX < 0 || x < 0)
                continue;
            memcpy(const_cast<uint8_t*>(dst.Row(dy)) + x * bytes, src.Row(sy) + sourceX * bytes, count * bytes);
        }
        return TRUE;
    }

    // Draws the caller's pixels straight to the window. The whole image is shown, which is what a
    // game wants when it hands over its complete framebuffer; the destination and source
    // rectangles are ignored.
    int WINAPI Shim_SetDIBitsToDevice(HDC destination, int, int, DWORD, DWORD, int, int, UINT, UINT lines,
        const void* bits, const BmiHeader* header, UINT)
    {
        if (!IsWindowDc(destination) || !bits || !header || header->bitCount != 32 || header->width <= 0 || lines == 0)
            return 0;
        Bitmap view;
        view.width = header->width;
        view.height = static_cast<int>(lines);
        view.topDown = header->height < 0;
        view.bits = const_cast<uint8_t*>(static_cast<const uint8_t*>(bits));
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

namespace
{
    // Replaces part of the color table of the 8-bit bitmap selected into a memory DC.
    UINT WINAPI Shim_SetDIBColorTable(HDC hdc, UINT start, UINT count, const uint32_t* colors)
    {
        std::lock_guard lock(g_lock);
        MemoryDc* dc = AsMemoryDc(hdc);
        if (!dc || !dc->selected || dc->selected->bitCount != 8 || !colors || start >= 256)
            return 0;
        count = std::min(count, 256 - start);
        for (UINT i = 0; i < count; ++i)
            dc->selected->colors[start + i] = colors[i] | 0xFF000000;
        return count;
    }

    // A display DC is the window's; monochrome or DDB bitmaps are only used for icon masks here.
    HDC WINAPI Shim_CreateDCW(LPCWSTR, LPCWSTR, LPCWSTR, const void*) { return GameWindowDc(); }
    HGDIOBJ WINAPI Shim_CreateBitmap(int, int, UINT, UINT, const void*) { return kStockObject; }

    // Palettes only matter on 8-bit displays, and ours is 32-bit: DIB color tables carry the colors.
    HGDIOBJ WINAPI Shim_CreatePalette(const void*) { return kStockObject; }
    HGDIOBJ WINAPI Shim_SelectPalette(HDC, HGDIOBJ, BOOL) { return kStockObject; }
    UINT WINAPI Shim_RealizePalette(HDC) { return 0; }
    HGDIOBJ WINAPI Shim_GetStockObject(int) { return kStockObject; }
    int WINAPI Shim_SetMapMode(HDC, int) { return 1; } // the previous mode, MM_TEXT

    // No GDI fonts: games fall back to their own (OpenTTD to its sprite font).
    int WINAPI Shim_AddFontResourceExW(LPCWSTR, DWORD, PVOID) { return 0; }
    HGDIOBJ WINAPI Shim_CreateFontIndirectW(const void*) { return nullptr; }
    int WINAPI Shim_EnumFontFamiliesExW(HDC, void*, void*, LPARAM, DWORD) { return 0; }
    DWORD WINAPI Shim_GdiError() { return GDI_ERROR; } // GetGlyphIndicesW, GetGlyphOutlineW
    UINT WINAPI Shim_GetOutlineTextMetricsW(HDC, UINT, void*) { return 0; }

    BOOL WINAPI Shim_GdiFalse() { return FALSE; }

    // gdi32's pixel-format calls go to opengl32's wgl ones, as on Windows. The console has no OpenGL of
    // its own, so they work only when the game brings one (Mesa's d3d12 driver); otherwise a game that
    // asks gets no pixel format and moves on.
    template <typename F> F GameOpenGl(const char* name)
    {
        HMODULE gl = GameDllNamed("opengl32");
        return gl ? reinterpret_cast<F>(GameDllExport(gl, name, 0)) : nullptr;
    }
    int WINAPI Shim_ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR* pfd)
    {
        auto f = GameOpenGl<int(WINAPI*)(HDC, const PIXELFORMATDESCRIPTOR*)>("wglChoosePixelFormat");
        return f ? f(dc, pfd) : 0;
    }
    int WINAPI Shim_DescribePixelFormat(HDC dc, int format, UINT size, PIXELFORMATDESCRIPTOR* pfd)
    {
        auto f = GameOpenGl<int(WINAPI*)(HDC, int, UINT, PIXELFORMATDESCRIPTOR*)>("wglDescribePixelFormat");
        return f ? f(dc, format, size, pfd) : 0;
    }
    int WINAPI Shim_GetPixelFormat(HDC dc)
    {
        auto f = GameOpenGl<int(WINAPI*)(HDC)>("wglGetPixelFormat");
        return f ? f(dc) : 0;
    }
    BOOL WINAPI Shim_SetPixelFormat(HDC dc, int format, const PIXELFORMATDESCRIPTOR* pfd)
    {
        auto f = GameOpenGl<BOOL(WINAPI*)(HDC, int, const PIXELFORMATDESCRIPTOR*)>("wglSetPixelFormat");
        return f ? f(dc, format, pfd) : FALSE;
    }
    BOOL WINAPI Shim_SwapBuffers(HDC dc)
    {
        auto f = GameOpenGl<BOOL(WINAPI*)(HDC)>("wglSwapBuffers");
        return f ? f(dc) : FALSE;
    }

    // Only the format query: with no bits, a bitmap's header (and BI_BITFIELDS masks) for its format.
    // SDL finds the desktop's pixel format this way, on a 1x1 compatible bitmap.
    int WINAPI Shim_GetDIBits(HDC, HBITMAP handle, UINT, UINT lines, void* bits, BITMAPINFO* info, UINT)
    {
        if (bits || !info || info->bmiHeader.biSize < sizeof(BITMAPINFOHEADER))
            return 0;
        int width = 1, height = 1, bitCount = 32;
        {
            std::lock_guard lock(g_lock);
            if (Bitmap* b = AsBitmap(handle))
            {
                width = b->width;
                height = b->height;
                bitCount = b->bitCount;
            }
        }
        auto& h = info->bmiHeader;
        h.biWidth = width;
        h.biHeight = height;
        h.biPlanes = 1;
        h.biBitCount = static_cast<WORD>(bitCount);
        h.biSizeImage = static_cast<DWORD>((width * bitCount + 31) / 32 * 4 * height);
        if (bitCount == 32)
        {
            h.biCompression = BI_BITFIELDS;
            auto masks = reinterpret_cast<DWORD*>(info->bmiColors);
            masks[0] = 0x00FF0000;
            masks[1] = 0x0000FF00;
            masks[2] = 0x000000FF;
        }
        else
            h.biCompression = BI_RGB;
        return static_cast<int>(lines ? lines : 1);
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
    SHIM(SetDIBColorTable);
    SHIM(CreateDCW);
    SHIM(CreateBitmap);
    layer.Register("gdi32!CreateDCA", reinterpret_cast<void*>(&Shim_CreateDCW));
    layer.Register("gdi32!CreateCompatibleBitmap", reinterpret_cast<void*>(&Shim_CreateBitmap));
    SHIM(CreatePalette);
    SHIM(SelectPalette);
    SHIM(RealizePalette);
    SHIM(GetStockObject);
    SHIM(SetMapMode);
    SHIM(AddFontResourceExW);
    SHIM(CreateFontIndirectW);
    SHIM(EnumFontFamiliesExW);
    SHIM(GetOutlineTextMetricsW);
    SHIM(ChoosePixelFormat);
    SHIM(DescribePixelFormat);
    SHIM(GetPixelFormat);
    SHIM(SetPixelFormat);
    SHIM(SwapBuffers);
    SHIM(GetDIBits);
#undef SHIM
    layer.Register("gdi32!GetGlyphIndicesW", reinterpret_cast<void*>(&Shim_GdiError));
    layer.Register("gdi32!GetGlyphOutlineW", reinterpret_cast<void*>(&Shim_GdiError));
}
