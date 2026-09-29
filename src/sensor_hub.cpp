#include "sensor_hub.h"

#include "sensors/cpu_pawnio.h"
#include "sensors/cpu_usage.h"
#include "sensors/fps_etw.h"
#include "sensors/game_detect.h"
#include "sensors/gpu_adl.h"
#include "sensors/gpu_d3dkmt.h"
#include "sensors/gpu_nvml.h"

#include <algorithm>
#include <chrono>
#include <memory>

namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
// The graph is drawn slightly behind real time (see DrawFrameGraph), and its oldest frames are
// compared with the normal frames before them (which were judged the same way), so it needs more
// history than it shows.
constexpr double kGraphHistorySeconds = kFrameGraphSeconds + 2.0 * kStutterWindowSeconds + 0.5;

// The first GPU source that works: NVML (NVIDIA), then ADL (AMD), then the vendor-neutral
// fallback. `status` describes the chosen one, plus why the vendor libraries were skipped.
std::unique_ptr<GpuSource> OpenGpu(std::string& status) {
    auto nvml = std::make_unique<GpuNvml>();
    if (nvml->Available()) {
        status = nvml->Status();
        return nvml;
    }
    auto adl = std::make_unique<GpuAdl>();
    if (adl->Available()) {
        status = adl->Status();
        return adl;
    }
    auto fallback = std::make_unique<GpuD3dkmt>();
    status = fallback->Status() + " (NVML: " + nvml->Status() + "; ADL: " + adl->Status() + ")";
    return fallback;
}

DWORD WindowProcessId(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

} // namespace

SensorHub::~SensorHub() { Stop(); }

void SensorHub::Start(HWND notifyWnd, UINT notifyMsg, int intervalMs) {
    notifyWnd_ = notifyWnd;
    notifyMsg_ = notifyMsg;
    intervalMs_ = intervalMs;
    stop_ = false;
    thread_ = std::thread(&SensorHub::Run, this);
}

