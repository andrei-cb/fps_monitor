# FPS Monitor

A topmost, click-through Dear ImGui overlay that shows FPS, frame time, CPU and GPU usage, temperature and power, and RAM / VRAM use. Nothing is injected into games, and the game process is never opened.

## Metrics

| Metric | Shown by default | Refresh | Source |
|---|---|---|---|
| FPS / frame time | FPS on, frame time off | Update interval | ETW present events (DXGI, D3D9, DxgKrnl) for the foreground app or the tracked game (see Usage) |
| CPU usage | on | Update interval | PDH `% Processor Utility` |
| CPU temp | on | Update interval | [PawnIO](https://pawnio.eu) driver + signed module: `AMDFamily17` (AMD Zen) or `IntelMSR` (Intel, experimental) |
| CPU power | off | 2 s | PawnIO: package energy counter MSR, averaged between reads |
| GPU usage / temp | on | Update interval | NVML (NVIDIA), ADL (AMD, experimental), else D3DKMT + PDH `GPU Engine` counters |
| GPU power | off | 2 s | NVML or ADL. N/A on the fallback |
| VRAM used / total | off | 5 s | NVML or ADL, else PDH `GPU Adapter Memory` counter |
| RAM used / total | off | 5 s | `GlobalMemoryStatusEx` |
| Frame time graph | off | Update interval, or ~30 Hz with **Fast graph updates** | The same ETW present events, last 5 s |

- The update interval is 500 ms by default (100–5000 ms) and set in Settings or `config.ini`.
- Power and memory refresh at the listed period, or at the update interval if that's longer.
- The frame time graph shows the time between the FPS target's frames over the last 5 seconds, newest on the right. Frames taking over twice the average of the normal (non-red) frames in the second before them are drawn in red. Leaving stutters out of the average keeps one long hitch from raising the limit afterwards; after a lasting frame rate drop, the limit adapts to the new rate within a second. Each frame is judged only against earlier frames, so it keeps its color while it scrolls across. The scale is fixed from 0 ms (a little above the bottom edge) to 60 ms at the top, so the line never jumps; slower frames are drawn at the top edge. **Fast graph updates** redraws only the graph about 30 times a second, so it scrolls smoothly; the other values keep the update interval.
  - The graph scrolls with the clock, about 120 ms behind real time, so frames that ETW delivers in bursts are already in place when drawn.
  - It costs about 2% of one CPU core in this app (about 0.6 ms per redraw, mostly in the graphics driver), and it pauses while the overlay is hidden.
- Hidden metrics aren't polled at all. When FPS and frame time are both hidden, the ETW session is stopped, unless the overlay is set to show only while a game is detected.

## Hardware support
| Hardware | Status | How |
|---|---|---|
| AMD Zen CPUs (Ryzen / Threadripper / EPYC, family 17h–1Ah) | Primary target | PawnIO `AMDFamily17`: Tctl/Tdie from the SMN thermal register, power from the package energy MSR |
| NVIDIA GPUs | Primary target | NVML (`nvml.dll`, ships with the driver) |
| **Intel CPUs** | **Experimental, untested** | PawnIO `IntelMSR`: package temperature as TjMax minus `IA32_PACKAGE_THERM_STATUS`, falling back to the core sensor. Power from the RAPL package energy MSR |
| **AMD GPUs** | **Experimental, untested** | ADL (`atiadlxx.dll`, ships with the driver): usage, edge temperature, board/ASIC power from the PMLog sensor table (Vega 20 / RDNA and newer), VRAM from ADL. Older AMD GPUs use the fallback |
| Any other GPU | Fallback | D3DKMT temperature + PDH usage/VRAM counters, like Task Manager. No power |

The experimental paths follow what LibreHardwareMonitor does, but nobody has run them on real hardware yet. Expect N/A values or wrong readings. The CPU and GPU lines under **Sensor status** in Settings show which source is in use; include them in bug reports.

## Build
Needs Visual Studio 2026 with the **Desktop development with C++** workload, which includes CMake. VS 2022 also works through the Ninja presets, but not the `vs2026` preset. The first configure downloads Dear ImGui, the PawnIO modules, the PawnIO installer and the AMD ADL SDK headers (MIT), all pinned and hash-checked, so it needs internet access.

- **Visual Studio:** **File > Open > Folder**, pick the `x64 Release (Ninja)` preset, and build `fps_monitor.exe`.
- **Terminal:** from a *Developer PowerShell for VS*, where `cmake` is on the PATH:
  ```
  cmake --preset vs2026
  cmake --build --preset release
  ```
  The exe ends up in `build\vs2026\Release\fps_monitor.exe`. The `configure` step is only needed once; afterwards `cmake --build --preset release` is enough, and it reconfigures by itself when `CMakeLists.txt` changes.

Build options, passed to the configure step as `-D<option>=OFF`:

| Option | Default | Effect |
|---|---|---|
| `FPS_MONITOR_REQUIRE_ADMIN` | ON | Embeds the admin manifest. Without it, FPS, CPU temp and CPU power show N/A. |
| `FPS_MONITOR_BUNDLE_PAWNIO` | ON | Embeds the PawnIO installer, adding about 3.4 MB to the exe. |

The app icon's source is `resources/icon.svg`. After editing it, run `python tools/make_icon.py` to regenerate `resources/app.ico`. Sizes of 40 px and up are rendered from the SVG. The 16–32 px sizes are drawn by the script as a flat, pixel-snapped version of the same design, from constants at the top of the script, so keep those in sync with the SVG. The script needs Pillow and Chrome or Edge; normal builds use the committed `.ico` and need neither.

Run `fps_monitor.exe` and accept the admin prompt; ETW and PawnIO both need admin rights. Only one instance runs at a time; starting it again opens the settings panel of the running one.

## PawnIO (CPU temperature and power)
The official signed `PawnIO_setup.exe` (v2.2.0, SHA-256 checked at configure time) is embedded in the exe.

If PawnIO is missing or outdated and CPU temperature or power is shown, the first launch asks whether to install it. It then extracts `PawnIO_setup.exe` next to `fps_monitor.exe`, runs `PawnIO_setup.exe -install -silent`, and deletes it afterwards. If you choose No, it doesn't ask again (`[Sensors] OfferPawnIOInstall=0`). The settings panel then shows an **Install PawnIO driver** button next to the CPU line under Sensor status.

When built with `FPS_MONITOR_BUNDLE_PAWNIO=OFF`, users install PawnIO themselves:
```
winget install namazso.PawnIO
```

PawnIO is GPL-2.0 (source: https://github.com/namazso/PawnIO), and its modules are LGPL-2.1 (https://github.com/namazso/PawnIO.Modules). They're shipped unmodified. If you distribute this exe, include these source links.

## Usage
- **Ctrl+Shift+F12**, or clicking the tray icon, opens the settings panel. Only the panel takes clicks; the rest of the screen stays usable. Press **Esc** or **Save & close** to write `config.ini`.
- Right-click the tray icon to exit.
- The Settings panel includes:
  - **Displayed values:** which metrics to show, the temperature unit, game tracking, showing the overlay only in games, and whether to hide the overlay from screenshots and recordings.
  - **Position:** corner, monitor and offset.
  - **Appearance:** size and background opacity.
  - **Behavior:** update interval and hotkey.
  - **Sensor status:** the FPS target, and the CPU and GPU names and sensor states.
- FPS is shown for the foreground app. With **Track game FPS when switching apps** turned on (`TrackGame=1`), the last focused game stays the target until it exits or another game is focused.
  - A game is a full-screen app that renders frames. It's detected with `SHQueryUserNotificationState`, the same shell signal Windows uses to turn on Do Not Disturb for full-screen apps, so portable games work too.
  - That signal is system-wide (it stays on while a full-screen game is open in the background), so the focused window must also have its content area cover the whole monitor, not be maximized, and not be the desktop or taskbar. Otherwise switching from a game to Task Manager or the desktop could make them the "game".
  - The tracked game is followed through its window, not a process handle.
- **Only show overlay for games** (`ShowOnlyWithGame=1`) needs the option above. The overlay stays hidden until a game is tracked, and hides again when that game exits. Settings always shows it.
  - While hidden, only the FPS session runs, to spot games. The other sensors aren't polled.
- **Only show while game is focused** (`ShowOnlyWhenGameFocused=1`) needs the option above. It also hides the overlay while another app is in the foreground, and shows it again as soon as the game is focused. Foreground changes come from an out-of-context `SetWinEventHook`, so nothing is loaded into other processes. Sensors keep polling while the game is in the background, so the values are current when it returns.
- `config.ini` sits next to the exe and can also be edited by hand.

## How it stays out of the way
- **Small window:** the overlay window is only as big as the stats panel. It grows to the monitor size (minus one pixel) only while Settings is open. A full-screen topmost window would make Windows switch on Do Not Disturb and could stop games from using direct-flip presentation.
- **No handles to the game:** the app never opens the game process. It gets process names from a Toolhelp snapshot, notices the tracked game exiting when its window disappears, and reads FPS from ETW events, the same mechanism PresentMon and Xbox Game Bar use.

## Limitations
- The overlay can't draw over true exclusive-fullscreen games. It works over borderless and windowed games, and over DX11/12 games that use Windows fullscreen optimizations.
- Remote Desktop (mstsc) in the foreground takes the hotkey. Use the tray icon there.
- CPU temperature and power need an AMD Zen or Intel CPU (Intel is experimental). Other CPUs show N/A.
- Intel CPU and AMD GPU support is experimental and untested (see Hardware support).
- PawnIO is a kernel driver. Some kernel anti-cheats keep blocklists of hardware-access drivers. If a game refuses to start, uninstall PawnIO; only CPU temperature and power stop working.
