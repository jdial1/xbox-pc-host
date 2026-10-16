#pragma once

// What the Win32 layer needs from the host's screen. The runtime implements it
// over Graphics; the desktop test harness passes none.
class GameDisplay
{
public:
    virtual ~GameDisplay() = default;
    virtual UINT Width() const = 0;
    virtual UINT Height() const = 0;
};
