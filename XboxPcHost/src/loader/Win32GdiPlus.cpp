#include "pch.h"
#include <wincodec.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// GDI+'s flat API, the part games use as an image codec: decode a PNG or JPEG from a stream, read
// its pixels, and encode a screenshot. GDI+ isn't in the app partition; WIC is, and does the
// decoding and encoding. A bitmap keeps its pixels in its own format (32-bit ARGB, PARGB, RGB or
// 24-bit RGB) and LockBits converts through 32-bit ARGB to whatever the caller asks for.
// Also SHCreateMemStream, which games use to hand GDI+ the file they read.
namespace
{
    using Status = int;
    constexpr Status kOk = 0, kGenericError = 1, kInvalidParameter = 2, kOutOfMemory = 3, kInsufficientBuffer = 5,
        kNotImplemented = 6, kWin32Error = 7, kWrongState = 8, kUnknownImageFormat = 13;

    using PixelFormat = INT;
    constexpr PixelFormat kRgb24 = 0x00021808, kRgb32 = 0x00022009, kArgb32 = 0x0026200A, kPargb32 = 0x000E200B;

    constexpr UINT kLockRead = 1, kLockWrite = 2, kLockUserBuffer = 4;

    struct GpRect { INT x, y, width, height; };
    struct BitmapData
    {
        UINT width, height;
        INT stride;
        PixelFormat format;
        void* scan0;
        UINT_PTR reserved;
    };
    static_assert(sizeof(BitmapData) == 32);

    struct StartupInput
    {
        UINT32 version;
        void* debugEventCallback;
        BOOL suppressBackgroundThread, suppressExternalCodecs;
    };
    struct StartupOutput
    {
        void* notificationHook;
        void* notificationUnhook;
    };

    struct ImageCodecInfo
    {
        CLSID clsid;
        GUID formatId;
        const WCHAR *codecName, *dllName, *formatDescription, *filenameExtension, *mimeType;
        DWORD flags, version, sigCount, sigSize;
        const BYTE *sigPattern, *sigMask;
    };
    static_assert(sizeof(ImageCodecInfo) == 104);

    int Bytes(PixelFormat f) { return f == kRgb24 ? 3 : 4; }
    bool Known(PixelFormat f) { return f == kRgb24 || f == kRgb32 || f == kArgb32 || f == kPargb32; }

    struct Bitmap
    {
        UINT width = 0, height = 0;
        PixelFormat format = kArgb32;
        INT stride = 0;
        BYTE* base = nullptr;        // own.data(), or the caller's buffer from CreateBitmapFromScan0
        std::vector<BYTE> own;
        // An open LockBits
        bool locked = false;
        UINT lockFlags = 0;
        GpRect lockRect{};
        BitmapData lockData{};
        std::vector<BYTE> lockBuffer;

        void Allocate(UINT w, UINT h, PixelFormat f)
        {
            width = w;
            height = h;
            format = f;
            stride = static_cast<INT>((w * Bytes(f) + 3) & ~3u);
            own.assign(static_cast<size_t>(stride) * h, 0);
            base = own.data();
        }
        BYTE* Row(UINT y) const { return base + static_cast<ptrdiff_t>(stride) * y; }
    };

