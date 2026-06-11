# nanobot-windows-gui

A native Windows GUI manager for [nanobot](https://github.com/HKUDS/nanobot) — start, stop, and monitor nanobot gateway processes from a lightweight Win32 dashboard.

[中文文档](README_CN.md)

## Features

- Start/stop nanobot gateway (`nanobot gateway` or `python -m nanobot gateway`)
- Real-time health check via WinHTTP (`GET /health`)
- Auto-detect `nanobot.exe` (pip console script) or fall back to Python + module
- Boot auto-start via registry (`HKCU\...\Run`)
- Silent auto-start mode (`--autostart`) for unattended launches

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
├── scripts/build.bat       # One-click build script
├── src/
│   ├── main.cpp            # Entry point, UI callbacks, async workers
│   ├── process.c/h         # Process management + health check
│   ├── autostart.c/h       # Registry auto-start read/write
│   └── config.c/h          # JSON config load/save
└── ui/
    └── dashboard.uix       # Core UI layout
```

## License

[MIT](LICENSE)
