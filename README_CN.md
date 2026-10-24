# nanobot-windows-gui

Windows 原生 GUI 管理面板，用于管理 [nanobot](https://github.com/HKUDS/nanobot) 进程的启动、停止和状态监控。

[English](README.md)

## 功能

- 启动/停止 nanobot gateway（支持 `nanobot gateway` 和 `python -m nanobot gateway` 两种方式）
- 实时健康检查（WinHTTP `GET /health`）
- 自动检测 nanobot：一键安装的托管 venv（`~\.nanobot\venv`，含 `%NANOBOT_VENV%` 覆盖）→ PATH 上的 `nanobot.exe`（pip 入口点）→ Python + 模块回退
- 应用内一键安装按钮：在可见的 PowerShell 窗口中运行 nanobot 官方安装命令，窗口关闭后自动重新检测
- 原生 WebSocket 聊天客户端，流式显示 delta/message，连接 gateway WebUI 端口
- 本地持久化对话历史，管理器重启后恢复
- 全局快捷键 **Ctrl+Shift+N**，在任意应用中显示/隐藏窗口（可在设置中开关）
- 手动刷新状态按钮，按需重新检测 gateway 状态
- 开机自启（注册表 `HKCU\...\Run`）
- 静默自启模式（`--autostart`），用于无人值守启动
- 最小化到系统托盘（设置开关）
- 便携模式（在 exe 同目录放置 `portable.flag`），配置就近保存
- 中英双语界面，运行时切换
- 主题：跟随系统 / 深色 / 浅色（跟随系统时读取 Windows 个性化设置）
- 可配置 Gateway 健康端口与 Manager WebUI 端口，认证密钥保存到 Credential Manager（绝不写入 `config.json`）
- NSIS 安装包，nanobot mark 图标，中英文双语安装向导，per-user 安装无需管理员权限

## 依赖

- Windows 10/11（64 位）
- [Visual Studio 2022](https://visualstudio.microsoft.com/) 或更新版本（支持 VS 2026），含 C++ CMake 工具集 —— 仅构建需要
- [Python 3.11+](https://www.python.org/) 与 nanobot（`nanobot-ai >= 0.2.2`）。推荐使用官方一键安装（PowerShell）：

  ```powershell
  irm https://raw.githubusercontent.com/HKUDS/nanobot/main/scripts/install.ps1 | iex
  ```

  它会在 `~\.nanobot\venv` 创建独立 venv（不改 PATH、不写注册表）；管理器会自动检测该位置，应用内的一键安装按钮运行的也是同一条命令。备选方式：`python -m pip install -U "nanobot-ai>=0.2.2"`。

## 构建

```bat
cd nanobot-manager
scripts\build.bat
```

构建产物在 `nanobot-manager\build\Release\nanobot-manager.exe` 下。

> 构建使用 CMake + MSVC。`core-ui` 作为 git submodule 自动参与构建。

### 打包安装程序

```bat
cd nanobot-manager
scripts\package.bat
```

输出：`nanobot-manager\dist\nanobot-manager-setup.exe`

> 此脚本会先执行 `build.bat` 编译，再调用 NSIS 生成安装包。

## 安装

前往 [GitHub Releases](https://github.com/aiguozhi123456/nanobot-windows-gui/releases) 下载最新安装包。

运行 `nanobot-manager-setup.exe`，按照向导完成安装即可。安装为 **per-user** 形态 —— 装到 `%LOCALAPPDATA%\Programs\nanobot-manager`，无需管理员权限（与 nanobot 自身的安装形态一致）。从 1.6 之前的 Program Files 版本升级时，安装程序会先引导运行旧版卸载器，再全新安装。

## 使用

```
nanobot-manager.exe              打开 GUI 面板
nanobot-manager.exe --autostart  静默模式：启动 nanobot 后退出（用于开机自启）
nanobot-manager.exe --delete-credential  删除当前用户保存的引导认证密钥
nanobot-manager.exe --scrub-adjacent-legacy-auth  仅供内部提权清理使用
```

应用运行中：

- **Ctrl+Shift+N** —— 在任意应用中显示或隐藏窗口（可在“设置”中开关）。
- 关闭管理器**不会**停止 gateway —— 关闭/最小化到托盘时 nanobot 继续运行，方便后台任务持续。需要停止 gateway 请在面板中显式操作。

## 配置

安装版将 `config.json` 保存到 `%LOCALAPPDATA%\nanobot-manager\config.json`。带有同目录 `portable.flag` 的便携版，以及独立复制的 exe，均在 exe 同目录保存：

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

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `nanobot_path` | string \| null | null | 覆盖自动检测的命令路径 |
| `autostart_nanobot` | bool | false | 管理器启动时是否自动启动 nanobot |
| `stay_in_tray` | bool | false | 最小化到系统托盘而非退出 |
| `hotkey_enabled` | bool | true | 启用 Ctrl+Shift+N 全局显示/隐藏快捷键 |
| `health_check_port` | int | 0 | 0 = 自动：跟随已运行的 Gateway（状态文件 + 探测，回退 18790），新启动的 Gateway 使用 18790；其他值钉死端口，下次启动生效（旧版写入的 18790 会归一化为自动） |
| `webui_port` | int | 0 | 聊天/WebSocket 端口：0 = 自动读取 nanobot 配置（`channels.websocket.port`，回退 8765）；其他值显式钉死端口（旧版写入的 8765 会归一化为自动） |
| 引导认证密钥 | Credential Manager，其次 nanobot 配置 | 自动 | 凭据管理器中的显式条目优先；未设置时自动读取 nanobot 配置的 `channels.websocket.tokenIssueSecret`（回退 `token`）。绝不写入 `config.json`（旧版 `webui_auth` 启动时迁移） |
| `theme_mode` | int | 0 | 0 = 跟随系统，1 = 深色，2 = 浅色 |
| `language` | string | "zh" | 界面语言：`zh` 或 `en` |

卸载会调用 `--delete-credential`，但 Windows Credential Manager 凭据按用户隔离：提权安装程序只能清除其当前执行用户的凭据。正常清理请优先使用应用内的“清除”操作。

从旧版本首次升级启动时，管理器会将 `webui_auth` 迁移到 Credential Manager 并删除明文键。若旧配置位于 `Program Files`，Windows 可能显示 UAC 提示，仅用于清除 exe 相邻旧配置中的该明文键；取消提示会取消本次启动并保留“待清理”状态，不会静默宣称已安全。

## 项目结构

```
nanobot-manager/
├── CMakeLists.txt          # 构建配置
├── assets/app.ico          # 应用图标（nanobot mark）
├── installer/installer.nsi # NSIS 安装脚本
├── scripts/
│   ├── build.bat           # 一键构建脚本
│   └── package.bat         # 构建 + 打包安装程序
├── src/
│   ├── main.cpp            # 入口、UI 回调、托盘、快捷键、主题/语言、健康定时器、聊天接线
│   ├── process.c/h         # 进程管理 + WinHTTP 健康检查 + uptime
│   ├── config.c/h          # JSON 配置（cJSON）读写（原子写入）+ 旧版密钥迁移
│   ├── credential.c/h      # 引导认证密钥的 Windows Credential Manager 存储
│   ├── autostart.c/h       # 注册表自启（`HKCU\...\Run`）
│   ├── chat.c/h            # WinHTTP WebSocket 聊天客户端（连接/发送/流式接收）
│   ├── chat_protocol.c/h   # WebSocket JSON 帧协议（纯逻辑，已单测）
│   ├── chat_history.c/h    # 本地对话历史持久化（已单测）
│   ├── chat_ui.c/h         # 聊天 UI 状态 + 消息数组管理
│   ├── resources.rc        # Windows 资源文件（应用图标）
│   └── version.h.in        # 版本模板（configure_file 输入）
└── ui/
    └── dashboard.uix       # Core UI 布局
```

## 测试

单元测试使用内置的 C 测试框架（`tests/nm_test.h`），通过 CTest 运行。覆盖 config JSON 的迁移/校验、使用每进程测试 target 的 Credential Manager 存取删除、进程辅助逻辑（PID 复用保护、uptime 计算）、开机自启注册表逻辑、聊天历史持久化，以及聊天 WebSocket 帧协议。

带测试构建（默认开启）：

```bat
cd nanobot-manager
cmake -B build -S . -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

> `-DBUILD_TESTING=ON` 控制是否构建测试（默认开启）。传 `-DBUILD_TESTING=OFF` 可跳过。

## 许可证

[MIT](LICENSE)
