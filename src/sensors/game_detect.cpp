#include "sensors/game_detect.h"

#include <shellapi.h>
#include <tlhelp32.h>

bool IsFullscreenApp(HWND hwnd) {
    if (!hwnd) return false;

    // The shell's state is system-wide and lags behind focus changes: right after switching away
    // from a game it can still report a full-screen app. So check the window itself first.

    // The desktop and the taskbar belong to the shell (explorer), which is never a game.
    DWORD pid = 0, shellPid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    if (pid == 0 || pid == shellPid) return false;

    // Maximized windows are not full-screen, even with an auto-hidden taskbar.
    if (IsZoomed(hwnd)) return false;

    // Its content (client) area must cover the whole monitor, so no visible title bar or frame.
    // The style bits do not tell: Dota 2's borderless window keeps WS_CAPTION, for example.
    RECT client;
    POINT origin = {0, 0};
    MONITORINFO mi = {sizeof(mi)};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL), &mi))
        return false;
    const RECT& mon = mi.rcMonitor;
    if (origin.x > mon.left || origin.y > mon.top || origin.x + client.right < mon.right ||
        origin.y + client.bottom < mon.bottom)
        return false;

    QUERY_USER_NOTIFICATION_STATE state;
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    // BUSY: a full-screen (or borderless full-screen) app is in the foreground.
    // RUNNING_D3D_FULL_SCREEN: an exclusive full-screen Direct3D app.
    return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

std::string ProcessExeName(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    std::string name;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID != pid) continue;
        char buf[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, buf, sizeof(buf), nullptr, nullptr);
        name = buf;
        break;
    }
    CloseHandle(snap);
    return name;
}

HWND FindProcessWindow(DWORD pid) {
    struct Search {
        DWORD pid;
        HWND visible = nullptr;
        HWND any = nullptr;
    } search{pid};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != s.pid) return TRUE;
        if (IsWindowVisible(hwnd)) {
            s.visible = hwnd;
            return FALSE; // good enough, stop
        }
        if (!s.any) s.any = hwnd;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.visible ? search.visible : search.any;
}
