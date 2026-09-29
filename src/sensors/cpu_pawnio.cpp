#include "sensors/cpu_pawnio.h"

#include "pawnio_setup.h"
#include "resource.h"
#include "util.h"

#include <intrin.h>

#include <cstring>

namespace {

constexpr DWORD kPawnIoDeviceType = 41394u << 16;
constexpr DWORD kIoctlLoadBinary = kPawnIoDeviceType | (0x821 << 2);
constexpr DWORD kIoctlExecute = kPawnIoDeviceType | (0x841 << 2);
constexpr size_t kFnNameLength = 32;

// AMD Zen
constexpr uint32_t kSmnThmTconCurTmp = 0x00059800;
constexpr uint32_t kTempRangeSelMask = 0x80000;
constexpr uint32_t kTempTjSelMask = 0x30000;
constexpr uint32_t kAmdMsrPwrUnit = 0xC0010299;       // bits 12:8 = energy status unit
constexpr uint32_t kAmdMsrPkgEnergyStat = 0xC001029B; // bits 31:0 = package energy counter (wraps)

// Intel
constexpr uint32_t kIntelMsrThermStatus = 0x19C;        // core: bits 22:16 = degrees below TjMax, bit 31 = valid
constexpr uint32_t kIntelMsrTemperatureTarget = 0x1A2;  // bits 23:16 = TjMax
constexpr uint32_t kIntelMsrPackageThermStatus = 0x1B1; // package: bits 22:16 = degrees below TjMax
constexpr uint32_t kIntelMsrRaplPowerUnit = 0x606;      // bits 12:8 = energy status unit
constexpr uint32_t kIntelMsrPkgEnergyStatus = 0x611;    // bits 31:0 = package energy counter (wraps)

std::string CpuVendor() {
    int regs[4];
    __cpuid(regs, 0);
    char vendor[13] = {};
    std::memcpy(vendor + 0, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    return vendor;
}

// Processor brand string from CPUID, e.g. "AMD Ryzen 9 5950X 16-Core Processor".
std::string CpuBrand() {
    int regs[4];
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] < 0x80000004) return {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, 16);
    }
    return Trim(brand); // some CPUs pad the brand string with spaces
}

bool IsAmdZen() {
    if (CpuVendor() != "AuthenticAMD") return false;
    int regs[4];
    __cpuid(regs, 1);
    unsigned baseFamily = (regs[0] >> 8) & 0xF;
    unsigned family = baseFamily == 0xF ? baseFamily + ((regs[0] >> 20) & 0xFF) : baseFamily;
    return family >= 0x17 && family <= 0x1A;
}

bool IsIntel() { return CpuVendor() == "GenuineIntel"; }

// CPUID leaf 6, EAX bit 6: package thermal status MSR is available.
bool IntelHasPackageThermal() {
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 6) return false;
    __cpuid(regs, 6);
    return (regs[0] & (1 << 6)) != 0;
}

// Tctl -> Tdie offsets for early Zen parts (linux k10temp table).
float TdieOffsetFor(const std::string& brand) {
    auto has = [&](const char* s) { return brand.find(s) != std::string::npos; };
    if (has("1600X") || has("1700X") || has("1800X")) return -20.0f;
    if (has("2700X")) return -10.0f;
    if (has("Threadripper 19") || has("Threadripper 29")) return -27.0f;
    return 0.0f;
}

} // namespace

CpuPawnIO::CpuPawnIO() { Init(); }

bool CpuPawnIO::CpuSupported() { return IsAmdZen() || IsIntel(); }

std::string CpuPawnIO::CpuName() { return CpuBrand(); }

CpuPawnIO::~CpuPawnIO() {
    if (device_ != INVALID_HANDLE_VALUE) CloseHandle(device_);
    if (pciMutex_) CloseHandle(pciMutex_);
}

