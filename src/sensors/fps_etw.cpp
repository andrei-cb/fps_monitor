#include "sensors/fps_etw.h"

#include "util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

const wchar_t kSessionName[] = L"FpsMonitorOverlaySession";

// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9} Microsoft-Windows-DXGI
const GUID kDxgiProvider = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
// {783ACA0A-790E-4D7F-8451-AA850511C6B9} Microsoft-Windows-D3D9
const GUID kD3d9Provider = {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};
// {802EC45A-1E99-4B83-9920-87C98277BA9D} Microsoft-Windows-DxgKrnl
const GUID kDxgKrnlProvider = {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};

constexpr USHORT kDxgiPresentStart = 42;
constexpr USHORT kDxgiPresentMpoStart = 55;
constexpr USHORT kD3d9PresentStart = 1;
constexpr USHORT kDxgKrnlPresentInfo = 184;

// Keywords as declared in the provider manifests (see PresentMon's PresentData/ETW headers).
constexpr ULONGLONG kDxgiD3d9Keywords = 0x8000000000000002ull;           // Events | Analytic channel
constexpr ULONGLONG kDxgKrnlKeywords = 0x4000000000000000ull | 0x8000001; // Performance channel | Present | Base

// Presents kept per process and API level, in case polling stalls (normally pruned on every poll).
constexpr size_t kMaxQueuedPresents = 20000;

struct TraceProps {
    EVENT_TRACE_PROPERTIES props;
    wchar_t name[64];
};

void InitProps(TraceProps& p) {
    ZeroMemory(&p, sizeof(p));
    p.props.Wnode.BufferSize = sizeof(TraceProps);
    p.props.LoggerNameOffset = offsetof(TraceProps, name);
}

ULONG EnableProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords, std::initializer_list<USHORT> ids) {
    // Filter by event id in the provider so we only receive present events.
    std::vector<BYTE> filterBuf(offsetof(EVENT_FILTER_EVENT_ID, Events) + ids.size() * sizeof(USHORT));
    auto* filter = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filterBuf.data());
    filter->FilterIn = TRUE;
    filter->Count = (USHORT)ids.size();
    std::copy(ids.begin(), ids.end(), filter->Events);

    EVENT_FILTER_DESCRIPTOR desc = {};
    desc.Ptr = (ULONGLONG)filterBuf.data();
    desc.Size = (ULONG)filterBuf.size();
    desc.Type = EVENT_FILTER_TYPE_EVENT_ID;

    ENABLE_TRACE_PARAMETERS params = {};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;

    ULONG st = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                              keywords, 0, 0, &params);
    if (st != ERROR_SUCCESS) {
        // Retry without the filter; OnEvent ignores unrelated event ids anyway.
        st = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                            keywords, 0, 0, nullptr);
    }
    return st;
}

} // namespace

FpsEtw::FpsEtw() : qpcFreq_(QpcFrequency()) {}

FpsEtw::~FpsEtw() { Stop(); }

bool FpsEtw::Start() {
    if (running_) return true;

    // A previous instance may have crashed and left the session running.
    TraceProps props;
    InitProps(props);
    ControlTraceW(0, kSessionName, &props.props, EVENT_TRACE_CONTROL_STOP);

    InitProps(props);
    props.props.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props.props.Wnode.ClientContext = 1; // QPC timestamps
    // A single buffer set (instead of one per CPU) keeps events in order, and small
    // buffers fill quickly, so events reach us within a fraction of a second.
    props.props.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    props.props.BufferSize = 4;          // KB
    props.props.MinimumBuffers = 32;
    props.props.MaximumBuffers = 512;
    props.props.FlushTimer = 1;          // seconds; we also flush explicitly on every poll

    ULONG st = StartTraceW(&session_, kSessionName, &props.props);
    if (st != ERROR_SUCCESS) {
        session_ = 0;
        status_ = st == ERROR_ACCESS_DENIED ? "ETW access denied (run as administrator)"
                                            : "StartTrace failed (" + std::to_string(st) + ")";
        return false;
    }

    ULONG e1 = EnableProvider(session_, kDxgiProvider, kDxgiD3d9Keywords, {kDxgiPresentStart, kDxgiPresentMpoStart});
    ULONG e2 = EnableProvider(session_, kD3d9Provider, kDxgiD3d9Keywords, {kD3d9PresentStart});
    ULONG e3 = EnableProvider(session_, kDxgKrnlProvider, kDxgKrnlKeywords, {kDxgKrnlPresentInfo});
    if (e1 != ERROR_SUCCESS && e2 != ERROR_SUCCESS && e3 != ERROR_SUCCESS) {
        status_ = "EnableTraceEx2 failed (DXGI " + std::to_string(e1) + ", D3D9 " + std::to_string(e2) + ", DxgKrnl " +
                  std::to_string(e3) + ")";
        Stop();
        return false;
    }

    EVENT_TRACE_LOGFILEW log = {};
    log.LoggerName = const_cast<LPWSTR>(kSessionName);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = &FpsEtw::OnEventStatic;
    log.Context = this;
    consumer_ = OpenTraceW(&log);
    if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
        status_ = "OpenTrace failed (" + std::to_string(GetLastError()) + ")";
        Stop();
        return false;
    }

    thread_ = std::thread([this] { ProcessTrace(&consumer_, 1, nullptr, nullptr); });
    running_ = true;
    status_ = "ETW session running";
    return true;
}

