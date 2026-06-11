# nanobot-windows-gui

A native Windows GUI manager for [nanobot](https://github.com/HKUDS/nanobot) — start, stop, and monitor nanobot gateway processes from a lightweight Win32 dashboard.

[中文文档](README_CN.md)

## Features

- Start/stop nanobot gateway (`nanobot gateway` or `python -m nanobot gateway`)
- Real-time health check via WinHTTP (`GET /health`)
- Auto-detect `nanobot.exe` (pip console script) or fall back to Python + module
- Boot auto-start via registry (`HKCU\...\Run`)
- Silent auto-start mode (`--autostart`) for unattended launches
- NSIS installer with cat emoji icon, bilingual (Chinese/English) setup wizard

## Requirements

- Windows 10 / 11 (64-bit)
- [Visual Studio 2022](https://visualstudio.microsoft.com/) with C++ CMake tools
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
  "health_check_port": 18790
}
```

| Field | Type | Description |
|---|---|---|
| `nanobot_path` | string \| null | Override auto-detected command path |
| `autostart_nanobot` | bool | Launch nanobot when the manager starts |
| `health_check_port` | int | Port for `/health` checks (default `18790`) |

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
│   ├── main.cpp            # Entry point, UI callbacks, async workers
│   ├── process.c/h         # Process management + health check
│   ├── autostart.c/h       # Registry auto-start read/write
│   ├── config.c/h          # JSON config load/save
│   └── resources.rc        # Windows resource (app icon)
└── ui/
    └── dashboard.uix       # Core UI layout
```

## License

[MIT](LICENSE)
