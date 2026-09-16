#pragma once

#include <windows.h>

struct RdpOcclusionRegion
{
    RECT bounds{};
    LONG cornerRadius = 0;
    bool topSquareBottomRounded = false;
    // This region is a visual overlay rather than an HTML hole.  The native
    // RDP surface must stay present underneath it so the host can render a
    // real per-pixel-alpha gradient above the desktop.
    bool nativeGradientShadow = false;
};
