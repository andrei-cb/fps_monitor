#include "pawnio_setup.h"

#include "resource.h"
#include "util.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>

namespace {

const wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";

bool FindSetupResource(const void*& data, DWORD& size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAWNIO_SETUP), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    data = mem ? LockResource(mem) : nullptr;
    size = res ? SizeofResource(nullptr, res) : 0;
    return data && size;
}

} // namespace

std::string InstalledPawnIOVersion() {
    wchar_t buf[64] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    char out[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

bool PawnIOReady() {
    std::string v = InstalledPawnIOVersion();
    return !v.empty() && std::atoi(v.c_str()) >= 2;
}

bool PawnIOSetupBundled() {
    const void* data;
    DWORD size;
    return FindSetupResource(data, size);
}

bool RunBundledPawnIOSetup(std::string& error) {
    const void* data;
    DWORD size;
    if (!FindSetupResource(data, size)) {
        error = "This build does not include the PawnIO installer";
        return false;
    }

    const std::wstring path = AppDirectory() + L"PawnIO_setup.exe";

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "Could not write the installer to the app folder";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    if (!ok || written != size) {
        DeleteFileW(path.c_str());
        error = "Could not write the installer to the app folder";
        return false;
    }

    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas"; // no prompt when we are already elevated
    sei.lpFile = path.c_str();
    sei.lpParameters = L"-install -silent";
    sei.nShow = SW_HIDE;
    bool success = false;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        success = code == 0 || PawnIOReady();
        if (!success) error = "PawnIO installer exited with code " + std::to_string(code);
    } else {
        error = GetLastError() == ERROR_CANCELLED ? "Installation was cancelled" : "Could not start the PawnIO installer";
    }

    DeleteFileW(path.c_str());
    return success;
}