void FpsEtw::Stop() {
    if (session_) {
        TraceProps props;
        InitProps(props);
        ControlTraceW(session_, nullptr, &props.props, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    if (consumer_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(consumer_);
        consumer_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (thread_.joinable()) thread_.join();
    if (running_) status_ = "Stopped (FPS hidden)";
    running_ = false;

    std::lock_guard lock(mutex_);
    frames_.clear();
    newestEvent_ = 0;
}

void WINAPI FpsEtw::OnEventStatic(PEVENT_RECORD rec) {
    static_cast<FpsEtw*>(rec->UserContext)->OnEvent(rec);
}

void FpsEtw::OnEvent(PEVENT_RECORD rec) {
    const EVENT_HEADER& h = rec->EventHeader;
    const USHORT id = h.EventDescriptor.Id;

    bool api = false;
    if (IsEqualGUID(h.ProviderId, kDxgiProvider)) {
        if (id != kDxgiPresentStart && id != kDxgiPresentMpoStart) return;
        api = true;
    } else if (IsEqualGUID(h.ProviderId, kD3d9Provider)) {
        if (id != kD3d9PresentStart) return;
        api = true;
    } else if (IsEqualGUID(h.ProviderId, kDxgKrnlProvider)) {
        if (id != kDxgKrnlPresentInfo) return;
    } else {
        return;
    }

    const int64_t ts = h.TimeStamp.QuadPart;
    std::lock_guard lock(mutex_);
    newestEvent_ = std::max(newestEvent_, ts);
    ProcessFrames& pf = frames_[h.ProcessId];
    std::vector<int64_t>& q = api ? pf.api : pf.kernel;
    if (q.size() >= kMaxQueuedPresents) q.erase(q.begin(), q.begin() + kMaxQueuedPresents / 4); // drop the oldest
    q.push_back(ts);
}

void FpsEtw::Prune(int64_t cutoff) {
    for (auto it = frames_.begin(); it != frames_.end();) {
        auto& pf = it->second;
        std::erase_if(pf.api, [&](int64_t t) { return t < cutoff; });
        std::erase_if(pf.kernel, [&](int64_t t) { return t < cutoff; });
        it = (pf.api.empty() && pf.kernel.empty()) ? frames_.erase(it) : std::next(it);
    }
}

// Pushes buffered events to the consumer so the numbers stay fresh.
void FpsEtw::Flush() {
    TraceProps props;
    InitProps(props);
    ControlTraceW(session_, nullptr, &props.props, EVENT_TRACE_CONTROL_FLUSH);
}

// Measurements end at the newest *delivered* event rather than wall-clock time: ETW hands events
// over in batches, so the most recent fraction of a second may not have arrived yet. If nothing
// at all arrived for a while (idle system), fall back to wall-clock time.
int64_t FpsEtw::WindowEnd() {
    const int64_t now = Qpc();
    return newestEvent_ == 0 || now - newestEvent_ > 2 * qpcFreq_ ? now : newestEvent_;
}

std::optional<double> FpsEtw::GetFps(DWORD pid, bool flush) {
    if (!running_) return std::nullopt;
    if (flush) Flush();

    std::lock_guard lock(mutex_);
    const int64_t windowEnd = WindowEnd();
    const int64_t windowStart = windowEnd - qpcFreq_;

    Prune(windowEnd - int64_t((std::max(1.0, historySeconds_) + 1.0) * qpcFreq_));
    auto it = frames_.find(pid);
    if (it == frames_.end()) return std::nullopt;

    // Order-independent scan: count, first and last present inside the window.
    struct Window { int n = 0; int64_t first = INT64_MAX, last = INT64_MIN; };
    auto scan = [&](const std::vector<int64_t>& q) {
        Window w;
        for (int64_t t : q) {
            if (t < windowStart || t > windowEnd) continue;
            ++w.n;
            w.first = std::min(w.first, t);
            w.last = std::max(w.last, t);
        }
        return w;
    };
    const Window api = scan(it->second.api);
    const Window kernel = scan(it->second.kernel);

    // Prefer runtime-level presents (DXGI/D3D9); fall back to kernel presents for Vulkan/OpenGL.
    const Window& w = api.n >= 2 ? api : kernel;
    if (w.n < 2) return std::nullopt;

    const double span = double(w.last - w.first) / qpcFreq_;
    const double avgInterval = span / double(w.n - 1);
    const double idle = double(windowEnd - w.last) / qpcFreq_;
    if (span <= 0.0) return std::nullopt;
    // Interval-based FPS is precise while frames keep coming; if the process stopped presenting
    // (paused, alt-tabbed), use the plain count over the window so the value decays to zero.
    return idle > std::max(0.25, 4.0 * avgInterval) ? double(w.n) : double(w.n - 1) / span;
}

bool FpsEtw::GetFrameTimes(DWORD pid, double seconds, bool flush, std::vector<float>& frameMs, int64_t& lastFrameQpc) {
    frameMs.clear();
    lastFrameQpc = 0;
    if (!running_) return false;
    if (flush) Flush();

    std::lock_guard lock(mutex_);
    const int64_t windowEnd = WindowEnd();
    const int64_t windowStart = windowEnd - int64_t(seconds * qpcFreq_);
    auto it = frames_.find(pid);
    if (it == frames_.end()) return false;

    // Same preference as GetFps: runtime presents, else kernel presents.
    auto collect = [&](const std::vector<int64_t>& q) {
        std::vector<int64_t> times;
        for (int64_t t : q)
            if (t >= windowStart && t <= windowEnd) times.push_back(t);
        return times;
    };
    std::vector<int64_t> times = collect(it->second.api);
    if (times.size() < 2) times = collect(it->second.kernel);
    if (times.size() < 2) return false;
    std::sort(times.begin(), times.end()); // events can arrive slightly out of order

    const double msPerTick = 1000.0 / double(qpcFreq_);
    frameMs.reserve(times.size() - 1);
    for (size_t i = 1; i < times.size(); ++i) frameMs.push_back(float(double(times[i] - times[i - 1]) * msPerTick));
    lastFrameQpc = times.back();
    return true;
}
