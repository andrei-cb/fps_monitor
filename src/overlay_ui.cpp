#include "overlay_ui.h"

#include "pawnio_setup.h"
#include "util.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

const ImVec4 kFpsColor(0.56f, 0.87f, 0.36f, 1.0f);
const ImVec4 kCpuColor(0.38f, 0.74f, 1.0f, 1.0f);
const ImVec4 kGpuColor(1.0f, 0.62f, 0.26f, 1.0f);
const ImVec4 kRamColor(0.78f, 0.6f, 1.0f, 1.0f);
const ImVec4 kValueColor(0.96f, 0.96f, 0.96f, 1.0f);
const ImVec4 kMissingColor(0.6f, 0.6f, 0.6f, 1.0f);
const ImVec4 kWarmColor(1.0f, 0.85f, 0.3f, 1.0f);
const ImVec4 kHotColor(1.0f, 0.35f, 0.3f, 1.0f);

// How far behind real time the frame graph is drawn. Fast graph updates flush ETW every ~33-47 ms,
// so frames have normally arrived by then; otherwise the newest part of the line appears late.
constexpr double kGraphDelayMs = 120.0;
// Height of the frame graph's 0 ms line, as a fraction of the graph height from the bottom.
constexpr float kGraphZero = 0.1f;
// Frame time at the top of the frame graph.
constexpr float kGraphMaxMs = 60.0f;

// Widest text a memory row can show; sizes the panel so it does not jitter.
const char* const kMemorySample = "000.0 / 000.0 GB";

struct Cell {
    std::string text;
    ImVec4 color;
};

struct Row {
    const char* label = "";
    ImVec4 labelColor;
    Cell cells[3];
    int count = 0;
    bool span = false; // a single value right-aligned across all value columns (memory rows)

    void Add(std::string text, ImVec4 color) { cells[count++] = {std::move(text), color}; }
};

