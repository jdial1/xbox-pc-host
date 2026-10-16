#pragma once

// What the Win32 layer needs from the host's screen. The runtime implements it
// over Graphics; the desktop test harness passes one over an unusable Graphics.
class GameDisplay
{
public:
    virtual ~GameDisplay() = default;
    virtual UINT Width() const = 0;
    virtual UINT Height() const = 0;

    // Creates the game's swapchain on the CoreWindow, first giving up the shell's.
    // Called from the game's thread; the implementation does the work on the UI thread.
    virtual HRESULT CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC1 const& desc, IDXGISwapChain1** swapChain) = 0;
};
