#pragma once

#include <windows.h>

#include "sensors/gpu_source.h"

// Vendor-neutral fallback (any WDDM 2.4+ GPU): temperature from
// D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERPERFDATA), and usage and VRAM from the
// "GPU Engine" / "GPU Adapter Memory" performance counters, like Task Manager.
// No power reading.
class GpuD3dkmt : public GpuSource {
public:
    GpuD3dkmt();
    ~GpuD3dkmt() override;

    bool Available() const override { return adapter_ != 0; }
    void Update() override; // collects the performance counters
    std::optional<float> Usage() override;
    std::optional<float> Temperature() override;
    std::optional<GpuMemory> Memory() override;

private:
    std::optional<double> SumForAdapter(void* counter) const;

    UINT adapter_ = 0; // D3DKMT_HANDLE
    void* query_ = nullptr;         // PDH_HQUERY
    void* usageCounter_ = nullptr;  // PDH_HCOUNTER: GPU Engine 3D utilization
    void* memoryCounter_ = nullptr; // PDH_HCOUNTER: GPU Adapter Memory dedicated usage
    bool collected_ = false;
    double vramTotal_ = 0.0;
    std::wstring luidTag_;
};