void SensorHub::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(wakeMutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

SensorSnapshot SensorHub::Snapshot() {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void SensorHub::SetInterval(int intervalMs) {
    intervalMs_ = intervalMs;
    Wake();
}

void SensorHub::SetMetrics(uint32_t mask) {
    metrics_ = mask;
    Wake(); // a changed mask triggers a poll, so newly enabled metrics are read right away
}

void SensorHub::Wake() {
    {
        std::lock_guard lock(wakeMutex_);
        wakeNow_ = true;
    }
    wake_.notify_all();
}

void SensorHub::Run() {
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    constexpr auto kSlack = milliseconds(50); // tolerate waking up slightly early
    constexpr int kPowerPeriodMs = 2000;
    constexpr int kMemoryPeriodMs = 5000;

    // All sensors live on this thread so slow calls never stall rendering.
    FpsEtw fps; // started below while FPS / frame time is shown
    CpuUsage cpuUsage;
    auto cpu = std::make_unique<CpuPawnIO>(); // CPU temperature + package power
    const std::string cpuName = CpuPawnIO::CpuName();
    std::string gpuStatus;
    const std::unique_ptr<GpuSource> gpu = OpenGpu(gpuStatus);

    const DWORD selfPid = GetCurrentProcessId();
    DWORD lastFgPid = 0;

    // The game whose FPS we keep showing while other apps are focused (TrackGame option).
    // It is followed through its window rather than a process handle, so the game (and its
    // anti-cheat) never sees us opening it: when the game exits, its windows are destroyed.
    DWORD trackedPid = 0;
    HWND trackedWnd = nullptr;
    std::string trackedExe; // guards against the PID being reused by another program
    auto untrack = [&] {
        trackedPid = 0;
        trackedWnd = nullptr;
        trackedExe.clear();
    };

    // Exe name of the FPS target, shown in the settings panel; refreshed when the target changes.
    DWORD namePid = 0;
    std::string name;

    // Slow metrics are read on their own schedule; their last values carry over between polls.
    Clock::time_point nextCpuPower{}, nextGpuPower{}, nextVram{}, nextRam{};
    SensorSnapshot slow;
    uint32_t prevMask = 0;
    Clock::time_point lastPoll{};
    DWORD graphPid = 0; // whose frame times the graph shows, for updates between polls

    // Sleeps until `when`, or until woken early (new metrics or interval, shutdown).
    auto waitUntil = [this](Clock::time_point when) {
        std::unique_lock lock(wakeMutex_);
        wake_.wait_until(lock, when, [this] { return stop_.load() || wakeNow_; });
        wakeNow_ = false;
    };

    while (!stop_) {
        const uint32_t mask = metrics_;
        const auto now = Clock::now();
        const auto interval = milliseconds(intervalMs_.load());
        const bool graph = (mask & kMetricFrameGraph) != 0;
        const bool fastGraph = graph && (mask & kMetricFastGraph);
        // Computed from the current interval, so a changed interval applies to the pending poll.
        const auto nextPoll = lastPoll + interval;

        if (mask == prevMask && now < nextPoll) {
            if (fastGraph) {
                // Between regular polls (fast graph updates): refresh only the frame time graph.
                std::vector<float> frameMs;
                int64_t lastFrameQpc = 0;
                if (graphPid) fps.GetFrameTimes(graphPid, kGraphHistorySeconds, true, frameMs, lastFrameQpc);
                {
                    std::lock_guard lock(mutex_);
                    snapshot_.frameTimesMs = std::move(frameMs);
                    snapshot_.lastFrameQpc = lastFrameQpc;
                }
                PostMessageW(notifyWnd_, notifyMsg_, kSnapshotGraphOnly, 0);
                waitUntil(std::min(nextPoll, now + milliseconds(kFastGraphPeriodMs)));
            } else {
                waitUntil(nextPoll); // woken early by an interval change
            }
            continue;
        }

        lastPoll = now;
        SensorSnapshot s;
        const uint32_t turnedOn = mask & ~prevMask;
        prevMask = mask;

        // True when metric `bit` is shown and its next read is due (immediately once enabled).
        auto due = [&](Clock::time_point& next, uint32_t bit, int minPeriodMs) {
            if (!(mask & bit)) return false;
            if (turnedOn & bit) next = {};
            if (now + kSlack < next) return false;
            next = now + std::max(interval, milliseconds(minPeriodMs));
            return true;
        };

        // FPS: the ETW session only runs while FPS or frame time is shown.
        const bool wantFps = (mask & kMetricFps) != 0;
        if (wantFps && (turnedOn & kMetricFps) && !fps.IsRunning()) fps.Start();
        if (!wantFps && fps.IsRunning()) fps.Stop();
        fps.SetHistorySeconds(graph ? kGraphHistorySeconds : 0.0);

        const HWND fgWnd = GetForegroundWindow();
        DWORD fgPid = fgWnd ? WindowProcessId(fgWnd) : 0;
        const bool fgIsSelf = fgPid == selfPid;
        if (fgIsSelf) fgPid = lastFgPid; // keep showing the game while the settings panel has focus
        lastFgPid = fgPid;

        const std::optional<double> fgFps = wantFps && fgPid ? fps.GetFps(fgPid) : std::nullopt;

        if (!trackGame_ || !wantFps) {
            untrack();
        } else {
            if (trackedPid && (!IsWindow(trackedWnd) || WindowProcessId(trackedWnd) != trackedPid)) {
                // The tracked window is gone. The game may just have recreated it (e.g. a display
                // mode change): keep tracking if the same program still owns a window.
                const HWND other = FindProcessWindow(trackedPid);
                if (other && ProcessExeName(trackedPid) == trackedExe) trackedWnd = other;
                else untrack(); // game exited
            }
            // A foreground app that renders frames and is full-screen (what triggers automatic
            // "Do not disturb") becomes the tracked game.
            if (fgPid && fgPid != trackedPid && !fgIsSelf && fgFps && IsFullscreenApp(fgWnd)) {
                trackedPid = fgPid;
                trackedWnd = fgWnd;
                trackedExe = ProcessExeName(fgPid);
            }
        }

        s.gameTracked = trackedPid != 0;

        const DWORD target = trackedPid ? trackedPid : fgPid;
        if (wantFps && target) {
            // Flush only if the foreground query above did not already (no foreground app).
            const std::optional<double> v = target == fgPid ? fgFps : fps.GetFps(target, fgPid == 0);
            if (v && *v > 0.0) {
                s.fps = (float)*v;
                s.frameTimeMs = (float)(1000.0 / *v);
            }
            if (target != namePid) {
                name = target == trackedPid ? trackedExe : ProcessExeName(target);
                namePid = target;
            }
            s.foregroundPid = target;
            s.foregroundProcess = name;
            s.targetIsTrackedGame = target == trackedPid;
            if (graph) fps.GetFrameTimes(target, kGraphHistorySeconds, false, s.frameTimesMs, s.lastFrameQpc);
        }
        graphPid = graph && wantFps ? target : 0;
        s.fpsStatus = fps.Status();

        // CPU
        if (mask & kMetricCpuUsage) s.cpuUsage = cpuUsage.Sample();
        if (reinitPawnIO_.exchange(false)) {
            cpu.reset(); // release the old device handle first
            cpu = std::make_unique<CpuPawnIO>();
        }
        if (mask & kMetricCpuTemp) s.cpuTemp = cpu->Temperature();
        if (due(nextCpuPower, kMetricCpuPower, kPowerPeriodMs)) {
            if (turnedOn & kMetricCpuPower) {
                // Power is energy used between two reads: take a fresh baseline now and
                // report the first value after one update interval instead of 2 s.
                cpu->PackagePower();
                slow.cpuPowerW.reset();
                nextCpuPower = now + interval;
            } else {
                slow.cpuPowerW = cpu->PackagePower();
            }
        }
        s.cpuName = cpuName;
        s.cpuStatus = cpu->Status();
        s.cpuNeedsDriver = cpu->NeedsDriver();

        // GPU
        const bool gpuPowerDue = due(nextGpuPower, kMetricGpuPower, kPowerPeriodMs);
        const bool vramDue = due(nextVram, kMetricVram, kMemoryPeriodMs);
        auto setVram = [&](const std::optional<GpuMemory>& m) {
            slow.vramUsedGB = m ? std::optional<float>(float(m->used / kGiB)) : std::nullopt;
            slow.vramTotalGB = m ? std::optional<float>(float(m->total / kGiB)) : std::nullopt;
        };
        if ((mask & (kMetricGpuUsage | kMetricGpuTemp)) || gpuPowerDue || vramDue) gpu->Update();
        if (mask & kMetricGpuUsage) s.gpuUsage = gpu->Usage();
        if (mask & kMetricGpuTemp) s.gpuTemp = gpu->Temperature();
        if (gpuPowerDue) slow.gpuPowerW = gpu->Power();
        if (vramDue) setVram(gpu->Memory());
        s.gpuName = gpu->Name();
        s.gpuStatus = gpuStatus;

        // RAM
        if (due(nextRam, kMetricRam, kMemoryPeriodMs)) {
            MEMORYSTATUSEX ms = {sizeof(ms)};
            if (GlobalMemoryStatusEx(&ms)) {
                slow.ramUsedGB = float((ms.ullTotalPhys - ms.ullAvailPhys) / kGiB);
                slow.ramTotalGB = float(ms.ullTotalPhys / kGiB);
            } else {
                slow.ramUsedGB.reset();
                slow.ramTotalGB.reset();
            }
        }

        s.cpuPowerW = slow.cpuPowerW;
        s.gpuPowerW = slow.gpuPowerW;
        s.vramUsedGB = slow.vramUsedGB;
        s.vramTotalGB = slow.vramTotalGB;
        s.ramUsedGB = slow.ramUsedGB;
        s.ramTotalGB = slow.ramTotalGB;

        {
            std::lock_guard lock(mutex_);
            snapshot_ = std::move(s);
        }
        PostMessageW(notifyWnd_, notifyMsg_, 0, 0);

        waitUntil(now + (fastGraph ? std::min(interval, milliseconds(kFastGraphPeriodMs)) : interval));
    }
}
