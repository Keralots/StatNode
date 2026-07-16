# PCMonitorColor Companion 4.1

The desktop companion discovers PC hardware sensors and sends selected values to
PCMonitorColor over UDP. Its web-style interface runs in a native window and
stays available from the system tray.

## Responsibilities

- The companion owns device targeting, sensor discovery, sensor selection,
  optional stream aliases, update frequency, autostart, logs, and backups.
- The PCMonitorColor device portal owns screen slots, per-slot labels, display
  styles, colors, brightness, clock settings, touch input, WiFi, and firmware.

The Device page reads the firmware's `/api/status` endpoint and displays the
real `/screen.bmp` capture. The retired 128x64 OLED emulator and layout-push
pipeline are not part of this companion.

## Folders

| Folder | Purpose |
| --- | --- |
| `win-companion/` | Windows sensor backend, tray integration, autostart, and PyInstaller build. |
| `linux-companion/` | Linux sensor backend and systemd user autostart. |
| `companion-common/` | Shared state, configuration migration, local HTTP server, tests, and web UI. |

## Protocol

The companion sends UDP JSON to port 4210 using wire version 2.2. Each metric
contains an integer `id`, `name`, `value`, and `unit`. Persistent sensor IDs keep
device slot bindings stable when sensors are reordered, deselected, or
temporarily unavailable.

See the platform-specific README files for setup and packaging instructions.
