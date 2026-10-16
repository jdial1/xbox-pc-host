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

    // Shows a software framebuffer (opaque BGRA pixels, row pitch in bytes). The pixels are
    // copied, so the caller may reuse its buffer at once. Called from the game's thread.
    virtual void PresentPixels(const uint32_t* bgra, UINT width, UINT height, UINT pitchBytes) = 0;

    // The game has ended; whatever it was drawing is no longer the screen's owner.
    virtual void GameEnded() = 0;
};
