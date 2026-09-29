#pragma once

#include <optional>

// Total CPU utilization via PDH (same counter Task Manager uses).
class CpuUsage {
public:
    CpuUsage();
    ~CpuUsage();

    std::optional<float> Sample();

private:
    void* query_ = nullptr;   // PDH_HQUERY
    void* counter_ = nullptr; // PDH_HCOUNTER
};
