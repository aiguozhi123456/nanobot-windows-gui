# AGENTS.md

Native Windows GUI manager for [nanobot](https://github.com/HKUDS/nanobot). Product code lives in `nanobot-manager/`. `core-ui/` is a **git submodule** (UI framework), not the product.

## Layout (what to edit)

| Path | Role |
|------|------|
| `nanobot-manager/src/main.cpp` | WinMain, UI mount callbacks, tray, theme/locale, health timers, chat UI wiring |
| `nanobot-manager/src/process.c` | Find/start/stop nanobot, WinHTTP `/health` |
| `nanobot-manager/src/config.c` | installed/portable `config.json`, atomic saves, legacy secret migration (cJSON) |
| `nanobot-manager/src/credential.c` | Windows Credential Manager storage for the WebUI bootstrap secret |
| `nanobot-manager/src/i18n.c` | zh/en language tables (`i18n_table` / `i18n_status`), single-source status table |
| `nanobot-manager/src/logger.c` | Unified log pipeline (`nm_log`, level/module prefixes, SRWLOCK, 512 KiB `.bak` rotation → `nanobot-manager.log` beside exe) |
| `nanobot-manager/src/chat.c` | WebUI bootstrap + WinHTTP WebSocket chat client (connect/send/stream) |
| `nanobot-manager/src/autostart.c` | `HKCU\...\Run` value `nanobot-manager` → `"exe" --autostart` |
| `nanobot-manager/ui/dashboard.uix` | Core UI SFC (layout + reactive state, chat page) |
| `nanobot-manager/installer/installer.nsi` | NSIS setup (needs prior Release build). **Per-user**: `$LOCALAPPDATA\Programs\nanobot-manager`, `RequestExecutionLevel user`; legacy Program Files installs are routed through their own (elevating) uninstaller first |
| `nanobot-manager/CMakeLists.txt` | App target; pulls `../core-ui` + FetchContent cJSON |
| `core-ui/` | Submodule only — do not treat as primary app source |
| `docs/solutions/` | Documented solutions to past problems (bugs, best practices, architecture patterns), organized by category with YAML frontmatter (`module`, `tags`, `problem_type`). Relevant when implementing or debugging in documented areas |
| `CONCEPTS.md` | Shared domain vocabulary (entities, named processes, status concepts) — relevant when orienting to the codebase or discussing domain concepts |

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

Native module tests run through CTest and CI uses the Windows MSVC build. Verification for release-facing changes = build + CTest + relevant manual run.

```bat
ctest --test-dir nanobot-manager\build -C Release --output-on-failure
```

## Runtime behavior agents miss

- **Start command**: `nanobot.exe gateway --background --port N`, else `python.exe -m nanobot gateway --background --port N` (`CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP`). Candidate order: config `nanobot_path` → `nanobot.exe` on PATH → one-click managed venv `%NANOBOT_VENV%` or `%USERPROFILE%\.nanobot\venv` (`Scripts\nanobot.exe`, then `Scripts\python.exe -m nanobot`) → py/python on PATH. Full resolution probes for `--background` support.
- **Health**: `GET http://localhost:{health_check_port}/health` via WinHTTP; HTTP 200 = running. Default port **18790**.
- **WebUI open**: port and bootstrap secret auto-read from nanobot's own config (`~\.nanobot\config.json` → `channels.websocket`; port default **8765**, secret = `tokenIssueSecret` else `token`). Manager `webui_port` **0/empty = auto** (default; legacy serialized 8765 normalizes to 0), any other value pins the port; refresh points = boot, RUNNING transitions, settings apply. "Open WebUI" appends `#/?bootstrapSecret=…` when a secret is known (mirrors upstream `_webui_browser_url`). Not `health_check_port + 1` (a machine can coincidentally set `websocket.port = gateway.port + 1`).
- **Chat**: native page talks to the same WebUI host via `GET /webui/bootstrap` (secret as `X-Nanobot-Auth` header) then WebSocket (see `chat.c`). Secret precedence: **Credential Manager → nanobot config auto-read → none**, resolved in one funnel (`set_chat_endpoint_effective`). The Credential Manager entry is still the explicit override; legacy `webui_auth` migrates there on startup.
- **One-click install button** (`btn_install_nanobot`, shown on the `!installed` banner): spawns a visible PowerShell running upstream's `irm .../scripts/install.ps1 | iex` (`-NoExit`), re-resolves on window close (`WM_INSTALLER_EXITED`). Upstream install = system Python 3.11+ + venv at `~\.nanobot\venv`, no PATH/registry changes.
- **`--autostart`**: if `stay_in_tray` is false → start nanobot (if needed) and exit; if tray enabled → silent tray + optional start. Logs (all paths): `nanobot-manager.log` beside exe via the unified logger (`src/logger.c`; module prefixes `[autostart]`/`[process]`/`[app]`, level + module per line, `log_level` config filters); the gateway launcher's stdout/stderr is redirected to `nanobot-gateway.log` beside exe (file redirection, never a pipe — see `docs/solutions/architecture-patterns/logging-pipeline.md`).
- **config.json** (exe directory) fields in code (README table is incomplete):

  `nanobot_path`, `autostart_nanobot`, `stay_in_tray`, `health_check_port` (0 = auto: attach to the detected running Gateway, start new ones on 18790; legacy serialized 18790 normalizes to 0; other values pin), `webui_port` (0 = auto-detect from nanobot config, the default; legacy 8765 normalizes to 0; other values pin the port), `theme_mode` (0=system, 1=dark, 2=light), `language` (`zh`/`en`), `log_level` (`debug`/`info`/`warn`/`error`, default `info`; settable from the settings page). Legacy `webui_auth` is migration-only and is never serialized.

- **i18n**: zh/en strings are **embedded** in `src/i18n.c` (`i18n_table()` feeds `ui_page_load_language_string`; `i18n_status()` resolves status keys for C++ pushes), not external `.lang` files; `tests/test_i18n.c` enforces zh/en key parity. UI copy changes often need both `.uix` (`$t('key')`) and those strings. Bilingual `MessageBoxW` dialog strings intentionally stay inline in `main.cpp` (`is_zh()` ternaries), outside the language-table system.
- **UI wiring**: widget `id`s in `dashboard.uix` must match `ui_page_on_widget_mount(..., "id", ...)` in `main.cpp`. Native clicks/toggles are mounted from C++; reactive fields use `ui_page_set_*` / page `data()`.
- **Dynamic hints in settings**: input `placeholder` is static-only in core-ui (no runtime setter, and `placeholder` is not a reactive binding property in `ApplyBindingToWidget`) — a `:placeholder="expr"` binding compiles but silently no-ops. For live values (current effective port, credential source), drive a model string from C++ and render it in a label (`{{ webuiPortSub }}` / `{{ credentialStatus }}` pattern), refreshed wherever the underlying value or the language changes.
- **core-ui API**: when changing UI patterns, prefer `core-ui/docs/uix-ai-guide.md` (and `core-ui/include/ui_core.h`) over inventing APIs. Submodule pin is independent of manager version.

## Conventions

- C11 + C++17, `/utf-8`, `UNICODE`/`_UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`.
- Prefer existing Win32 + Core UI patterns; no Qt/.NET/Electron.
- Do not commit `build/`, `dist/`, or generated VS project artifacts (see root `.gitignore`).
- After cloning: `git submodule update --init --recursive` so `core-ui` exists before build.