std::string Format(const char* fmt, float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

void AddUsage(Row& row, const std::optional<float>& v) {
    if (v) row.Add(Format("%.0f%%", *v), kValueColor);
    else row.Add("N/A", kMissingColor);
}

void AddTemp(Row& row, const std::optional<float>& celsius, bool fahrenheit) {
    if (!celsius) {
        row.Add("N/A", kMissingColor);
        return;
    }
    const float c = *celsius;
    const ImVec4 color = c >= 85.0f ? kHotColor : c >= 70.0f ? kWarmColor : kValueColor;
    row.Add(fahrenheit ? Format("%.0f\xC2\xB0""F", c * 9.0f / 5.0f + 32.0f) : Format("%.0f\xC2\xB0""C", c), color);
}

void AddPower(Row& row, const std::optional<float>& watts) {
    if (watts) row.Add(Format("%.0f W", *watts), kValueColor);
    else row.Add("N/A", kMissingColor);
}

Row MemoryRow(const char* label, ImVec4 labelColor, const std::optional<float>& usedGB, const std::optional<float>& totalGB) {
    Row r;
    r.label = label;
    r.labelColor = labelColor;
    r.span = true;
    if (!usedGB || !totalGB || *totalGB <= 0.0f) {
        r.Add("N/A", kMissingColor);
        return r;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.1f / %.1f GB", *usedGB, *totalGB);
    r.Add(buf, *usedGB / *totalGB >= 0.9f ? kWarmColor : kValueColor);
    return r;
}

// Frame times over the last kFrameGraphSeconds, newest at the right edge. Each frame spans its
// duration on the time axis, and a pixel column shows the slowest frame in it, so spikes stay visible.
void DrawFrameGraph(const SensorSnapshot& snap, float width) {
    const float fontSize = ImGui::GetFontSize();
    const float height = std::round(fontSize * 2.5f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 16), ImGui::GetStyle().FrameRounding);

    const std::vector<float>& frames = snap.frameTimesMs;
    if (frames.empty() || width < 2.0f) return;

    // The graph scrolls with the clock (not with data arrival, which comes in bursts), slightly
    // behind real time so that ETW has delivered the frames by the time they are drawn.
    const double msPerTick = 1000.0 / double(QpcFrequency());
    const double msPerPx = kFrameGraphSeconds * 1000.0 / width;
    const double rightMs = double(Qpc()) * msPerTick - kGraphDelayMs;
    const double leftMs = rightMs - kFrameGraphSeconds * 1000.0;

    // When each frame ended, oldest first.
    const size_t n = frames.size();
    std::vector<double> ends(n);
    double end = double(snap.lastFrameQpc) * msPerTick;
    for (size_t i = n; i-- > 0;) {
        ends[i] = end;
        end -= frames[i];
    }

    // Each frame's stutter limit: twice the average of the normal (non-stutter) frames in the second
    // before it. Leaving stutters out keeps one long hitch from inflating the limit for the next
    // second. If the frame rate drops for good, the reference runs out of normal frames after a
    // second and restarts from the new frame rate. Only earlier frames count, so a frame keeps its
    // color while it scrolls across the graph.
    std::vector<float> limits(n, 0.0f); // 0 = nothing to compare with
    std::vector<char> stutters(n, 0);
    double normalSum = 0.0;
    int normalCount = 0;
    for (size_t i = 0, first = 0; i < n; ++i) {
        const double start = ends[i] - frames[i];
        for (; first < i && ends[first] <= start - kStutterWindowSeconds * 1000.0; ++first) {
            if (stutters[first]) continue;
            normalSum -= frames[first];
            --normalCount;
        }
        if (normalCount > 0) limits[i] = float(2.0 * normalSum / normalCount);
        stutters[i] = limits[i] > 0.0f && frames[i] > limits[i];
        if (!stutters[i]) {
            normalSum += frames[i];
            ++normalCount;
        }
    }

    // Columns sit on a fixed time grid, so a frame stays in the same column as the graph scrolls,
    // and the grid is drawn at a sub-pixel offset: the line slides instead of shimmering. A column
    // shows its slowest frame (so spikes stay visible), and is a stutter if any frame in it is.
    struct Column {
        float slowest = 0.0f; // 0 = no frame
        bool stutter = false;
    };
    const int64_t firstCol = (int64_t)std::floor(leftMs / msPerPx);
    const int64_t lastCol = (int64_t)std::floor(rightMs / msPerPx);
    std::vector<Column> cols(size_t(lastCol - firstCol + 1));
    int counted = 0;
    for (size_t i = n; i-- > 0 && ends[i] > leftMs;) {
        const double start = ends[i] - frames[i];
        if (start >= rightMs) continue;
        const bool stutter = stutters[i] != 0;
        const int64_t c0 = std::max(firstCol, (int64_t)std::floor(start / msPerPx));
        const int64_t c1 = std::min(lastCol, (int64_t)std::ceil(ends[i] / msPerPx) - 1);
        for (int64_t c = c0; c <= c1; ++c) {
            Column& col = cols[size_t(c - firstCol)];
            col.slowest = std::max(col.slowest, frames[i]);
            col.stutter |= stutter;
        }
        ++counted;
    }
    if (counted == 0) return;
    const double colOffset = leftMs / msPerPx; // x of column c is c - colOffset
    auto columnX = [&](size_t i) { return p0.x + float(double(firstCol + int64_t(i)) + 0.5 - colOffset); };

    // A fixed scale, so the line never jumps; slower frames are drawn at the top edge.
    const float top = kGraphMaxMs;

    // 0 ms sits a bit above the bottom edge (kGraphZero of the height); the scale runs above it.
    const float thickness = std::max(1.0f, fontSize / 12.0f);
    const float zeroY = p1.y - thickness * 0.5f - (height - thickness) * kGraphZero;
    const float usable = zeroY - (p0.y + thickness * 0.5f);
    auto valueY = [&](float ms) { return zeroY - std::min(ms / top, 1.0f) * usable; };

    const ImU32 gridColor = IM_COL32(255, 255, 255, 30);
    const float midY = std::round(valueY(top * 0.5f));
    draw->AddLine(ImVec2(p0.x, midY), ImVec2(p1.x, midY), gridColor);
    char label[16];
    std::snprintf(label, sizeof(label), "%.0f ms", top);
    const float pad = std::round(fontSize * 0.15f);
    draw->AddText(nullptr, fontSize * 0.7f, ImVec2(p0.x + pad * 2.0f, p0.y + pad), ImGui::GetColorU32(kMissingColor), label);

    draw->PushClipRect(p0, p1, true); // the partial columns at both ends

    // The frame times; a segment is red when either end is a stutter (both edges of a spike).
    for (size_t i = 1; i < cols.size(); ++i) {
        const Column& a = cols[i - 1];
        const Column& b = cols[i];
        if (a.slowest <= 0.0f || b.slowest <= 0.0f) continue;
        const ImU32 color = ImGui::GetColorU32(a.stutter || b.stutter ? kHotColor : kFpsColor);
        draw->AddLine(ImVec2(columnX(i - 1), valueY(a.slowest)), ImVec2(columnX(i), valueY(b.slowest)), color, thickness);
    }
    draw->PopClipRect();
}

} // namespace

