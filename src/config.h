#pragma once

#include <windows.h>

#include <string>

enum class Corner { TopLeft = 0, TopRight, BottomLeft, BottomRight };

// Value ranges enforced by Config::Clamp, which are also the ranges of the settings panel controls.
constexpr int kMaxOffset = 4000;
constexpr float kMinFontScale = 0.5f;
constexpr float kMaxFontScale = 4.0f;
constexpr int kMinUpdateIntervalMs = 100;
constexpr int kMaxUpdateIntervalMs = 5000;

struct Config {
    // [Overlay]
    Corner corner = Corner::TopRight;
    int offsetX = 12;
    int offsetY = 12;
    int monitor = 0;              // 0 = primary, then the remaining monitors in enumeration order
    float fontScale = 1.0f;
    float backgroundAlpha = 0.55f;
    int updateIntervalMs = 500;
    std::string hotkey = "Ctrl+Shift+F12";
    bool hideFromCapture = false;

    // [Metrics]
    bool showFps = true;
    bool showFrameTime = false;
    bool showCpuUsage = true;
    bool showCpuTemp = true;
    bool showGpuUsage = true;
    bool showGpuTemp = true;
    bool showCpuPower = false;
    bool showGpuPower = false;
    bool showVram = false;
    bool showRam = false;
    bool showFrameGraph = false;
    bool fastFrameGraph = false; // redraw the graph ~30 times a second instead of every update interval
    bool fahrenheit = false;
    bool trackGame = false; // keep showing a game's FPS while another app is focused
    bool onlyWithGame = false; // hide the overlay while no game is tracked (needs trackGame)
    bool onlyWhenFocused = false; // ...and while the tracked game is not in the foreground (needs onlyWithGame)

    // [Sensors]
    bool offerPawnIOInstall = true; // ask on startup to install the bundled PawnIO driver

    void Clamp();
};

// config.ini next to the executable.
std::wstring DefaultConfigPath();

// Returns false if the file did not exist (cfg keeps defaults for missing keys either way).
bool LoadConfig(const std::wstring& path, Config& cfg);
bool SaveConfig(const std::wstring& path, const Config& cfg);

const char* CornerToString(Corner c);

// "Ctrl+Shift+F12" -> MOD_CONTROL|MOD_SHIFT, VK_F12. Returns false on unknown syntax.
bool ParseHotkey(const std::string& text, UINT& mods, UINT& vk);
