#pragma once

// Optional per-profile settings for the embedded Microsoft RDP control.
// A value of -1 means that MasterTerm does not override the control/server
// default for options that still use Windows RDP behavior. Visual quality
// switches default to enabled so embedded sessions include font smoothing.
struct RdpConnectionOptions
{
    int colorDepth = -1;             // 16, 24 or 32
    int smartSizing = -1;            // 0/1
    int performanceFlags = -1;       // IMsRdpClientAdvancedSettings mask
    int networkConnectionType = -1;  // 1 modem .. 6 LAN
    int desktopBackground = 1;        // 0/1
    int fontSmoothing = 1;             // 0/1
    int desktopComposition = 1;        // 0/1
    int fullWindowDrag = 1;            // 0/1
    int menuAnimations = 1;            // 0/1
    int visualStyles = 1;              // 0/1
    int cursorShadow = 1;              // 0/1
    int cursorSettings = 1;            // 0/1
    int useMultimon = 1;               // 0/1
    int keyboardHookMode = -1;       // 0 local, 1 fullscreen, 2 remote
    int redirectClipboard = -1;      // 0/1
    int audioRedirectionMode = -1;   // 0 client, 1 none, 2 remote
    int redirectDrives = -1;         // 0/1
    int redirectPrinters = -1;       // 0/1
    int autoReconnect = -1;          // 0/1
    int maxReconnectAttempts = 3;
};