ImVec2 CornerPivot(Corner c) {
    switch (c) {
    case Corner::TopLeft: return {0.0f, 0.0f};
    case Corner::TopRight: return {1.0f, 0.0f};
    case Corner::BottomLeft: return {0.0f, 1.0f};
    case Corner::BottomRight: return {1.0f, 1.0f};
    }
    return {1.0f, 0.0f};
}

ImVec2 DrawStats(const Config& cfg, const SensorSnapshot& snap, ImVec2 pos, ImVec2 pivot) {
    Row rows[5];
    int rowCount = 0;

    if (cfg.showFps || cfg.showFrameTime) {
        Row& r = rows[rowCount++];
        r.label = "FPS";
        r.labelColor = kFpsColor;
        if (cfg.showFps) {
            if (snap.fps) r.Add(Format("%.0f", *snap.fps), kValueColor);
            else r.Add("--", kMissingColor);
        }
        if (cfg.showFrameTime) {
            if (snap.frameTimeMs) r.Add(Format("%.1f ms", *snap.frameTimeMs), kValueColor);
            else r.Add("-- ms", kMissingColor);
        }
    }
    if (cfg.showCpuUsage || cfg.showCpuTemp || cfg.showCpuPower) {
        Row& r = rows[rowCount++];
        r.label = "CPU";
        r.labelColor = kCpuColor;
        if (cfg.showCpuUsage) AddUsage(r, snap.cpuUsage);
        if (cfg.showCpuTemp) AddTemp(r, snap.cpuTemp, cfg.fahrenheit);
        if (cfg.showCpuPower) AddPower(r, snap.cpuPowerW);
    }
    if (cfg.showGpuUsage || cfg.showGpuTemp || cfg.showGpuPower) {
        Row& r = rows[rowCount++];
        r.label = "GPU";
        r.labelColor = kGpuColor;
        if (cfg.showGpuUsage) AddUsage(r, snap.gpuUsage);
        if (cfg.showGpuTemp) AddTemp(r, snap.gpuTemp, cfg.fahrenheit);
        if (cfg.showGpuPower) AddPower(r, snap.gpuPowerW);
    }
    if (cfg.showVram) rows[rowCount++] = MemoryRow("VRAM", kGpuColor, snap.vramUsedGB, snap.vramTotalGB);
    if (cfg.showRam) rows[rowCount++] = MemoryRow("RAM", kRamColor, snap.ramUsedGB, snap.ramTotalGB);
    if (rowCount == 0 && !cfg.showFrameGraph) return ImVec2(0, 0);

    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 size(0, 0);

    ImGui::PushFont(nullptr, style.FontSizeBase * cfg.fontScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(style.WindowPadding.x * cfg.fontScale, style.WindowPadding.y * 0.8f * cfg.fontScale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * 2.0f * cfg.fontScale, 1.0f * cfg.fontScale));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(cfg.backgroundAlpha);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##stats", nullptr, flags)) {
        // Fixed column widths so the panel does not jitter as numbers change.
        float cellWidth = 0.0f;
        for (const char* sample : {"0000", "100%", "99.9 ms", "100\xC2\xB0""C", "212\xC2\xB0""F", "000 W", "N/A"})
            cellWidth = std::max(cellWidth, ImGui::CalcTextSize(sample).x);
        float labelWidth = 0.0f;
        int columns = 0;
        bool anySpan = false;
        for (int i = 0; i < rowCount; ++i) {
            labelWidth = std::max(labelWidth, ImGui::CalcTextSize(rows[i].label).x);
            if (rows[i].span) anySpan = true;
            else columns = std::max(columns, rows[i].count);
        }
        columns = std::max(columns, 1);

        // Memory rows span every value column; widen the last column if they would not fit.
        const float cellGap = ImGui::GetStyle().CellPadding.x * 2.0f;
        float lastWidth = cellWidth;
        if (anySpan) {
            const float valueArea = columns * cellWidth + (columns - 1) * cellGap;
            lastWidth += std::max(0.0f, ImGui::CalcTextSize(kMemorySample).x - valueArea);
        }
        auto columnWidth = [&](int c) { return c == columns - 1 ? lastWidth : cellWidth; };

        const ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX |
                                           ImGuiTableFlags_NoClip; // lets span rows overflow leftwards
        float contentWidth = ImGui::GetFontSize() * 9.0f; // graph width when there are no rows
        if (rowCount > 0 && ImGui::BeginTable("##t", 1 + columns, tableFlags)) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            for (int c = 0; c < columns; ++c) ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthFixed, columnWidth(c));

            // Right-aligns `text` to the right edge of value column `c` (may extend left of it).
            auto rightAligned = [&](int c, const Cell& cell) {
                ImGui::TableSetColumnIndex(1 + c);
                const float w = ImGui::CalcTextSize(cell.text.c_str()).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + columnWidth(c) - w);
                ImGui::TextColored(cell.color, "%s", cell.text.c_str());
            };

            for (int i = 0; i < rowCount; ++i) {
                const Row& r = rows[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(r.labelColor, "%s", r.label);
                if (r.span) {
                    rightAligned(columns - 1, r.cells[0]);
                } else {
                    for (int c = 0; c < r.count; ++c) rightAligned(c, r.cells[c]);
                }
            }
            // The table's natural width; its outer rect would stretch to the (auto-sized) window.
            contentWidth = ImGui::GetCurrentTable()->ColumnsAutoFitWidth;
            ImGui::EndTable();
        }
        if (cfg.showFrameGraph) DrawFrameGraph(snap, std::round(contentWidth));
        size = ImGui::GetWindowSize();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    return size;
}

