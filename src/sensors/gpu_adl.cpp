#include "sensors/gpu_adl.h"

#include "util.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr int kAmdVendorId = 0x1002;

void* __stdcall AdlAlloc(int size) { return std::malloc(size); }

} // namespace

GpuAdl::GpuAdl() {
    lib_ = LoadLibraryExW(L"atiadlxx.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_) {
        status_ = "atiadlxx.dll not found";
        return;
    }
    if (!Resolve(lib_, "ADL2_Main_Control_Create", controlCreate_) ||
        !Resolve(lib_, "ADL2_Main_Control_Destroy", controlDestroy_) ||
        !Resolve(lib_, "ADL2_Adapter_NumberOfAdapters_Get", adapterCount_) ||
        !Resolve(lib_, "ADL2_Adapter_AdapterInfo_Get", adapterInfo_) ||
        !Resolve(lib_, "ADL2_New_QueryPMLogData_Get", queryPmLog_)) {
        status_ = "atiadlxx.dll is missing required exports";
        return;
    }
    Resolve(lib_, "ADL2_Adapter_DedicatedVRAMUsage_Get", vramUsage_);
    Resolve(lib_, "ADL2_Adapter_MemoryInfoX4_Get", memoryInfo_);

    if (controlCreate_(AdlAlloc, 1, &context_) != ADL_OK) {
        context_ = nullptr;
        status_ = "ADL initialization failed";
        return;
    }

    int count = 0;
    if (adapterCount_(context_, &count) != ADL_OK || count <= 0) {
        status_ = "No AMD GPU found";
        return;
    }
    std::vector<AdapterInfo> infos(count);
    std::memset(infos.data(), 0, sizeof(AdapterInfo) * count);
    for (AdapterInfo& info : infos) info.iSize = sizeof(AdapterInfo);
    if (adapterInfo_(context_, infos.data(), int(sizeof(AdapterInfo) * count)) != ADL_OK) {
        status_ = "ADL adapter query failed";
        return;
    }

    // One GPU can show up as several adapter indices. Pick the AMD GPU with the most
    // VRAM, so a discrete card wins over the integrated graphics of a Ryzen APU.
    const AdapterInfo* best = nullptr;
    long long bestMemory = -1;
    for (const AdapterInfo& info : infos) {
        if (info.iVendorID != kAmdVendorId || info.strUDID[0] == '\0') continue;
        long long memory = 0;
        ADLMemoryInfoX4 mi = {};
        if (memoryInfo_ && memoryInfo_(context_, info.iAdapterIndex, &mi) == ADL_OK) memory = mi.iMemorySize;
        if (memory > bestMemory) {
            bestMemory = memory;
            best = &info;
        }
    }
    if (!best) {
        status_ = "No AMD GPU found";
        return;
    }

    // Only GPUs with the PMLog sensor table are handled here; older ones use the fallback.
    ADLPMLogDataOutput probe = {};
    if (queryPmLog_(context_, best->iAdapterIndex, &probe) != ADL_OK) {
        status_ = "ADL: GPU has no PMLog sensors";
        return;
    }

    adapter_ = best->iAdapterIndex;
    name_ = best->strAdapterName;
    vramTotal_ = bestMemory > 0 ? double(bestMemory) : 0.0;
    status_ = "ADL OK (AMD support is experimental)";
}

GpuAdl::~GpuAdl() {
    if (context_) controlDestroy_(context_);
    if (lib_) FreeLibrary(lib_);
}

void GpuAdl::Update() {
    if (adapter_ < 0) return;
    std::memset(&log_, 0, sizeof(log_));
    logValid_ = queryPmLog_(context_, adapter_, &log_) == ADL_OK;
}

std::optional<float> GpuAdl::Sensor(int index) const {
    if (!logValid_ || index < 0 || index >= ADL_PMLOG_MAX_SENSORS || !log_.sensors[index].supported) return std::nullopt;
    return (float)log_.sensors[index].value;
}

std::optional<float> GpuAdl::Usage() { return Sensor(ADL_PMLOG_INFO_ACTIVITY_GFX); }

std::optional<float> GpuAdl::Temperature() {
    if (auto t = Sensor(ADL_PMLOG_TEMPERATURE_EDGE)) return t;
    return Sensor(ADL_PMLOG_TEMPERATURE_GFX);
}

std::optional<float> GpuAdl::Power() {
    // Total board power where the GPU reports it (RDNA 3+), else the ASIC power.
    if (auto p = Sensor(ADL_PMLOG_BOARD_POWER)) return p;
    return Sensor(ADL_PMLOG_ASIC_POWER);
}

std::optional<GpuMemory> GpuAdl::Memory() {
    if (adapter_ < 0 || !vramUsage_ || vramTotal_ <= 0.0) return std::nullopt;
    int usedMB = 0;
    if (vramUsage_(context_, adapter_, &usedMB) != ADL_OK || usedMB < 0) return std::nullopt;
    return GpuMemory{double(usedMB) * 1024.0 * 1024.0, vramTotal_};
}
