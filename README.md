# PrintScreenRegionSnip

A lightweight Windows 10/11 screenshot helper that keeps your `PrintScreen` habit unchanged while forcing a region-select flow.

## What it does

- Installs a global low-level keyboard hook for `PrintScreen` (`VK_SNAPSHOT`)
- Suppresses the default system handling for that key
- Captures the monitor under the mouse cursor (DXGI Desktop Duplication first, GDI fallback)
- Shows a full-screen frozen overlay with darkened background
- Lets you drag with left mouse to select a rectangle
- Keeps the frozen overlay open after selection so the region can be moved or resized
- Provides an on-overlay toolbar for move, arrow, pen, rectangle, ellipse, text, undo, color, cancel, and OK
- Copies the final annotated crop to clipboard (`CF_DIB`) after OK, Enter, double-clicking outside the selected region, or double-clicking anywhere while using a non-drawing tool
- `Esc`, right-click, or Cancel closes the frozen overlay without copying

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

On machines with Visual Studio 2026, keep the same `build` output directory and use:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Binary output:

- `build\Release\PrintScreenRegionSnip.exe`

## Install for current user (startup + silent background)

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1
```

This script:

- copies EXE to `%LOCALAPPDATA%\PrintScreenRegionSnip\PrintScreenRegionSnip.exe`
- writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\PrintScreenRegionSnip`

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
```

## Behavior notes

- Current version captures only the monitor containing the cursor.
- If DXGI returns a black frame, the app automatically falls back to GDI capture.
- Most borderless/fullscreen apps work.
- Some protected or anti-cheat-restricted content can still fail to capture due to OS/driver security boundaries.

## Next improvements

- Multi-monitor full virtual desktop freeze (single canvas across all displays)
- Optional delayed capture
- Optional PNG auto-save
- Tray icon + settings UI
