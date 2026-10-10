#pragma once

// Owns the D3D11 device and the CoreWindow swapchain. A game's DirectX output
// will eventually be routed into BackBuffer() (stage 4); for now the shell
// draws its own status screen with Direct2D on top of it.
class Graphics
{
public:
    void Initialize(winrt::Windows::UI::Core::CoreWindow const& window);
    void OnWindowSizeChanged();
    void Trim();

    // Clears the back buffer and binds it as the render target.
    void BeginFrame(float r, float g, float b);
    // Draws lines of text top-left, inside the TV-safe area.
    void DrawTextLines(std::vector<std::wstring> const& lines, size_t highlight = SIZE_MAX);
    // Presents with vsync. Recreates everything if the device was lost.
    void Present();

    ID3D11Device3* Device() const { return m_device.get(); }
    ID3D11DeviceContext3* Context() const { return m_context.get(); }
    IDXGISwapChain1* SwapChain() const { return m_swapChain.get(); }
    ID3D11Texture2D* BackBuffer() const { return m_backBuffer.get(); }
    ID3D11RenderTargetView* BackBufferView() const { return m_rtv.get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    std::wstring const& AdapterName() const { return m_adapterName; }

private:
    void CreateDevice();
    void CreateSizeDependentResources();
    void HandleDeviceLost();

    winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> m_window;

    winrt::com_ptr<ID3D11Device3> m_device;
    winrt::com_ptr<ID3D11DeviceContext3> m_context;
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    winrt::com_ptr<ID3D11Texture2D> m_backBuffer;
    winrt::com_ptr<ID3D11RenderTargetView> m_rtv;

    winrt::com_ptr<ID2D1Factory1> m_d2dFactory;
    winrt::com_ptr<ID2D1Device> m_d2dDevice;
    winrt::com_ptr<ID2D1DeviceContext> m_d2dContext;
    winrt::com_ptr<ID2D1Bitmap1> m_d2dTarget;
    winrt::com_ptr<IDWriteFactory> m_dwrite;
    winrt::com_ptr<IDWriteTextFormat> m_textFormat;
    winrt::com_ptr<ID2D1SolidColorBrush> m_textBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_highlightBrush;

    UINT m_width = 0;
    UINT m_height = 0;
    std::wstring m_adapterName;
};
