#include "config.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::string& def) {
    wchar_t buf[256];
    DWORD n = GetPrivateProfileStringW(section, key, L"\x01", buf, (DWORD)std::size(buf), path.c_str());
    if (n == 1 && buf[0] == L'\x01') return def;
    std::string out;
    for (DWORD i = 0; i < n; ++i) out.push_back(buf[i] < 128 ? (char)buf[i] : '?');
    // Strip inline comments ("Corner=TopRight ; comment").
    size_t semi = out.find(';');
    if (semi != std::string::npos) out.resize(semi);
    return Trim(out);
}

int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
    std::string s = ReadString(path, section, key, "");
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

float ReadFloat(const std::wstring& path, const wchar_t* section, const wchar_t* key, float def) {
    std::string s = ReadString(path, section, key, "");
    if (s.empty()) return def;
    try { return std::stof(s); } catch (...) { return def; }
}

bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool def) {
    std::string s = ToLower(ReadString(path, section, key, ""));
    if (s.empty()) return def;
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

Corner ParseCorner(const std::string& s, Corner def) {
    std::string l = ToLower(s);
    if (l == "topleft") return Corner::TopLeft;
    if (l == "topright") return Corner::TopRight;
    if (l == "bottomleft") return Corner::BottomLeft;
    if (l == "bottomright") return Corner::BottomRight;
    return def;
}

} // namespace

void Config::Clamp() {
    offsetX = std::clamp(offsetX, 0, kMaxOffset);
    offsetY = std::clamp(offsetY, 0, kMaxOffset);
    monitor = std::max(monitor, 0);
    fontScale = std::clamp(fontScale, kMinFontScale, kMaxFontScale);
    backgroundAlpha = std::clamp(backgroundAlpha, 0.0f, 1.0f);
    updateIntervalMs = std::clamp(updateIntervalMs, kMinUpdateIntervalMs, kMaxUpdateIntervalMs);
    onlyWithGame = onlyWithGame && trackGame;
    onlyWhenFocused = onlyWhenFocused && onlyWithGame;
}

std::wstring DefaultConfigPath() { return AppDirectory() + L"config.ini"; }

const char* CornerToString(Corner c) {
    switch (c) {
    case Corner::TopLeft: return "TopLeft";
    case Corner::TopRight: return "TopRight";
    case Corner::BottomLeft: return "BottomLeft";
    case Corner::BottomRight: return "BottomRight";
    }
    return "TopRight";
}

bool LoadConfig(const std::wstring& path, Config& cfg) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    const wchar_t* ov = L"Overlay";
    cfg.corner = ParseCorner(ReadString(path, ov, L"Corner", ""), cfg.corner);
    cfg.offsetX = ReadInt(path, ov, L"OffsetX", cfg.offsetX);
    cfg.offsetY = ReadInt(path, ov, L"OffsetY", cfg.offsetY);
    cfg.monitor = ReadInt(path, ov, L"Monitor", cfg.monitor);
    cfg.fontScale = ReadFloat(path, ov, L"FontScale", cfg.fontScale);
    cfg.backgroundAlpha = ReadFloat(path, ov, L"BackgroundAlpha", cfg.backgroundAlpha);
    cfg.updateIntervalMs = ReadInt(path, ov, L"UpdateIntervalMs", cfg.updateIntervalMs);
    cfg.hotkey = ReadString(path, ov, L"Hotkey", cfg.hotkey);
    cfg.hideFromCapture = ReadBool(path, ov, L"HideFromCapture", cfg.hideFromCapture);

    const wchar_t* me = L"Metrics";
    cfg.showFps = ReadBool(path, me, L"ShowFps", cfg.showFps);
    cfg.showFrameTime = ReadBool(path, me, L"ShowFrameTime", cfg.showFrameTime);
    cfg.showCpuUsage = ReadBool(path, me, L"ShowCpuUsage", cfg.showCpuUsage);
    cfg.showCpuTemp = ReadBool(path, me, L"ShowCpuTemp", cfg.showCpuTemp);
    cfg.showGpuUsage = ReadBool(path, me, L"ShowGpuUsage", cfg.showGpuUsage);
    cfg.showGpuTemp = ReadBool(path, me, L"ShowGpuTemp", cfg.showGpuTemp);
    cfg.showCpuPower = ReadBool(path, me, L"ShowCpuPower", cfg.showCpuPower);
    cfg.showGpuPower = ReadBool(path, me, L"ShowGpuPower", cfg.showGpuPower);
    cfg.showVram = ReadBool(path, me, L"ShowVram", cfg.showVram);
    cfg.showRam = ReadBool(path, me, L"ShowRam", cfg.showRam);
    cfg.showFrameGraph = ReadBool(path, me, L"ShowFrameTimeGraph", cfg.showFrameGraph);
    cfg.fastFrameGraph = ReadBool(path, me, L"FastFrameTimeGraph", cfg.fastFrameGraph);
    cfg.fahrenheit = ToLower(ReadString(path, me, L"TemperatureUnit", cfg.fahrenheit ? "F" : "C")) == "f";
    cfg.trackGame = ReadBool(path, me, L"TrackGame", cfg.trackGame);
    cfg.onlyWithGame = ReadBool(path, me, L"ShowOnlyWithGame", cfg.onlyWithGame);
    cfg.onlyWhenFocused = ReadBool(path, me, L"ShowOnlyWhenGameFocused", cfg.onlyWhenFocused);

    cfg.offerPawnIOInstall = ReadBool(path, L"Sensors", L"OfferPawnIOInstall", cfg.offerPawnIOInstall);

    cfg.Clamp();
    return true;
}

