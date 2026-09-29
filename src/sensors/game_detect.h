#pragma once

#include <windows.h>

#include <string>

// None of these open a handle to the target process, so games and their
// anti-cheats never see us touching them.

// True when `hwnd` (the foreground window) is a full-screen app: its content area covers its
// whole monitor, it is not maximized, not the desktop or taskbar, and Windows reports a full-screen app
// (the shell signal behind the automatic "Do not disturb" rule "When using an app in
// full-screen mode"). Works for any game, installed or portable.
bool IsFullscreenApp(HWND hwnd);

// Exe file name of a process (UTF-8, empty if not found), from a Toolhelp process
// snapshot of the whole system.
std::string ProcessExeName(DWORD pid);

// A top-level window owned by `pid` (visible ones first), or nullptr if it has none.
HWND FindProcessWindow(DWORD pid);
