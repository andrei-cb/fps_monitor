#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Current QueryPerformanceCounter time, in ticks.
inline int64_t Qpc() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// QueryPerformanceCounter ticks per second.
inline int64_t QpcFrequency() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
}

// Looks up export `name` of `lib` into the function pointer `fn`. False if it is missing.
template <typename T>
bool Resolve(HMODULE lib, const char* name, T& fn) {
    fn = reinterpret_cast<T>(GetProcAddress(lib, name));
    return fn != nullptr;
}

// `s` without leading and trailing whitespace.
inline std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Folder of the executable, with a trailing backslash (empty if unknown).
inline std::wstring AppDirectory() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring path(exe, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}
