#pragma once
// Default settings by platform: phones and the Switch start at the game's own
// quality, desktops above it.
#if defined(__ANDROID__) || defined(__SWITCH__)
#define RENUT_DEFAULT(desktop, handheld) handheld
#else
#define RENUT_DEFAULT(desktop, handheld) desktop
#endif

// The Switch's GPU spends most of a frame on 2x MSAA; phones' tilers do it cheaply.
#if defined(__SWITCH__)
#define RENUT_DEFAULT_MSAA "Off"
#else
#define RENUT_DEFAULT_MSAA "Game"
#endif
