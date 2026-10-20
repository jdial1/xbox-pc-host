#include "pch.h"
#include <dxgi1_6.h>
#include "loader/Win32Layer.h"
#include "Log.h"

// The one DXGI output a game sees: the screen the CoreWindow covers. A console adapter (and a
// PC's GPU under remote desktop) may have no outputs a UWP app can use, yet games enumerate one to
// find their display modes and fail without it. This one is the same monitor user32's shims report
// (\\.\DISPLAY1, the output's size, 32-bit at 60 Hz); it never owns the screen or changes modes.
namespace
{
    // Every format a swapchain can present, each in the one mode there is.
    constexpr DXGI_FORMAT kFormats[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };

    bool Presentable(DXGI_FORMAT f) { return std::find(std::begin(kFormats), std::end(kFormats), f) != std::end(kFormats); }

    DXGI_MODE_DESC Mode(DXGI_FORMAT format)
    {
        DXGI_MODE_DESC m{};
        m.Width = GameScreenWidth();
        m.Height = GameScreenHeight();
        m.RefreshRate = { 60, 1 };
        m.Format = format;
        m.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
        m.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        return m;
    }

    DXGI_MODE_DESC1 Mode1(DXGI_FORMAT format)
    {
        DXGI_MODE_DESC m = Mode(format);
        return { m.Width, m.Height, m.RefreshRate, m.Format, m.ScanlineOrdering, m.Scaling, FALSE };
    }

    template <typename M> HRESULT ModeList(DXGI_FORMAT format, UINT* count, M* modes, M (*make)(DXGI_FORMAT))
    {
        if (!count) return DXGI_ERROR_INVALID_CALL;
        UINT n = Presentable(format) ? 1 : 0;
        if (!modes)
        {
            *count = n;
            return S_OK;
        }
        if (n && *count < n)
            return DXGI_ERROR_MORE_DATA;
        if (n)
            modes[0] = make(format);
        *count = n;
        return S_OK;
    }

    struct GameOutput : winrt::implements<GameOutput, IDXGIOutput6>
    {
        explicit GameOutput(IDXGIAdapter* adapter) { m_adapter.copy_from(adapter); }

        // IDXGIObject
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override
        {
            if (size) *size = 0;
            return DXGI_ERROR_NOT_FOUND;
        }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** out) override
        {
            if (!out) return E_POINTER;
            *out = nullptr;
            return m_adapter ? m_adapter->QueryInterface(iid, out) : E_NOINTERFACE;
        }

        // IDXGIOutput
        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* desc) override
        {
            if (!desc) return E_INVALIDARG;
            *desc = {};
            wcscpy_s(desc->DeviceName, L"\\\\.\\DISPLAY1");
            desc->DesktopCoordinates = { 0, 0, static_cast<LONG>(GameScreenWidth()), static_cast<LONG>(GameScreenHeight()) };
            desc->AttachedToDesktop = TRUE;
            desc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
            desc->Monitor = GameMonitor();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT format, UINT, UINT* count, DXGI_MODE_DESC* modes) override
        {
            return ModeList(format, count, modes, &Mode);
        }
        HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* want, DXGI_MODE_DESC* closest, IUnknown*) override
        {
            if (!want || !closest) return DXGI_ERROR_INVALID_CALL;
            *closest = Mode(Presentable(want->Format) ? want->Format : DXGI_FORMAT_B8G8R8A8_UNORM);
            return S_OK;
        }
        // The next 60 Hz tick.
        HRESULT STDMETHODCALLTYPE WaitForVBlank() override
        {
            ULONGLONG now = GetTickCount64();
            Sleep(static_cast<DWORD>(16 - now % 16 + 1));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown*, BOOL) override { return S_OK; }
        void STDMETHODCALLTYPE ReleaseOwnership() override {}
        HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return DXGI_ERROR_UNSUPPORTED; }

        // IDXGIOutput1
        HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT format, UINT, UINT* count, DXGI_MODE_DESC1* modes) override
        {
            return ModeList(format, count, modes, &Mode1);
        }
        HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* want, DXGI_MODE_DESC1* closest, IUnknown*) override
        {
            if (!want || !closest) return DXGI_ERROR_INVALID_CALL;
            *closest = Mode1(Presentable(want->Format) ? want->Format : DXGI_FORMAT_B8G8R8A8_UNORM);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource*) override { return DXGI_ERROR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown*, IDXGIOutputDuplication** out) override
        {
            if (out) *out = nullptr;
            return DXGI_ERROR_UNSUPPORTED;
        }

        // IDXGIOutput2-4: no overlays
        BOOL STDMETHODCALLTYPE SupportsOverlays() override { return FALSE; }
        HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT, IUnknown*, UINT* flags) override
        {
            if (flags) *flags = 0;
            return DXGI_ERROR_UNSUPPORTED;
        }
        HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT, DXGI_COLOR_SPACE_TYPE, IUnknown*, UINT* flags) override
        {
            if (flags) *flags = 0;
            return DXGI_ERROR_UNSUPPORTED;
        }

        // IDXGIOutput5
        HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown*, UINT, UINT, const DXGI_FORMAT*, IDXGIOutputDuplication** out) override
        {
            if (out) *out = nullptr;
            return DXGI_ERROR_UNSUPPORTED;
        }

        // IDXGIOutput6: an SDR sRGB screen
        HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* desc) override
        {
            if (!desc) return E_INVALIDARG;
            DXGI_OUTPUT_DESC d;
            GetDesc(&d);
            *desc = {};
            memcpy(desc->DeviceName, d.DeviceName, sizeof(d.DeviceName));
            desc->DesktopCoordinates = d.DesktopCoordinates;
            desc->AttachedToDesktop = d.AttachedToDesktop;
            desc->Rotation = d.Rotation;
            desc->Monitor = d.Monitor;
            desc->BitsPerColor = 8;
            desc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            desc->RedPrimary[0] = 0.64f; desc->RedPrimary[1] = 0.33f;
            desc->GreenPrimary[0] = 0.30f; desc->GreenPrimary[1] = 0.60f;
            desc->BluePrimary[0] = 0.15f; desc->BluePrimary[1] = 0.06f;
            desc->WhitePoint[0] = 0.3127f; desc->WhitePoint[1] = 0.3290f;
            desc->MinLuminance = 0.5f;
            desc->MaxLuminance = 270.0f;
            desc->MaxFullFrameLuminance = 270.0f;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* flags) override
        {
            if (flags) *flags = 0;
            return S_OK;
        }

    private:
        winrt::com_ptr<IDXGIAdapter> m_adapter;
    };
}

// The object answers QueryInterface for every interface IDXGIOutput6 is built on.
template <> bool winrt::is_guid_of<IDXGIOutput6>(guid const& id) noexcept
{
    return winrt::is_guid_of<IDXGIOutput6, IDXGIOutput5, IDXGIOutput4, IDXGIOutput3, IDXGIOutput2, IDXGIOutput1, IDXGIOutput, IDXGIObject>(id);
}

IDXGIOutput* MakeGameOutput(IDXGIAdapter* adapter)
{
    auto output = winrt::make_self<GameOutput>(adapter);
    IDXGIOutput* raw = nullptr;
    output.as<IDXGIOutput>().copy_to(&raw);
    return raw;
}