void CpuPawnIO::Init() {
    vendor_ = IsAmdZen() ? Vendor::AmdZen : IsIntel() ? Vendor::Intel : Vendor::Unsupported;
    if (vendor_ == Vendor::Unsupported) {
        status_ = "Unsupported CPU (only AMD Zen and Intel are implemented)";
        return;
    }
    const bool intel = vendor_ == Vendor::Intel;
    const char* moduleName = intel ? "IntelMSR" : "AMDFamily17";

    device_ = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device_ == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        needsDriver_ = err != ERROR_ACCESS_DENIED;
        status_ = err == ERROR_ACCESS_DENIED ? "PawnIO access denied (run as administrator)" : "PawnIO driver not installed";
        return;
    }

    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(intel ? IDR_PAWNIO_INTELMSR : IDR_PAWNIO_AMD17), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* blob = mem ? LockResource(mem) : nullptr;
    DWORD blobSize = res ? SizeofResource(nullptr, res) : 0;
    DWORD ret = 0;
    if (!blob || !DeviceIoControl(device_, kIoctlLoadBinary, const_cast<void*>(blob), blobSize, nullptr, 0, &ret, nullptr)) {
        status_ = std::string("PawnIO failed to load ") + moduleName + " module (" + std::to_string(GetLastError()) + ")";
        needsDriver_ = !PawnIOReady(); // an outdated 1.x driver rejects current modules
        if (needsDriver_) status_ += " - PawnIO " + InstalledPawnIOVersion() + " is outdated";
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
        return;
    }

    uint64_t unit = 0;
    if (intel) {
        uint64_t target = 0;
        if (ReadMsr(kIntelMsrTemperatureTarget, target) && ((target >> 16) & 0xFF) != 0)
            tjMax_ = float((target >> 16) & 0xFF);
        intelPackageTemp_ = IntelHasPackageThermal();
        energyMsr_ = kIntelMsrPkgEnergyStatus;
        if (ReadMsr(kIntelMsrRaplPowerUnit, unit)) joulesPerTick_ = 1.0 / double(1ull << ((unit >> 8) & 0x1F));
        status_ = "PawnIO OK (Intel support is experimental)";
    } else {
        // Shared with HWiNFO / LibreHardwareMonitor etc. to serialize PCI config accesses.
        pciMutex_ = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
        if (!pciMutex_) pciMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
        tdieOffset_ = TdieOffsetFor(CpuBrand());
        energyMsr_ = kAmdMsrPkgEnergyStat;
        if (ReadMsr(kAmdMsrPwrUnit, unit)) joulesPerTick_ = 1.0 / double(1ull << ((unit >> 8) & 0x1F));
        status_ = "PawnIO OK";
    }
}

// Calls a module function taking one input and returning one output value.
bool CpuPawnIO::Execute(const char* fn, uint64_t in, uint64_t& out) {
    uint8_t buf[kFnNameLength + sizeof(uint64_t)] = {};
    strncpy_s(reinterpret_cast<char*>(buf), kFnNameLength, fn, _TRUNCATE);
    std::memcpy(buf + kFnNameLength, &in, sizeof(in));
    DWORD ret = 0;
    if (!DeviceIoControl(device_, kIoctlExecute, buf, sizeof(buf), &out, sizeof(out), &ret, nullptr)) return false;
    return ret >= sizeof(out);
}

bool CpuPawnIO::ReadSmn(uint32_t address, uint32_t& value) {
    bool locked = false;
    if (pciMutex_) {
        DWORD w = WaitForSingleObject(pciMutex_, 10);
        locked = (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
        if (!locked) return false; // someone else is busy on the PCI bus; try next poll
    }
    uint64_t out = 0;
    bool ok = Execute("ioctl_read_smn", address, out);
    if (locked) ReleaseMutex(pciMutex_);
    value = (uint32_t)out;
    return ok;
}

bool CpuPawnIO::ReadMsr(uint32_t msr, uint64_t& value) { return Execute("ioctl_read_msr", msr, value); }

std::optional<float> CpuPawnIO::Temperature() {
    if (device_ == INVALID_HANDLE_VALUE) return std::nullopt;
    const std::optional<float> t = vendor_ == Vendor::Intel ? IntelTemperature() : AmdTemperature();
    if (!t || *t < -20.0f || *t > 150.0f) return std::nullopt;
    return t;
}

std::optional<float> CpuPawnIO::AmdTemperature() {
    uint32_t raw = 0;
    if (!ReadSmn(kSmnThmTconCurTmp, raw)) return std::nullopt;
    float t = float(raw >> 21) * 0.125f;
    if ((raw & kTempRangeSelMask) || (raw & kTempTjSelMask) == kTempTjSelMask) t -= 49.0f;
    return t + tdieOffset_;
}

std::optional<float> CpuPawnIO::IntelTemperature() {
    uint64_t raw = 0;
    if (intelPackageTemp_) {
        if (!ReadMsr(kIntelMsrPackageThermStatus, raw)) return std::nullopt;
    } else {
        // No package sensor: fall back to the core this thread happens to run on.
        if (!ReadMsr(kIntelMsrThermStatus, raw) || !(raw & (1ull << 31))) return std::nullopt;
    }
    return tjMax_ - float((raw >> 16) & 0x7F);
}

std::optional<float> CpuPawnIO::PackagePower() {
    if (device_ == INVALID_HANDLE_VALUE || joulesPerTick_ <= 0.0) return std::nullopt;
    uint64_t raw = 0;
    if (!ReadMsr(energyMsr_, raw)) return std::nullopt;

    const uint32_t energy = (uint32_t)raw;
    const int64_t now = Qpc();
    const bool first = lastEnergyQpc_ == 0;
    const uint32_t delta = energy - lastEnergy_; // unsigned math handles the 32-bit wrap
    const int64_t ticks = now - lastEnergyQpc_;
    lastEnergy_ = energy;
    lastEnergyQpc_ = now;
    if (first || ticks <= 0) return std::nullopt;

    const double seconds = double(ticks) / double(QpcFrequency());
    const double watts = double(delta) * joulesPerTick_ / seconds;
    if (watts < 0.0 || watts > 1000.0) return std::nullopt;
    return (float)watts;
}
