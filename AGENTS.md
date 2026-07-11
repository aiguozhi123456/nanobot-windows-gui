# AGENTS.md

Native Windows GUI manager for [nanobot](https://github.com/HKUDS/nanobot). Product code lives in `nanobot-manager/`. `core-ui/` is a **git submodule** (UI framework), not the product.

## Layout (what to edit)

| Path | Role |
|------|------|
| `nanobot-manager/src/main.cpp` | WinMain, UI mount callbacks, tray, theme/locale, health timers |
| `nanobot-manager/src/process.c` | Find/start/stop nanobot, WinHTTP `/health` |
| `nanobot-manager/src/config.c` | `config.json` next to the exe (cJSON) |
| `nanobot-manager/src/autostart.c` | `HKCU\...\Run` value `nanobot-manager` → `"exe" --autostart` |
| `nanobot-manager/ui/dashboard.uix` | Core UI SFC (layout + reactive state) |
| `nanobot-manager/installer/installer.nsi` | NSIS setup (needs prior Release build) |
| `nanobot-manager/CMakeLists.txt` | App target; pulls `../core-ui` + FetchContent cJSON |
| `core-ui/` | Submodule only — do not treat as primary app source |

Generated at configure time: `build/generated/version.h`, `build/generated/version.nsh` from `NANOBOT_MANAGER_VERSION` in CMakeLists (`1.5.0` today).

## Build & package

Windows only. MSVC (or VS generator); **not MinGW**.

```bat
cd nanobot-manager
scripts\build.bat
```

- Output: `nanobot-manager\build\Release\nanobot-manager.exe`
- POST_BUILD copies `ui/` and `core-ui.dll` beside the exe — run from that directory (or install layout). Missing `ui\dashboard.uix` → load failure MessageBox.
- First configure FetchContents **cJSON** (manager) and **QuickJS-NG** (via core-ui); needs network once.
- `build.bat` sets `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` so CMake 4.x accepts cJSON’s old `cmake_minimum_required`.
- Manager CMake forces `UI_CORE_BUILD_STATIC_ARCHIVE OFF` (skip fat static archive; faster).

Installer (requires NSIS `makensis` on PATH or default install path):

```bat
cd nanobot-manager
scripts\package.bat
```

→ `nanobot-manager\dist\nanobot-manager-setup.exe` (runs `build.bat` first).

No repo-level unit/integration test suite or CI for the manager. Verification = build + manual run.

## Runtime behavior agents miss

- **Start command**: `nanobot.exe gateway`, else `python.exe -m nanobot gateway` (`CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP`).
- **Health**: `GET http://localhost:{health_check_port}/health` via WinHTTP; HTTP 200 = running. Default port **18790**.
- **WebUI open**: `http://localhost:{health_check_port + 1}` (hardcoded offset in `main.cpp`).
- **`--autostart`**: if `stay_in_tray` is false → start nanobot (if needed) and exit; if tray enabled → silent tray + optional start. Logs: `nanobot-manager-autostart.log` / `nanobot-manager-process.log` beside exe.
- **config.json** (exe directory) fields in code (README table is incomplete):

  `nanobot_path`, `autostart_nanobot`, `stay_in_tray`, `health_check_port`, `theme_mode` (0=system, 1=dark, 2=light), `language` (`zh`/`en`).

- **i18n**: zh/en strings are **embedded** in `main.cpp` via `ui_page_load_language_string`, not external `.lang` files. UI copy changes often need both `.uix` (`$t('key')`) and those strings.
- **UI wiring**: widget `id`s in `dashboard.uix` must match `ui_page_on_widget_mount(..., "id", ...)` in `main.cpp`. Native clicks/toggles are mounted from C++; reactive fields use `ui_page_set_*` / page `data()`.
- **core-ui API**: when changing UI patterns, prefer `core-ui/docs/uix-ai-guide.md` (and `core-ui/include/ui_core.h`) over inventing APIs. Submodule pin is independent of manager version.

## Conventions

- C11 + C++17, `/utf-8`, `UNICODE`/`_UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`.
- Prefer existing Win32 + Core UI patterns; no Qt/.NET/Electron.
- Do not commit `build/`, `dist/`, or generated VS project artifacts (see root `.gitignore`).
- After cloning: `git submodule update --init --recursive` so `core-ui` exists before build.
