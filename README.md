# nanobot-windows-gui

A native Windows GUI manager for [nanobot](https://github.com/HKUDS/nanobot) — start, stop, and monitor nanobot gateway processes from a lightweight Win32 dashboard.

[中文文档](README_CN.md)

## Features

- Start/stop nanobot gateway (`nanobot gateway` or `python -m nanobot gateway`)
- Real-time health check via WinHTTP (`GET /health`)
- Auto-detect nanobot: one-click installer's managed venv (`~\.nanobot\venv`, incl. `%NANOBOT_VENV%`), `nanobot.exe` on PATH (pip console script), or Python + module fallback
- In-app one-click install button that runs the official nanobot installer in a visible PowerShell window and re-detects when it closes
- Native WebSocket chat client with streaming UI (delta/message) connected to the gateway WebUI port
- Conversation history persisted locally across manager restarts
- Global hotkey **Ctrl+Shift+N** to show/hide the window from any application (toggleable)
- Manual refresh-status button to re-check gateway state on demand
- Boot auto-start via registry (`HKCU\...\Run`)
- Silent auto-start mode (`--autostart`) for unattended launches
- Minimize to system tray with settings toggle
- Portable mode (place a `portable.flag` next to the executable) to keep config alongside it
- Bilingual UI (Chinese / English) with runtime switching
- Theme modes: system / dark / light (follows the Windows personalization setting)
- Configurable Gateway health and Manager WebUI ports; bootstrap secret stored in Windows Credential Manager (never written to `config.json`)
- NSIS installer with the nanobot mark icon, bilingual (Chinese/English) setup wizard, per-user install without administrator rights

## Requirements

- Windows 10 / 11 (64-bit)
- [Visual Studio 2022](https://visualstudio.microsoft.com/) or newer (VS 2026 supported), with C++ CMake tools — build only
- [Python 3.11+](https://www.python.org/) and nanobot (`nanobot-ai >= 0.2.2`). Recommended: the official one-click installer (PowerShell):

  ```powershell
  irm https://raw.githubusercontent.com/HKUDS/nanobot/main/scripts/install.ps1 | iex
  ```

  It creates an isolated venv at `~\.nanobot\venv` (no PATH or registry changes); the manager auto-detects that location, and the in-app install button runs the same command. Alternative: `python -m pip install -U "nanobot-ai>=0.2.2"`.

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

Run `nanobot-manager-setup.exe` and follow the wizard to install. The setup is **per-user** — it installs to `%LOCALAPPDATA%\Programs\nanobot-manager` and needs no administrator rights (matching nanobot's own install form). When upgrading from a pre-1.6 Program Files install, the setup asks you to run the old uninstaller first, then install fresh.

## Usage

```
nanobot-manager.exe              Open the GUI dashboard
nanobot-manager.exe --autostart  Silent mode: start nanobot and exit (for boot auto-start)
nanobot-manager.exe --delete-credential  Remove this user's saved bootstrap secret
nanobot-manager.exe --scrub-adjacent-legacy-auth  Internal elevated cleanup command
```

Inside the running app:

- **Ctrl+Shift+N** — show or hide the window from any application (toggleable in Settings).
- The manager does **not** stop the gateway when you close it — closing the manager (or quitting to tray) leaves nanobot running so background work continues. Stop the gateway explicitly from the dashboard when you want it down.

## Configuration

Installed builds store `config.json` in `%LOCALAPPDATA%\nanobot-manager\config.json`.
Portable builds (an adjacent `portable.flag`) and independent executable copies store it next to the executable:

```json
{
  "nanobot_path": null,
  "autostart_nanobot": false,
  "stay_in_tray": false,
  "hotkey_enabled": true,
  "health_check_port": 0,
  "webui_port": 0,
  "theme_mode": 0,
  "language": "zh"
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `nanobot_path` | string \| null | null | Override auto-detected command path |
| `autostart_nanobot` | bool | false | Launch nanobot when the manager starts |
| `stay_in_tray` | bool | false | Minimize to system tray instead of exiting |
| `hotkey_enabled` | bool | true | Enable the Ctrl+Shift+N global show/hide hotkey |
| `health_check_port` | int | 0 | 0 = auto: attach to the detected running Gateway (state file + probe, fallback 18790) and start new Gateways on 18790; any other value pins the port, applied on the next start (legacy serialized 18790 normalizes to auto) |
| `webui_port` | int | 0 | Chat/WebSocket port: 0 = auto-read from nanobot's config (`channels.websocket.port`, fallback 8765); any other value pins the port explicitly (legacy serialized 8765 normalizes to auto) |
| bootstrap secret | Credential Manager, then nanobot config | auto | Explicit Credential Manager entry wins; when unset the manager auto-reads nanobot's `channels.websocket.tokenIssueSecret` (falls back to `token`). Never serialized to `config.json` (legacy `webui_auth` migrates on startup) |
| `theme_mode` | int | 0 | 0 = follow system, 1 = dark, 2 = light |
| `language` | string | "zh" | UI language: `zh` or `en` |

Uninstall invokes `--delete-credential`, but Windows Credential Manager records are per-user: an elevated installer can only remove the credential for the Windows user it executes as. Use the in-app **Clear** action as the normal cleanup path.

On first startup after an upgrade from a legacy version, the manager migrates `webui_auth` into Credential Manager and removes the plaintext key. If the old config is under `Program Files`, Windows may show a UAC prompt solely to remove that adjacent legacy key; declining it cancels startup and leaves cleanup pending rather than claiming it is secure.

## Project Structure

```
nanobot-manager/
├── CMakeLists.txt          # Build configuration
├── assets/app.ico          # Application icon (nanobot mark)
├── installer/installer.nsi # NSIS installer script
├── scripts/
│   ├── build.bat           # One-click build script
│   └── package.bat         # Build + package installer
├── src/
│   ├── main.cpp            # Entry point, UI callbacks, tray, hotkey, theme/locale, health timers, chat wiring
│   ├── process.c/h         # Process management + WinHTTP health check + uptime
│   ├── config.c/h          # JSON config (cJSON) load/save (atomic) + legacy secret migration
│   ├── credential.c/h      # Windows Credential Manager storage for the bootstrap secret
│   ├── autostart.c/h       # Registry auto-start (`HKCU\...\Run`)
│   ├── chat.c/h            # WinHTTP WebSocket chat client (connect/send/stream)
│   ├── chat_protocol.c/h   # WebSocket JSON frame protocol (pure-logic, unit-tested)
│   ├── chat_history.c/h    # Local conversation history persistence (unit-tested)
│   ├── chat_ui.c/h         # Chat UI state + message array management
│   ├── resources.rc        # Windows resource (app icon)
│   └── version.h.in        # Version template (configure_file input)
└── ui/
    └── dashboard.uix       # Core UI layout
```

## Testing

Unit tests use a self-contained C test framework (`tests/nm_test.h`) and run under CTest. The suite covers config JSON migration/validation, Credential Manager store-load-delete on a per-process test target, process-helper logic (PID-reuse guards, uptime math), autostart registry logic, chat history persistence, and the chat WebSocket frame protocol.

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
