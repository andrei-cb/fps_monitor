#include "sensors/gpu_d3dkmt.h"

#include <winternl.h> // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;

GpuD3dkmt::GpuD3dkmt() {
    // Pick the hardware adapter with the most dedicated VRAM.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        status_ = "CreateDXGIFactory1 failed";
        return;
    }
    DXGI_ADAPTER_DESC1 best = {};
    bool found = false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!found || d.DedicatedVideoMemory > best.DedicatedVideoMemory)) {
            best = d;
            found = true;
        }
    }
    if (!found) {
        status_ = "No hardware GPU adapter";
        return;
    }

    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, best.Description, -1, name, sizeof(name), nullptr, nullptr);
    name_ = name;
    const LUID luid = best.AdapterLuid;

    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) {
        status_ = "D3DKMTOpenAdapterFromLuid failed";
        return;
    }
    adapter_ = open.hAdapter;

    wchar_t tag[64];
    swprintf_s(tag, L"luid_0x%08X_0x%08X", (unsigned)luid.HighPart, (unsigned)luid.LowPart);
    luidTag_ = tag;

    vramTotal_ = (double)best.DedicatedVideoMemory;

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        PDH_HCOUNTER usage = nullptr, memory = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &usage) != ERROR_SUCCESS)
            usage = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory) != ERROR_SUCCESS)
            memory = nullptr;
        if (usage || memory) {
            PdhCollectQueryData(q); // first sample primes the rate counter
            query_ = q;
            usageCounter_ = usage;
            memoryCounter_ = memory;
        } else {
            PdhCloseQuery(q);
        }
    }
    status_ = "D3DKMT OK";
}

GpuD3dkmt::~GpuD3dkmt() {
    if (query_) PdhCloseQuery((PDH_HQUERY)query_);
    if (adapter_) {
        D3DKMT_CLOSEADAPTER close = {};
        close.hAdapter = adapter_;
        D3DKMTCloseAdapter(&close);
    }
}

std::optional<float> GpuD3dkmt::Temperature() {
    if (!adapter_) return std::nullopt;
    D3DKMT_ADAPTER_PERFDATA perf = {};
    perf.PhysicalAdapterIndex = 0;
    D3DKMT_QUERYADAPTERINFO q = {};
    q.hAdapter = adapter_;
    q.Type = KMTQAITYPE_ADAPTERPERFDATA;
    q.pPrivateDriverData = &perf;
    q.PrivateDriverDataSize = sizeof(perf);
    if (D3DKMTQueryAdapterInfo(&q) != 0 || perf.Temperature == 0) return std::nullopt;
    return perf.Temperature / 10.0f;
}

void GpuD3dkmt::Update() {
    collected_ = query_ && PdhCollectQueryData((PDH_HQUERY)query_) == ERROR_SUCCESS;
}

// Sums a wildcard counter over the instances that belong to our adapter.
std::optional<double> GpuD3dkmt::SumForAdapter(void* counter) const {
    if (!collected_ || !counter) return std::nullopt;
    DWORD bytes = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW((PDH_HCOUNTER)counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (st != PDH_MORE_DATA) return std::nullopt;
    std::vector<BYTE> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS)
        return std::nullopt;

    // Instances look like "pid_1234_luid_0x00000000_0x0000D1F3_phys_0_eng_0_engtype_3D"
    // (GPU Engine) or "luid_0x00000000_0x0000D1F3_phys_0" (GPU Adapter Memory).
    double sum = 0.0;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS && wcsstr(items[i].szName, luidTag_.c_str()))
            sum += items[i].FmtValue.doubleValue;
    }
    return sum;
}

std::optional<float> GpuD3dkmt::Usage() {
    auto sum = SumForAdapter(usageCounter_);
    if (!sum) return std::nullopt;
    return (float)std::clamp(*sum, 0.0, 100.0);
}

std::optional<GpuMemory> GpuD3dkmt::Memory() {
    auto used = SumForAdapter(memoryCounter_);
    if (!used || vramTotal_ <= 0.0) return std::nullopt;
    return GpuMemory{*used, vramTotal_};
}