UiAction DrawSettings(Config& cfg, const SensorSnapshot& snap, const std::vector<MonitorInfo>& monitors,
                      SettingsState& state) {
    UiAction action = UiAction::None;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Esc while editing a field (the hotkey) only cancels the edit. Checked before the widgets
    // handle Esc, which deactivates them.
    const bool editing = ImGui::IsAnyItemActive();

    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.96f);
    bool open = true;
    if (ImGui::Begin("FPS Monitor Settings", &open,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        const float fieldWidth = ImGui::GetFontSize() * 14.0f;

        ImGui::SeparatorText("Displayed values");
        if (ImGui::BeginTable("##metrics", 2)) {
            ImGui::TableNextColumn(); ImGui::Checkbox("FPS", &cfg.showFps);
            ImGui::TableNextColumn(); ImGui::Checkbox("Frame time", &cfg.showFrameTime);
            ImGui::TableNextColumn(); ImGui::Checkbox("CPU usage", &cfg.showCpuUsage);
            ImGui::TableNextColumn(); ImGui::Checkbox("CPU temperature", &cfg.showCpuTemp);
            ImGui::TableNextColumn(); ImGui::Checkbox("GPU usage", &cfg.showGpuUsage);
            ImGui::TableNextColumn(); ImGui::Checkbox("GPU temperature", &cfg.showGpuTemp);
            ImGui::TableNextColumn(); ImGui::Checkbox("CPU power", &cfg.showCpuPower);
            ImGui::TableNextColumn(); ImGui::Checkbox("GPU power", &cfg.showGpuPower);
            ImGui::TableNextColumn(); ImGui::Checkbox("RAM used / total", &cfg.showRam);
            ImGui::TableNextColumn(); ImGui::Checkbox("VRAM used / total", &cfg.showVram);
            ImGui::TableNextColumn(); ImGui::Checkbox("Frame time graph", &cfg.showFrameGraph);
            ImGui::SetItemTooltip("Frame times of the FPS target over the last %.0f seconds.\n"
                                  "Frames taking over twice the average of the normal frames in the second before them are\n"
                                  "drawn in red.", kFrameGraphSeconds);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!cfg.showFrameGraph);
            ImGui::Checkbox("Fast graph updates", &cfg.fastFrameGraph);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Redraws the graph about 30 times a second instead of every update interval,\n"
                                  "so it scrolls smoothly. Other values keep the update interval.\n"
                                  "Uses a little more CPU and GPU while the graph is visible.");
            ImGui::EndTable();
        }
        int unit = cfg.fahrenheit ? 1 : 0;
        ImGui::TextUnformatted("Temperature unit");
        ImGui::SameLine();
        ImGui::RadioButton("\xC2\xB0""C", &unit, 0);
        ImGui::SameLine();
        ImGui::RadioButton("\xC2\xB0""F", &unit, 1);
        cfg.fahrenheit = unit == 1;
        ImGui::Checkbox("Track game FPS when switching apps", &cfg.trackGame);
        ImGui::SetItemTooltip("Once a game has been in the foreground, its FPS stays on the overlay while you use\n"
                              "other apps, until the game exits or another game is focused.\n"
                              "A game is a full-screen app that renders frames, detected the same way Windows\n"
                              "decides to turn on \"Do not disturb\" for full-screen apps.");
        if (!cfg.trackGame) cfg.onlyWithGame = false; // it relies on game tracking
        ImGui::Indent();
        ImGui::BeginDisabled(!cfg.trackGame);
        ImGui::Checkbox("Only show overlay for games", &cfg.onlyWithGame);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", cfg.trackGame
                ? "The overlay stays hidden until a full-screen app that renders frames is detected,\n"
                  "and hides again when that game exits. It is always shown while Settings is open."
                : "Needs \"Track game FPS when switching apps\".");
        if (!cfg.onlyWithGame) cfg.onlyWhenFocused = false;
        ImGui::Indent();
        ImGui::BeginDisabled(!cfg.onlyWithGame);
        ImGui::Checkbox("Only show while game is focused", &cfg.onlyWhenFocused);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", cfg.onlyWithGame
                ? "Also hides the overlay while you use other apps, and shows it again as soon as\n"
                  "the detected game is back in the foreground."
                : "Needs \"Only show overlay for games\".");
        ImGui::Unindent();
        ImGui::Unindent();
        ImGui::Checkbox("Hide overlay from screenshots / recordings", &cfg.hideFromCapture);

        ImGui::SeparatorText("Position");
        const char* corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        int corner = (int)cfg.corner;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::Combo("Corner", &corner, corners, IM_ARRAYSIZE(corners))) cfg.corner = (Corner)corner;

        std::string current = cfg.monitor < (int)monitors.size() ? monitors[cfg.monitor].label
                                                                 : "Monitor " + std::to_string(cfg.monitor) + " (not connected)";
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::BeginCombo("Monitor", current.c_str())) {
            for (int i = 0; i < (int)monitors.size(); ++i) {
                if (ImGui::Selectable(monitors[i].label.c_str(), i == cfg.monitor)) cfg.monitor = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::DragInt("Offset X", &cfg.offsetX, 1.0f, 0, kMaxOffset, "%d px");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::DragInt("Offset Y", &cfg.offsetY, 1.0f, 0, kMaxOffset, "%d px");

        ImGui::SeparatorText("Appearance");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("Size", &cfg.fontScale, kMinFontScale, kMaxFontScale, "%.2fx");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::SliderFloat("Background opacity", &cfg.backgroundAlpha, 0.0f, 1.0f, "%.2f");

        ImGui::SeparatorText("Behavior");
        ImGui::SetNextItemWidth(fieldWidth);
        // Logarithmic, so the short intervals keep fine steps across the wide range.
        ImGui::SliderInt("Update interval", &cfg.updateIntervalMs, kMinUpdateIntervalMs, kMaxUpdateIntervalMs, "%d ms",
                         ImGuiSliderFlags_Logarithmic);

        UINT mods = 0, vk = 0;
        const bool hotkeyValid = ParseHotkey(state.hotkeyBuf, mods, vk);
        if (!hotkeyValid) ImGui::PushStyleColor(ImGuiCol_Text, kHotColor);
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("Settings hotkey", state.hotkeyBuf, sizeof(state.hotkeyBuf));
        if (!hotkeyValid) ImGui::PopStyleColor();
        if (hotkeyValid) cfg.hotkey = state.hotkeyBuf;
        ImGui::SetItemTooltip("e.g. Ctrl+Shift+F12, Alt+F10, Ctrl+Alt+O");

        ImGui::SeparatorText("Sensor status");
        ImGui::TextDisabled("FPS target:  %s (PID %lu)%s",
                            snap.foregroundProcess.empty() ? "?" : snap.foregroundProcess.c_str(), snap.foregroundPid,
                            snap.targetIsTrackedGame ? "  - tracked game" : "");
        ImGui::TextDisabled("FPS:  %s", snap.fpsStatus.c_str());
        ImGui::TextDisabled("CPU:  %s  [%s]", snap.cpuName.empty() ? "?" : snap.cpuName.c_str(), snap.cpuStatus.c_str());
        if (state.pawnioInstalling) {
            ImGui::SameLine();
            ImGui::TextColored(kWarmColor, "Installing PawnIO...");
        } else if (snap.cpuNeedsDriver && PawnIOSetupBundled()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Install PawnIO driver")) action = UiAction::InstallPawnIO;
            ImGui::SetItemTooltip("Installs the signed PawnIO driver (pawnio.eu), needed to read the CPU temperature and power.");
        }
        if (!state.pawnioMessage.empty()) ImGui::TextColored(kWarmColor, "%s", state.pawnioMessage.c_str());
        ImGui::TextDisabled("GPU:  %s  [%s]", snap.gpuName.empty() ? "?" : snap.gpuName.c_str(), snap.gpuStatus.c_str());

        ImGui::Separator();
        if (ImGui::Button("Save & close")) action = UiAction::CloseSettings;
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) {
            const bool offerPawnIOInstall = cfg.offerPawnIOInstall; // not a setting shown here
            cfg = Config{};
            cfg.offerPawnIOInstall = offerPawnIOInstall;
            std::snprintf(state.hotkeyBuf, sizeof(state.hotkeyBuf), "%s", cfg.hotkey.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit FPS Monitor")) action = UiAction::ExitApp;
    }
    ImGui::End();

    if (!open || (!editing && ImGui::IsKeyPressed(ImGuiKey_Escape))) action = UiAction::CloseSettings;
    cfg.Clamp();
    return action;
}