bool SaveConfig(const std::wstring& path, const Config& cfg) {
    char buf[4096];
    int len = std::snprintf(buf, sizeof(buf),
        "; FPS Monitor configuration. Also editable from the settings panel (hotkey or tray icon).\r\n"
        "\r\n"
        "[Overlay]\r\n"
        "; TopLeft | TopRight | BottomLeft | BottomRight\r\n"
        "Corner=%s\r\n"
        "OffsetX=%d\r\n"
        "OffsetY=%d\r\n"
        "; 0 = primary monitor, 1.. = other monitors\r\n"
        "Monitor=%d\r\n"
        "FontScale=%.2f\r\n"
        "BackgroundAlpha=%.2f\r\n"
        "UpdateIntervalMs=%d\r\n"
        "; Modifiers: Ctrl, Shift, Alt, Win. Keys: F1-F24, A-Z, 0-9, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock\r\n"
        "Hotkey=%s\r\n"
        "; 1 = hide the overlay from screenshots / recordings / streams\r\n"
        "HideFromCapture=%d\r\n"
        "\r\n"
        "[Metrics]\r\n"
        "ShowFps=%d\r\n"
        "ShowFrameTime=%d\r\n"
        "ShowCpuUsage=%d\r\n"
        "ShowCpuTemp=%d\r\n"
        "ShowGpuUsage=%d\r\n"
        "ShowGpuTemp=%d\r\n"
        "ShowCpuPower=%d\r\n"
        "ShowGpuPower=%d\r\n"
        "ShowVram=%d\r\n"
        "ShowRam=%d\r\n"
        "ShowFrameTimeGraph=%d\r\n"
        "; 1 = redraw the frame time graph about 30 times a second, scrolling smoothly (other values keep UpdateIntervalMs)\r\n"
        "FastFrameTimeGraph=%d\r\n"
        "; C | F\r\n"
        "TemperatureUnit=%s\r\n"
        "; 1 = keep showing a full-screen game's FPS while another app is focused\r\n"
        "TrackGame=%d\r\n"
        "; 1 = only show the overlay while a full-screen game is detected (needs TrackGame=1)\r\n"
        "ShowOnlyWithGame=%d\r\n"
        "; 1 = ...and only while that game is focused (needs ShowOnlyWithGame=1)\r\n"
        "ShowOnlyWhenGameFocused=%d\r\n"
        "\r\n"
        "[Sensors]\r\n"
        "; 1 = offer to install the bundled PawnIO driver (needed for CPU temperature and power) on startup\r\n"
        "OfferPawnIOInstall=%d\r\n",
        CornerToString(cfg.corner), cfg.offsetX, cfg.offsetY, cfg.monitor, cfg.fontScale, cfg.backgroundAlpha,
        cfg.updateIntervalMs, cfg.hotkey.c_str(), cfg.hideFromCapture ? 1 : 0,
        cfg.showFps ? 1 : 0, cfg.showFrameTime ? 1 : 0, cfg.showCpuUsage ? 1 : 0, cfg.showCpuTemp ? 1 : 0,
        cfg.showGpuUsage ? 1 : 0, cfg.showGpuTemp ? 1 : 0, cfg.showCpuPower ? 1 : 0, cfg.showGpuPower ? 1 : 0,
        cfg.showVram ? 1 : 0, cfg.showRam ? 1 : 0, cfg.showFrameGraph ? 1 : 0, cfg.fastFrameGraph ? 1 : 0,
        cfg.fahrenheit ? "F" : "C", cfg.trackGame ? 1 : 0,
        cfg.onlyWithGame ? 1 : 0, cfg.onlyWhenFocused ? 1 : 0, cfg.offerPawnIOInstall ? 1 : 0);
    if (len <= 0 || len >= (int)sizeof(buf)) return false;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, buf, (DWORD)len, &written, nullptr);
    CloseHandle(f);
    return ok && written == (DWORD)len;
}

bool ParseHotkey(const std::string& text, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    std::stringstream ss(text);
    std::string tok;
    std::vector<std::string> parts;
    while (std::getline(ss, tok, '+')) {
        tok = ToLower(Trim(tok));
        if (!tok.empty()) parts.push_back(tok);
    }
    if (parts.empty()) return false;

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string& m = parts[i];
        if (m == "ctrl" || m == "control") mods |= MOD_CONTROL;
        else if (m == "shift") mods |= MOD_SHIFT;
        else if (m == "alt") mods |= MOD_ALT;
        else if (m == "win") mods |= MOD_WIN;
        else return false;
    }

    const std::string& k = parts.back();
    if (k.size() == 1 && std::isalnum((unsigned char)k[0])) {
        vk = (UINT)std::toupper((unsigned char)k[0]);
    } else if (k.size() >= 2 && k[0] == 'f' &&
               std::all_of(k.begin() + 1, k.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        int n = std::atoi(k.c_str() + 1);
        if (n < 1 || n > 24) return false;
        vk = VK_F1 + (n - 1);
    } else {
        static const struct { const char* name; UINT vk; } named[] = {
            {"insert", VK_INSERT}, {"delete", VK_DELETE}, {"home", VK_HOME}, {"end", VK_END},
            {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"pause", VK_PAUSE}, {"scrolllock", VK_SCROLL},
        };
        for (const auto& n : named)
            if (k == n.name) vk = n.vk;
        if (!vk) return false;
    }
    return true;
}
