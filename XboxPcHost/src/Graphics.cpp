#include "pch.h"
#include "Graphics.h"
#include "Log.h"

using namespace winrt;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::Graphics::Display;

void Graphics::Initialize(CoreWindow const& window)
{
    m_window = window;

    check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
        nullptr, m_d2dFactory.put_void()));
    check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<::IUnknown**>(m_dwrite.put())));

    CreateDevice();
    CreateSizeDependentResources();
}

void Graphics::CreateDevice()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // needed for Direct2D interop
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };

    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), &level, context.put());
#if defined(_DEBUG)
    if (FAILED(hr))
    {
        // The debug layer isn't always installed on the console; retry without it.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), &level, context.put());
    }
#endif
    check_hresult(hr);
    m_device = device.as<ID3D11Device3>();
    m_context = context.as<ID3D11DeviceContext3>();

    auto dxgiDevice = m_device.as<IDXGIDevice3>();
    com_ptr<IDXGIAdapter> adapter;
    check_hresult(dxgiDevice->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    m_adapterName = desc.Description;
    HOST_LOG(L"D3D11 device: %s, feature level 0x%x", desc.Description, static_cast<unsigned>(level));

    check_hresult(m_d2dFactory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));
    check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));
    check_hresult(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_textBrush.put()));
    check_hresult(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.85f, 0.2f), m_highlightBrush.put()));
}

void Graphics::CreateSizeDependentResources()
{
    m_d2dContext->SetTarget(nullptr);
    m_d2dTarget = nullptr;
    m_rtv = nullptr;
    m_backBuffer = nullptr;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->Flush();

    // CoreWindow bounds are in view pixels; scale up to real pixels.
    // On Xbox this gives the 1920x1080 app resolution.
    CoreWindow window = m_window.get();
    auto bounds = window.Bounds();
    double scale = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
    m_width = std::max<UINT>(1, static_cast<UINT>(std::lround(bounds.Width * scale)));
    m_height = std::max<UINT>(1, static_cast<UINT>(std::lround(bounds.Height * scale)));

    if (m_swapChain)
    {
        HRESULT hr = m_swapChain->ResizeBuffers(2, m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            HandleDeviceLost();
            return;
        }
        check_hresult(hr);
    }
    else
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        auto dxgiDevice = m_device.as<IDXGIDevice3>();
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(dxgiDevice->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        check_hresult(factory->CreateSwapChainForCoreWindow(m_device.get(),
            winrt::get_unknown(window), &desc, nullptr, m_swapChain.put()));
        // Keep latency low; one queued frame is plenty for a host shell.
        check_hresult(dxgiDevice->SetMaximumFrameLatency(1));
    }

    check_hresult(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), m_backBuffer.put_void()));
    check_hresult(m_device->CreateRenderTargetView(m_backBuffer.get(), nullptr, m_rtv.put()));

    auto surface = m_backBuffer.as<IDXGISurface>();
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(surface.get(), &props, m_d2dTarget.put()));
    m_d2dContext->SetTarget(m_d2dTarget.get());

    // Font size tracks the output height so the status text reads the same at 1080p and 4K.
    m_textFormat = nullptr;
    check_hresult(m_dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, m_height / 36.0f, L"en-us", m_textFormat.put()));

    HOST_LOG(L"Swapchain %ux%u (scale %.2f)", m_width, m_height, scale);
}

bool Graphics::ReleaseForGame()
{
    std::lock_guard lock(m_lock);
    if (!m_swapChain)
        return false;
    m_d2dContext->SetTarget(nullptr);
    m_d2dTarget = nullptr;
    m_rtv = nullptr;
    m_backBuffer = nullptr;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->ClearState();
    m_swapChain = nullptr;
    m_context->Flush(); // destroys the released swapchain now, not later
    m_handedOver = true;
    HOST_LOG(L"shell swapchain released for the game");
    return true;
}

