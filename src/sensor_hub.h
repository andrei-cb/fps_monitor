#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

constexpr float kFrameGraphSeconds = 5.0f; // time span of the frame time graph
constexpr float kStutterWindowSeconds = 1.0f; // a stutter is a frame over twice the average of this long before it
constexpr int kFastGraphPeriodMs = 33;     // graph data refresh and redraw with fast updates on (~30 Hz)
constexpr WPARAM kSnapshotGraphOnly = 1;   // notify wParam: only the frame time graph data changed

struct SensorSnapshot {
    std::optional<float> fps;
    std::optional<float> frameTimeMs;
    std::optional<float> cpuUsage;
    std::optional<float> cpuTemp;
    std::optional<float> gpuUsage;
    std::optional<float> gpuTemp;
    std::optional<float> cpuPowerW;
    std::optional<float> gpuPowerW;
    std::optional<float> vramUsedGB;
    std::optional<float> vramTotalGB;
    std::optional<float> ramUsedGB;
    std::optional<float> ramTotalGB;
    std::vector<float> frameTimesMs; // FPS target's recent frame times (ms), oldest first
    int64_t lastFrameQpc = 0;        // QueryPerformanceCounter time at which the newest one ended

    // Diagnostics shown in the settings panel.
    std::string foregroundProcess; // process whose FPS is shown (foreground app or tracked game)
    DWORD foregroundPid = 0;
    bool targetIsTrackedGame = false;
    bool gameTracked = false; // a game is being tracked (TrackGame option)
    std::string fpsStatus;
    std::string cpuName;
    std::string cpuStatus;       // PawnIO state (CPU temperature and power)
    bool cpuNeedsDriver = false; // PawnIO missing or outdated
    std::string gpuName;
    std::string gpuStatus;
};

// Metrics currently shown on the overlay. Hidden metrics are not polled at all.
enum MetricBits : uint32_t {
    kMetricFps = 1u << 0, // FPS and/or frame time
    kMetricCpuUsage = 1u << 1,
    kMetricCpuTemp = 1u << 2,
    kMetricCpuPower = 1u << 3,
    kMetricGpuUsage = 1u << 4,
    kMetricGpuTemp = 1u << 5,
    kMetricGpuPower = 1u << 6,
    kMetricVram = 1u << 7,
    kMetricRam = 1u << 8,
    kMetricFrameGraph = 1u << 9,
    kMetricFastGraph = 1u << 10, // refresh the frame time graph every kFastGraphPeriodMs
};

// Polls the shown sensors on a background thread and posts `notifyMsg` to `notifyWnd`
// whenever a fresh snapshot is available. Most values refresh every update interval;
// power refreshes at most every 2 s and RAM/VRAM at most every 5 s. With fast graph updates,
// the frame time graph also refreshes in between (wParam = kSnapshotGraphOnly).
class SensorHub {
public:
    SensorHub() = default;
    ~SensorHub();

    void Start(HWND notifyWnd, UINT notifyMsg, int intervalMs);
    void Stop();
    // Takes effect right away: the next poll is due one new interval after the previous one.
    void SetInterval(int intervalMs);
    // Which metrics to poll (MetricBits). Newly enabled ones are read right away.
    void SetMetrics(uint32_t mask);
    // Keep reporting the last focused game's FPS while other apps are in the foreground.
    void SetTrackGame(bool enabled) { trackGame_ = enabled; }
    // Re-open the PawnIO driver on the next poll (after it was installed).
    void ReinitPawnIO() { reinitPawnIO_ = true; }

    SensorSnapshot Snapshot();

private:
    void Run();
    void Wake(); // makes the polling thread re-check its schedule

    HWND notifyWnd_ = nullptr;
    UINT notifyMsg_ = 0;
    std::atomic<int> intervalMs_{500};
    std::atomic<bool> stop_{false};
    std::atomic<bool> reinitPawnIO_{false};
    std::atomic<bool> trackGame_{false};
    std::atomic<uint32_t> metrics_{0};
    std::thread thread_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool wakeNow_ = false; // guarded by wakeMutex_: re-check the schedule without waiting

    std::mutex mutex_;
    SensorSnapshot snapshot_;
};
