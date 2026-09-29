#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

// Reads CPU sensors through the signed PawnIO driver (https://pawnio.eu) using its
// official modules, which are embedded in the executable as resources:
//  - AMD Zen (family 17h-1Ah), AMDFamily17 module: Tctl/Tdie temperature (SMN thermal
//    register) and package power (package energy counter MSR).
//  - Intel (experimental), IntelMSR module: package temperature (thermal status MSR,
//    relative to TjMax) and package power (RAPL package energy counter MSR).
class CpuPawnIO {
public:
    CpuPawnIO();
    ~CpuPawnIO();

    // AMD Zen or Intel: the CPUs this reader supports.
    static bool CpuSupported();
    // Processor name from CPUID (works on any CPU, no driver needed).
    static std::string CpuName();

    std::optional<float> Temperature();
    // Average package power since the previous call (nullopt on the first call).
    std::optional<float> PackagePower();

    const std::string& Status() const { return status_; }
    // The CPU is supported but the PawnIO driver is missing or too old.
    bool NeedsDriver() const { return needsDriver_; }

private:
    enum class Vendor { Unsupported, AmdZen, Intel };

    void Init();
    bool Execute(const char* fn, uint64_t in, uint64_t& out);
    bool ReadSmn(uint32_t address, uint32_t& value);
    bool ReadMsr(uint32_t msr, uint64_t& value);
    std::optional<float> AmdTemperature();
    std::optional<float> IntelTemperature();

    Vendor vendor_ = Vendor::Unsupported;
    HANDLE device_ = INVALID_HANDLE_VALUE;
    HANDLE pciMutex_ = nullptr;
    float tdieOffset_ = 0.0f; // AMD: Tctl -> Tdie
    float tjMax_ = 100.0f;    // Intel: readings are "degrees below TjMax"
    bool intelPackageTemp_ = false;
    bool needsDriver_ = false;
    std::string status_;

    // Package power state.
    uint32_t energyMsr_ = 0;
    double joulesPerTick_ = 0.0;
    uint32_t lastEnergy_ = 0;
    int64_t lastEnergyQpc_ = 0;
};
