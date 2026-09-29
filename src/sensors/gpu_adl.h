#pragma once

#include <windows.h>

#include "sensors/gpu_source.h"

#include <adl_sdk.h>

// AMD GPU sensors (experimental) through the AMD Display Library, atiadlxx.dll,
// which ships with the AMD driver and is loaded dynamically. Usage, temperature
// and power come from the PMLog sensor table (Vega 20 / RDNA and newer); GPUs
// without it are left to the vendor-neutral fallback.
class GpuAdl : public GpuSource {
public:
    GpuAdl();
    ~GpuAdl() override;

    bool Available() const override { return adapter_ >= 0; }
    void Update() override; // queries the PMLog sensor table
    std::optional<float> Usage() override;
    std::optional<float> Temperature() override;
    std::optional<float> Power() override;
    std::optional<GpuMemory> Memory() override;

private:
    std::optional<float> Sensor(int index) const;

    int (*controlCreate_)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*) = nullptr;
    int (*controlDestroy_)(ADL_CONTEXT_HANDLE) = nullptr;
    int (*adapterCount_)(ADL_CONTEXT_HANDLE, int*) = nullptr;
    int (*adapterInfo_)(ADL_CONTEXT_HANDLE, LPAdapterInfo, int) = nullptr;
    int (*queryPmLog_)(ADL_CONTEXT_HANDLE, int, ADLPMLogDataOutput*) = nullptr;
    // Optional exports: a missing one only disables that value.
    int (*vramUsage_)(ADL_CONTEXT_HANDLE, int, int*) = nullptr;
    int (*memoryInfo_)(ADL_CONTEXT_HANDLE, int, ADLMemoryInfoX4*) = nullptr;

    HMODULE lib_ = nullptr;
    ADL_CONTEXT_HANDLE context_ = nullptr;
    int adapter_ = -1;
    double vramTotal_ = 0.0;
    ADLPMLogDataOutput log_ = {};
    bool logValid_ = false;
};
