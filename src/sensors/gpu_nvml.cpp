#include "sensors/gpu_nvml.h"

#include "util.h"

namespace {
constexpr int NVML_SUCCESS = 0;
constexpr int NVML_TEMPERATURE_GPU = 0;
constexpr unsigned NVML_DEVICE_NAME_BUFFER_SIZE = 96;
} // namespace

GpuNvml::GpuNvml() {
    lib_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_) {
        // Older drivers installed NVML here instead of System32.
        wchar_t path[MAX_PATH];
        if (ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, MAX_PATH))
            lib_ = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!lib_) {
        status_ = "nvml.dll not found";
        return;
    }

    if (!Resolve(lib_, "nvmlInit_v2", init_) || !Resolve(lib_, "nvmlShutdown", shutdown_) ||
        !Resolve(lib_, "nvmlDeviceGetCount_v2", getCount_) || !Resolve(lib_, "nvmlDeviceGetHandleByIndex_v2", getHandle_) ||
        !Resolve(lib_, "nvmlDeviceGetName", getName_) || !Resolve(lib_, "nvmlDeviceGetUtilizationRates", getUtil_) ||
        !Resolve(lib_, "nvmlDeviceGetTemperature", getTemp_)) {
        status_ = "nvml.dll is missing required exports";
        return;
    }
    Resolve(lib_, "nvmlDeviceGetPowerUsage", getPower_);
    Resolve(lib_, "nvmlDeviceGetMemoryInfo", getMemory_);

    if (init_() != NVML_SUCCESS) {
        status_ = "nvmlInit failed";
        return;
    }
    initialized_ = true;

    unsigned count = 0;
    if (getCount_(&count) != NVML_SUCCESS || count == 0 || getHandle_(0, &device_) != NVML_SUCCESS) {
        device_ = nullptr;
        status_ = "No NVIDIA GPU found";
        return;
    }

    char name[NVML_DEVICE_NAME_BUFFER_SIZE] = {};
    if (getName_(device_, name, NVML_DEVICE_NAME_BUFFER_SIZE) == NVML_SUCCESS) name_ = name;
    status_ = "NVML OK";
}

GpuNvml::~GpuNvml() {
    if (initialized_) shutdown_();
    if (lib_) FreeLibrary(lib_);
}

std::optional<float> GpuNvml::Usage() {
    if (!device_) return std::nullopt;
    nvmlUtilization_t u = {};
    if (getUtil_(device_, &u) != NVML_SUCCESS) return std::nullopt;
    return (float)u.gpu;
}

std::optional<float> GpuNvml::Temperature() {
    if (!device_) return std::nullopt;
    unsigned t = 0;
    if (getTemp_(device_, NVML_TEMPERATURE_GPU, &t) != NVML_SUCCESS) return std::nullopt;
    return (float)t;
}

std::optional<float> GpuNvml::Power() {
    if (!device_ || !getPower_) return std::nullopt;
    unsigned mw = 0;
    if (getPower_(device_, &mw) != NVML_SUCCESS) return std::nullopt;
    return mw / 1000.0f;
}

std::optional<GpuMemory> GpuNvml::Memory() {
    if (!device_ || !getMemory_) return std::nullopt;
    nvmlMemory_t m = {};
    if (getMemory_(device_, &m) != NVML_SUCCESS || m.total == 0) return std::nullopt;
    return GpuMemory{(double)m.used, (double)m.total};
}
