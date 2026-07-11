# nanobot-windows-gui

A native Windows GUI manager for [nanobot](https://github.com/HKUDS/nanobot) — start, stop, and monitor nanobot gateway processes from a lightweight Win32 dashboard.

[中文文档](README_CN.md)

## Features

- Start/stop nanobot gateway (`nanobot gateway` or `python -m nanobot gateway`)
- Real-time health check via WinHTTP (`GET /health`)
- Auto-detect `nanobot.exe` (pip console script) or fall back to Python + module
- Native WebSocket chat client with streaming UI (delta/message) connected to the gateway WebUI port
- Boot auto-start via registry (`HKCU\...\Run`)
- Silent auto-start mode (`--autostart`) for unattended launches
- Minimize to system tray with settings toggle
- Bilingual UI (Chinese / English) with runtime switching
- Theme modes: system / dark / light
- Configurable WebUI port and optional auth secret
- NSIS installer with cat emoji icon, bilingual (Chinese/English) setup wizard

## Requirements

- Windows 10 / 11 (64-bit)
- [Visual Studio 2022](https://visualstudio.microsoft.com/) or newer (VS 2026 supported), with C++ CMake tools
- [Python 3.x](https://www.python.org/) + `nanobot-ai` (`pip install nanobot-ai`)

## Build

```bat
cd nanobot-manager
scripts\build.bat
```

Output: `nanobot-manager\build\Release\nanobot-manager.exe`

> The build script uses CMake + MSVC. `core-ui` is included as a git submodule and built automatically.

### Package Installer

```bat
cd nanobot-manager
scripts\package.bat
```

Output: `nanobot-manager\dist\nanobot-manager-setup.exe`

> This runs `build.bat` first, then calls NSIS to create a setup executable.

## Installation

Download the latest installer from [GitHub Releases](https://github.com/aiguozhi123456/nanobot-windows-gui/releases).

Run `nanobot-manager-setup.exe` and follow the wizard to install.

## Usage

```
nanobot-manager.exe              Open the GUI dashboard
nanobot-manager.exe --autostart  Silent mode: start nanobot and exit (for boot auto-start)
```

## Configuration

A `config.json` file is created next to the executable on first run:

```json
{
  "nanobot_path": null,
  "autostart_nanobot": false,
  "stay_in_tray": false,
  "health_check_port": 18790,
  "webui_port": 8765,
  "webui_auth": "",
  "theme_mode": 0,
  "language": "zh"
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `nanobot_path` | string \| null | null | Override auto-detected command path |
| `autostart_nanobot` | bool | false | Launch nanobot when the manager starts |
| `stay_in_tray` | bool | false | Minimize to system tray instead of exiting |
| `health_check_port` | int | 18790 | Port for `/health` checks |
| `webui_port` | int | 8765 | Gateway WebUI port (chat/WebSocket); independent of health port |
| `webui_auth` | string | "" | Optional bootstrap auth secret (gateway `tokenIssueSecret`) |
| `theme_mode` | int | 0 | 0 = follow system, 1 = dark, 2 = light |
| `language` | string | "zh" | UI language: `zh` or `en` |

## Project Structure

```
nanobot-manager/
├── CMakeLists.txt          # Build configuration
├── assets/app.ico          # Application icon (cat emoji)
├── installer/installer.nsi # NSIS installer script
├── scripts/
│   ├── build.bat           # One-click build script
│   └── package.bat         # Build + package installer
├── src/
│   ├── main.cpp            # Entry point, UI callbacks, tray, theme/locale, health timers, chat wiring
│   ├── process.c/h         # Process management + WinHTTP health check
│   ├── config.c/h          # JSON config (cJSON) load/save
│   ├── autostart.c/h       # Registry auto-start (`HKCU\...\Run`)
│   ├── chat.c/h            # WinHTTP WebSocket chat client (connect/send/stream)
│   ├── chat_ui.c/h         # Chat UI state + message array management
│   ├── resources.rc        # Windows resource (app icon)
│   └── version.h.in        # Version template
└── ui/
    └── dashboard.uix       # Core UI layout
```

## Testing

Unit tests use a self-contained C test framework and run under CTest. They cover the config JSON round-trip (load/save/validation) and the autostart registry logic.

Build with tests (on by default):

```bat
cd nanobot-manager
cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

> `-DBUILD_TESTING=ON` controls whether tests are built (default on). Pass `-DBUILD_TESTING=OFF` to skip.

## License

[MIT](LICENSE)
