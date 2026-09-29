#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Counts presents per process via a real-time ETW session (DXGI / D3D9 / DxgKrnl
// providers), the same technique PresentMon uses. No code is injected anywhere.
class FpsEtw {
public:
    FpsEtw();
    ~FpsEtw();

    bool Start();
    void Stop();

    bool IsRunning() const { return running_; }
    const std::string& Status() const { return status_; }

    // FPS of `pid` over the last second of delivered events, or nullopt if it did not present.
    // `flush` pushes buffered events first; pass false for extra queries in the same poll.
    std::optional<double> GetFps(DWORD pid, bool flush = true);

    // Frame times of `pid` (ms, oldest first) over the last `seconds` of delivered events, and the
    // QPC time of its newest frame. Returns false if it did not present in that time.
    bool GetFrameTimes(DWORD pid, double seconds, bool flush, std::vector<float>& frameMs, int64_t& lastFrameQpc);

    // How much present history to keep beyond the one second FPS needs (frame time graph).
    void SetHistorySeconds(double seconds) { historySeconds_ = seconds; }

private:
    struct ProcessFrames {
        std::vector<int64_t> api;    // DXGI / D3D9 present calls
        std::vector<int64_t> kernel; // DxgKrnl presents (covers Vulkan / OpenGL)
    };

    static void WINAPI OnEventStatic(PEVENT_RECORD rec);
    void OnEvent(PEVENT_RECORD rec);
    void Flush();
    int64_t WindowEnd(); // requires mutex_
    void Prune(int64_t cutoff);

    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread_;
    bool running_ = false;
    std::string status_ = "Not started";

    std::mutex mutex_;
    std::unordered_map<DWORD, ProcessFrames> frames_;
    int64_t newestEvent_ = 0; // newest timestamp delivered so far (any process)
    int64_t qpcFreq_ = 1;
    double historySeconds_ = 0.0;
};
