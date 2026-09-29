#pragma once

#include "config.h"
#include "overlay_window.h"
#include "sensor_hub.h"

#include <imgui.h>

#include <string>
#include <vector>

enum class UiAction { None, CloseSettings, ExitApp, InstallPawnIO };

struct SettingsState {
    bool open = false;
    char hotkeyBuf[64] = {};
    bool pawnioInstalling = false;
    std::string pawnioMessage; // result of the last install attempt
};

// Pivot of the stats panel for a corner: (0,0) = top-left ... (1,1) = bottom-right.
ImVec2 CornerPivot(Corner c);

// The always-visible stats panel, placed at `pos` (client coordinates) with `pivot`.
// Returns the panel size (zero if every metric is hidden).
ImVec2 DrawStats(const Config& cfg, const SensorSnapshot& snap, ImVec2 pos, ImVec2 pivot);

// The settings panel (only while the overlay is interactive). Edits `cfg` live.
UiAction DrawSettings(Config& cfg, const SensorSnapshot& snap, const std::vector<MonitorInfo>& monitors,
                      SettingsState& state);