    // One pixel to and from 32-bit ARGB (0xAARRGGBB; in memory B, G, R, A).
    uint32_t ToArgb(const BYTE* p, PixelFormat f)
    {
        switch (f)
        {
        case kRgb24: return 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
        case kRgb32: return 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
        case kPargb32:
        {
            uint32_t a = p[3];
            if (a == 0) return 0;
            auto un = [&](uint32_t c) { return std::min<uint32_t>(255, (c * 255 + a / 2) / a); };
            return (a << 24) | (un(p[2]) << 16) | (un(p[1]) << 8) | un(p[0]);
        }
        default: return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
    }
    void FromArgb(uint32_t c, BYTE* p, PixelFormat f)
    {
        BYTE b = c & 0xFF, g = (c >> 8) & 0xFF, r = (c >> 16) & 0xFF, a = c >> 24;
        switch (f)
        {
        case kRgb24: p[0] = b; p[1] = g; p[2] = r; break;
        case kRgb32: p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF; break;
        case kPargb32:
        {
            auto pm = [&](uint32_t v) { return static_cast<BYTE>((v * a + 127) / 255); };
            p[0] = pm(b); p[1] = pm(g); p[2] = pm(r); p[3] = a;
            break;
        }
        default: p[0] = b; p[1] = g; p[2] = r; p[3] = a;
        }
    }

    // Copies a w x h block between two pixel formats.
    void Convert(const BYTE* from, INT fromStride, PixelFormat fromFormat, BYTE* to, INT toStride, PixelFormat toFormat,
        UINT w, UINT h)
    {
        for (UINT y = 0; y < h; ++y)
        {
            const BYTE* s = from + static_cast<ptrdiff_t>(fromStride) * y;
            BYTE* d = to + static_cast<ptrdiff_t>(toStride) * y;
            if (fromFormat == toFormat)
            {
                memcpy(d, s, static_cast<size_t>(w) * Bytes(fromFormat));
                continue;
            }
            for (UINT x = 0; x < w; ++x)
                FromArgb(ToArgb(s + x * Bytes(fromFormat), fromFormat), d + x * Bytes(toFormat), toFormat);
        }
    }

    // ------------------------------------------------------------------ WIC
    std::mutex g_wicLock;
    winrt::com_ptr<IWICImagingFactory> g_wic;

    IWICImagingFactory* Wic()
    {
        std::lock_guard lock(g_wicLock);
        if (g_wic)
            return g_wic.get();
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(g_wic.put()));
        if (hr == CO_E_NOTINITIALIZED)
        {
            // The game's thread may never have initialized COM (CoInitialize is faked); keep the MTA up.
            CO_MTA_USAGE_COOKIE cookie;
            CoIncrementMTAUsage(&cookie);
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(g_wic.put()));
        }
        if (FAILED(hr))
            HOST_LOG(L"gdiplus: no WIC factory (0x%08X)", static_cast<unsigned>(hr));
        return g_wic.get();
    }

    Status Decode(IStream* stream, Bitmap** out)
    {
        if (!stream || !out) return kInvalidParameter;
        *out = nullptr;
        IWICImagingFactory* wic = Wic();
        if (!wic) return kGenericError;
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::com_ptr<IWICFormatConverter> converter;
        HRESULT hr = wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
        if (FAILED(hr))
        {
            HOST_LOG(L"gdiplus: can't decode the stream (0x%08X)", static_cast<unsigned>(hr));
            return kUnknownImageFormat;
        }
        UINT w = 0, h = 0;
        if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(frame->GetSize(&w, &h)) ||
            FAILED(wic->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                WICBitmapPaletteTypeCustom)))
            return kUnknownImageFormat;
        auto bitmap = std::make_unique<Bitmap>();
        bitmap->Allocate(w, h, kArgb32);
        if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(bitmap->stride), static_cast<UINT>(bitmap->own.size()),
            bitmap->own.data())))
            return kGenericError;
        *out = bitmap.release();
        return kOk;
    }

    // GDI+'s built-in encoders: their class ids end in 1A04-11D3-9A73-0000F81EF32E, and the formats'
    // ids in 0728-11D3-9D7B-0000F81EF32E.
    struct Encoder
    {
        DWORD clsid1, format1;
        const WCHAR *name, *description, *extensions, *mime;
        const GUID* container;
    };
    const Encoder kEncoders[] = {
        { 0x557CF400, 0xB96B3CAB, L"Built-in BMP Codec", L"BMP", L"*.BMP;*.DIB;*.RLE", L"image/bmp", &GUID_ContainerFormatBmp },
        { 0x557CF401, 0xB96B3CAE, L"Built-in JPEG Codec", L"JPEG", L"*.JPG;*.JPEG;*.JPE;*.JFIF", L"image/jpeg", &GUID_ContainerFormatJpeg },
        { 0x557CF402, 0xB96B3CB0, L"Built-in GIF Codec", L"GIF", L"*.GIF", L"image/gif", &GUID_ContainerFormatGif },
        { 0x557CF405, 0xB96B3CB1, L"Built-in TIFF Codec", L"TIFF", L"*.TIF;*.TIFF", L"image/tiff", &GUID_ContainerFormatTiff },
        { 0x557CF406, 0xB96B3CAF, L"Built-in PNG Codec", L"PNG", L"*.PNG", L"image/png", &GUID_ContainerFormatPng },
    };
    constexpr UINT kEncoderCount = static_cast<UINT>(std::size(kEncoders));
    GUID EncoderClsid(DWORD d1) { return { d1, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } }; }
    GUID FormatId(DWORD d1) { return { d1, 0x0728, 0x11D3, { 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } }; }

    size_t Chars(const WCHAR* s) { return (wcslen(s) + 1) * sizeof(WCHAR); }
    UINT EncodersSize()
    {
        size_t size = sizeof(ImageCodecInfo) * kEncoderCount;
        for (auto const& e : kEncoders)
            size += Chars(e.name) + Chars(e.description) + Chars(e.extensions) + Chars(e.mime);
        return static_cast<UINT>(size);
    }

    // ------------------------------------------------------------------ the flat API
    Status WINAPI Shim_GdiplusStartup(ULONG_PTR* token, const StartupInput* input, StartupOutput* output)
    {
        if (!token || !input) return kInvalidParameter;
        *token = 1;
        if (output)
        {
            // With SuppressBackgroundThread the game pumps GDI+'s hooks itself; these do nothing.
            static auto hook = [](ULONG_PTR* t) -> Status { if (t) *t = 1; return kOk; };
            static auto unhook = [](ULONG_PTR) {};
            output->notificationHook = reinterpret_cast<void*>(static_cast<Status(WINAPI*)(ULONG_PTR*)>(hook));
            output->notificationUnhook = reinterpret_cast<void*>(static_cast<void(WINAPI*)(ULONG_PTR)>(unhook));
        }
        return kOk;
    }
    void WINAPI Shim_GdiplusShutdown(ULONG_PTR) {}

    void* WINAPI Shim_GdipAlloc(size_t size) { return malloc(size ? size : 1); }
    void WINAPI Shim_GdipFree(void* p) { free(p); }

    Status WINAPI Shim_GdipCreateBitmapFromStream(IStream* stream, Bitmap** out) { return Decode(stream, out); }

    // With scan0 the bitmap uses the caller's memory as it is, as GDI+ does.
    Status WINAPI Shim_GdipCreateBitmapFromScan0(INT w, INT h, INT stride, PixelFormat format, BYTE* scan0, Bitmap** out)
    {
        if (!out || w <= 0 || h <= 0) return kInvalidParameter;
        if (!Known(format))
        {
            HOST_LOG(L"gdiplus: CreateBitmapFromScan0 pixel format 0x%X not supported", format);
            return kNotImplemented;
        }
        auto bitmap = std::make_unique<Bitmap>();
        if (scan0)
        {
            bitmap->width = static_cast<UINT>(w);
            bitmap->height = static_cast<UINT>(h);
            bitmap->format = format;
            bitmap->stride = stride;
            bitmap->base = scan0;
        }
        else
            bitmap->Allocate(static_cast<UINT>(w), static_cast<UINT>(h), format);
        *out = bitmap.release();
        return kOk;
    }

    Status WINAPI Shim_GdipCloneImage(Bitmap* image, Bitmap** out)
    {
        if (!image || !out) return kInvalidParameter;
        auto copy = std::make_unique<Bitmap>();
        copy->Allocate(image->width, image->height, image->format);
        Convert(image->base, image->stride, image->format, copy->base, copy->stride, copy->format, image->width, image->height);
        *out = copy.release();
        return kOk;
    }

    Status WINAPI Shim_GdipDisposeImage(Bitmap* image)
    {
        delete image;
        return kOk;
    }

    Status WINAPI Shim_GdipGetImageWidth(Bitmap* image, UINT* w)
    {
        if (!image || !w) return kInvalidParameter;
        *w = image->width;
        return kOk;
    }
    Status WINAPI Shim_GdipGetImageHeight(Bitmap* image, UINT* h)
    {
        if (!image || !h) return kInvalidParameter;
        *h = image->height;
        return kOk;
    }
    Status WINAPI Shim_GdipGetImagePixelFormat(Bitmap* image, PixelFormat* format)
    {
        if (!image || !format) return kInvalidParameter;
        *format = image->format;
        return kOk;
    }

    Status WINAPI Shim_GdipBitmapLockBits(Bitmap* bitmap, const GpRect* rect, UINT flags, PixelFormat format, BitmapData* data)
    {
        if (!bitmap || !data) return kInvalidParameter;
        if (bitmap->locked) return kWrongState;
        if (!Known(format))
        {
            HOST_LOG(L"gdiplus: LockBits pixel format 0x%X not supported", format);
            return kNotImplemented;
        }
        GpRect r = rect ? *rect : GpRect{ 0, 0, static_cast<INT>(bitmap->width), static_cast<INT>(bitmap->height) };
        if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 || static_cast<UINT>(r.x + r.width) > bitmap->width ||
            static_cast<UINT>(r.y + r.height) > bitmap->height)
            return kInvalidParameter;
        BYTE* source = bitmap->Row(static_cast<UINT>(r.y)) + r.x * Bytes(bitmap->format);
        BitmapData d{ static_cast<UINT>(r.width), static_cast<UINT>(r.height), 0, format, nullptr, 0 };
        if (flags & kLockUserBuffer)
        {
            d.stride = data->stride;
            d.scan0 = data->scan0;
            if (!d.scan0) return kInvalidParameter;
        }
        else if (format == bitmap->format)
        {
            d.stride = bitmap->stride;
            d.scan0 = source; // the pixels themselves
        }
        else
        {
            d.stride = static_cast<INT>((r.width * Bytes(format) + 3) & ~3);
            bitmap->lockBuffer.assign(static_cast<size_t>(d.stride) * r.height, 0);
            d.scan0 = bitmap->lockBuffer.data();
        }
        if ((flags & kLockRead) && d.scan0 != source)
            Convert(source, bitmap->stride, bitmap->format, static_cast<BYTE*>(d.scan0), d.stride, format,
                static_cast<UINT>(r.width), static_cast<UINT>(r.height));
        bitmap->locked = true;
        bitmap->lockFlags = flags;
        bitmap->lockRect = r;
        bitmap->lockData = d;
        *data = d;
        return kOk;
    }

    Status WINAPI Shim_GdipBitmapUnlockBits(Bitmap* bitmap, BitmapData* data)
    {
        if (!bitmap || !data) return kInvalidParameter;
        if (!bitmap->locked) return kWrongState;
        auto const& d = bitmap->lockData;
        auto const& r = bitmap->lockRect;
        BYTE* target = bitmap->Row(static_cast<UINT>(r.y)) + r.x * Bytes(bitmap->format);
        if ((bitmap->lockFlags & kLockWrite) && d.scan0 != target)
            Convert(static_cast<BYTE*>(d.scan0), d.stride, d.format, target, bitmap->stride, bitmap->format, d.width, d.height);
        bitmap->locked = false;
        bitmap->lockBuffer.clear();
        bitmap->lockBuffer.shrink_to_fit();
        return kOk;
    }

    Status WINAPI Shim_GdipGetImageEncodersSize(UINT* count, UINT* size)
    {
        if (!count || !size) return kInvalidParameter;
        *count = kEncoderCount;
        *size = EncodersSize();
        return kOk;
    }

    // The array, then the strings it points to, in the caller's buffer.
    Status WINAPI Shim_GdipGetImageEncoders(UINT count, UINT size, ImageCodecInfo* out)
    {
        if (!out || count != kEncoderCount || size < EncodersSize()) return kInsufficientBuffer;
        BYTE* strings = reinterpret_cast<BYTE*>(out + kEncoderCount);
        auto put = [&](const WCHAR* s) {
            size_t n = Chars(s);
            memcpy(strings, s, n);
            auto at = reinterpret_cast<const WCHAR*>(strings);
            strings += n;
            return at;
        };
        for (UINT i = 0; i < kEncoderCount; ++i)
        {
            auto const& e = kEncoders[i];
            ImageCodecInfo& info = out[i];
            info = {};
            info.clsid = EncoderClsid(e.clsid1);
            info.formatId = FormatId(e.format1);
            info.codecName = put(e.name);
            info.dllName = nullptr;
            info.formatDescription = put(e.description);
            info.filenameExtension = put(e.extensions);
            info.mimeType = put(e.mime);
            info.flags = 0x1 | 0x2 | 0x4 | 0x10000; // encoder, decoder, supports bitmap, built-in
            info.version = 1;
        }
        return kOk;
    }

    // Encoder parameters (JPEG quality and the like) are ignored; WIC's defaults apply.
    Status WINAPI Shim_GdipSaveImageToStream(Bitmap* image, IStream* stream, const CLSID* encoder, const void*)
    {
        if (!image || !stream || !encoder) return kInvalidParameter;
        const GUID* container = nullptr;
        for (auto const& e : kEncoders)
            if (EncoderClsid(e.clsid1) == *encoder)
                container = e.container;
        if (!container) return kUnknownImageFormat;
        IWICImagingFactory* wic = Wic();
        if (!wic) return kGenericError;
        // WIC takes 32-bit BGRA (= GDI+ ARGB); convert first if the bitmap is in another format.
        std::vector<BYTE> argb;
        BYTE* pixels = image->base;
        INT stride = image->stride;
        if (image->format != kArgb32)
        {
            stride = static_cast<INT>(image->width * 4);
            argb.resize(static_cast<size_t>(stride) * image->height);
            Convert(image->base, image->stride, image->format, argb.data(), stride, kArgb32, image->width, image->height);
            pixels = argb.data();
        }
        winrt::com_ptr<IWICBitmap> source;
        winrt::com_ptr<IWICBitmapEncoder> enc;
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        HRESULT hr = wic->CreateBitmapFromMemory(image->width, image->height, GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(stride),
            static_cast<UINT>(stride) * image->height, pixels, source.put());
        if (SUCCEEDED(hr)) hr = wic->CreateEncoder(*container, nullptr, enc.put());
        if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(frame.put(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(image->width, image->height);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(source.get(), nullptr); // converts to what the encoder takes
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        if (FAILED(hr))
        {
            HOST_LOG(L"gdiplus: SaveImageToStream failed (0x%08X)", static_cast<unsigned>(hr));
            return kWin32Error;
        }
        return kOk;
    }

    // shlwapi's memory stream (ordinal 12): an HGLOBAL stream holding a copy of the data, at its start.
    IStream* WINAPI Shim_SHCreateMemStream(const BYTE* init, UINT size)
    {
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
            return nullptr;
        if (init && size)
        {
            ULONG written = 0;
            if (FAILED(stream->Write(init, size, &written)) || written != size)
            {
                stream->Release();
                return nullptr;
            }
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        }
        return stream;
    }
}

void RegisterGdiPlus(Win32Layer& layer)
{
    auto reg = [&](const char* name, void* fn) { layer.Register((std::string("gdiplus!") + name).c_str(), fn); };
#define FN(f) reinterpret_cast<void*>(&f)
    reg("GdiplusStartup", FN(Shim_GdiplusStartup));
    reg("GdiplusShutdown", FN(Shim_GdiplusShutdown));
    reg("GdipAlloc", FN(Shim_GdipAlloc));
    reg("GdipFree", FN(Shim_GdipFree));
    reg("GdipCreateBitmapFromStream", FN(Shim_GdipCreateBitmapFromStream));
    reg("GdipCreateBitmapFromStreamICM", FN(Shim_GdipCreateBitmapFromStream));
    reg("GdipLoadImageFromStream", FN(Shim_GdipCreateBitmapFromStream));
    reg("GdipCreateBitmapFromScan0", FN(Shim_GdipCreateBitmapFromScan0));
    reg("GdipCloneImage", FN(Shim_GdipCloneImage));
    reg("GdipDisposeImage", FN(Shim_GdipDisposeImage));
    reg("GdipGetImageWidth", FN(Shim_GdipGetImageWidth));
    reg("GdipGetImageHeight", FN(Shim_GdipGetImageHeight));
    reg("GdipGetImagePixelFormat", FN(Shim_GdipGetImagePixelFormat));
    reg("GdipBitmapLockBits", FN(Shim_GdipBitmapLockBits));
    reg("GdipBitmapUnlockBits", FN(Shim_GdipBitmapUnlockBits));
    reg("GdipGetImageEncodersSize", FN(Shim_GdipGetImageEncodersSize));
    reg("GdipGetImageEncoders", FN(Shim_GdipGetImageEncoders));
    reg("GdipSaveImageToStream", FN(Shim_GdipSaveImageToStream));
    layer.Register("shlwapi!SHCreateMemStream", FN(Shim_SHCreateMemStream));
    layer.Register("shlwapi!#12", FN(Shim_SHCreateMemStream));
#undef FN
}
