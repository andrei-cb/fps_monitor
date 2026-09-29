#pragma once

#include <windows.h>

#include "sensors/gpu_source.h"

// NVIDIA GPU sensors through nvml.dll (ships with the driver).
// The library is loaded dynamically, so no CUDA SDK is required.
class GpuNvml : public GpuSource {
public:
    GpuNvml();
    ~GpuNvml() override;

    bool Available() const override { return device_ != nullptr; }
    std::optional<float> Usage() override;
    std::optional<float> Temperature() override;
    std::optional<float> Power() override;
    std::optional<GpuMemory> Memory() override;

private:
    using nvmlReturn_t = int;
    using nvmlDevice_t = void*;
    struct nvmlUtilization_t { unsigned int gpu, memory; };
    struct nvmlMemory_t { unsigned long long total, free, used; };

    nvmlReturn_t (*init_)() = nullptr;
    nvmlReturn_t (*shutdown_)() = nullptr;
    nvmlReturn_t (*getCount_)(unsigned int*) = nullptr;
    nvmlReturn_t (*getHandle_)(unsigned int, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*getName_)(nvmlDevice_t, char*, unsigned int) = nullptr;
    nvmlReturn_t (*getUtil_)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    nvmlReturn_t (*getTemp_)(nvmlDevice_t, int, unsigned int*) = nullptr;
    // Optional exports: a missing one only disables that value.
    nvmlReturn_t (*getPower_)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*getMemory_)(nvmlDevice_t, nvmlMemory_t*) = nullptr;

    HMODULE lib_ = nullptr;
    bool initialized_ = false;
    nvmlDevice_t device_ = nullptr;
};
