#include "sensors/cpu_usage.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>

CpuUsage::CpuUsage() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;

    PDH_HCOUNTER c = nullptr;
    // "% Processor Utility" accounts for boost clocks like Task Manager; older systems only have "% Processor Time".
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Utility", 0, &c) != ERROR_SUCCESS &&
        PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time", 0, &c) != ERROR_SUCCESS) {
        PdhCloseQuery(q);
        return;
    }
    PdhCollectQueryData(q); // first sample primes the rate counter
    query_ = q;
    counter_ = c;
}

CpuUsage::~CpuUsage() {
    if (query_) PdhCloseQuery((PDH_HQUERY)query_);
}

std::optional<float> CpuUsage::Sample() {
    if (!query_) return std::nullopt;
    if (PdhCollectQueryData((PDH_HQUERY)query_) != ERROR_SUCCESS) return std::nullopt;
    PDH_FMT_COUNTERVALUE v = {};
    if (PdhGetFormattedCounterValue((PDH_HCOUNTER)counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS)
        return std::nullopt;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return std::nullopt;
    return std::clamp((float)v.doubleValue, 0.0f, 100.0f);
}