bool Graphics::DrawGamePixels(const uint32_t* pixels, UINT width, UINT height, UINT pitchBytes)
{
    std::lock_guard lock(m_lock);
    if (m_handedOver || !m_swapChain || !pixels || width == 0 || height == 0)
        return false;
    m_gameSurface = true;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    com_ptr<ID2D1Bitmap1> bitmap;
    if (FAILED(m_d2dContext->CreateBitmap(D2D1::SizeU(width, height), pixels, pitchBytes, &props, bitmap.put())))
        return false;

    m_d2dContext->BeginDraw();
    m_d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    m_d2dContext->DrawBitmap(bitmap.get(), D2D1::RectF(0, 0, static_cast<float>(m_width), static_cast<float>(m_height)),
        1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    HRESULT hr = m_d2dContext->EndDraw();
    if (hr != D2DERR_RECREATE_TARGET) check_hresult(hr);

    hr = m_swapChain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        HandleDeviceLost();
    else
        check_hresult(hr);
    return true;
}

void Graphics::ClearGameSurface()
{
    std::lock_guard lock(m_lock);
    m_gameSurface = false;
}

void Graphics::OnWindowSizeChanged()
{
    std::lock_guard lock(m_lock);
    if (m_handedOver) return;
    CreateSizeDependentResources();
}

void Graphics::Trim()
{
    std::lock_guard lock(m_lock);
    if (m_handedOver) return;
    m_context->ClearState();
    m_device.as<IDXGIDevice3>()->Trim();
}

void Graphics::BeginFrame(float r, float g, float b)
{
    std::lock_guard lock(m_lock);
    if (m_handedOver || m_gameSurface) return;
    const float color[4] = { r, g, b, 1.0f };
    ID3D11RenderTargetView* rtv = m_rtv.get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->ClearRenderTargetView(rtv, color);
    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
}

void Graphics::DrawTextLines(std::vector<std::wstring> const& lines, size_t highlight)
{
    std::lock_guard lock(m_lock);
    if (m_handedOver || m_gameSurface) return;
    // TVs can crop up to ~5% on each edge.
    const float marginX = m_width * 0.06f;
    const float marginY = m_height * 0.06f;
    const float lineHeight = m_height / 36.0f * 1.3f;

    m_d2dContext->BeginDraw();
    float y = marginY;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        auto const& line = lines[i];
        D2D1_RECT_F rect{ marginX, y, m_width - marginX, y + lineHeight };
        ID2D1Brush* brush = (i == highlight) ? m_highlightBrush.get() : m_textBrush.get();
        m_d2dContext->DrawText(line.c_str(), static_cast<UINT32>(line.size()), m_textFormat.get(), rect, brush);
        y += lineHeight;
    }
    HRESULT hr = m_d2dContext->EndDraw();
    if (hr != D2DERR_RECREATE_TARGET) check_hresult(hr);
}

void Graphics::Present()
{
    std::lock_guard lock(m_lock);
    if (m_handedOver || m_gameSurface) return;
    HRESULT hr = m_swapChain->Present(1, 0);
    m_context->DiscardView1(m_rtv.get(), nullptr, 0);

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        HandleDeviceLost();
    else
        check_hresult(hr);
}

void Graphics::HandleDeviceLost()
{
    HOST_LOG(L"GPU device lost (reason 0x%08x), recreating", static_cast<unsigned>(m_device->GetDeviceRemovedReason()));
    m_d2dContext->SetTarget(nullptr);
    m_d2dTarget = nullptr;
    m_textBrush = nullptr;
    m_highlightBrush = nullptr;
    m_d2dContext = nullptr;
    m_d2dDevice = nullptr;
    m_rtv = nullptr;
    m_backBuffer = nullptr;
    m_swapChain = nullptr;
    m_context = nullptr;
    m_device = nullptr;

    CreateDevice();
    CreateSizeDependentResources();
}
