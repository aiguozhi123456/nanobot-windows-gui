# nanobot-windows-gui

Windows 原生 GUI 管理面板，用于管理 [nanobot](https://github.com/HKUDS/nanobot) 进程的启动、停止和状态监控。

[English](README.md)

## 功能

- 启动/停止 nanobot gateway（支持 `nanobot gateway` 和 `python -m nanobot gateway` 两种方式）
- 实时健康检查（WinHTTP `GET /health`）
- 自动检测 `nanobot.exe`（pip 入口点）或回退到 Python + 模块方式
- 原生 WebSocket 聊天客户端，流式显示 delta/message，连接 gateway WebUI 端口
- 开机自启（注册表 `HKCU\...\Run`）
- 静默自启模式（`--autostart`），用于无人值守启动
- 最小化到系统托盘（设置开关）
- 中英双语界面，运行时切换
- 主题：跟随系统 / 深色 / 浅色
- 可配置 WebUI 端口与可选认证 secret
- NSIS 安装包，猫表情图标，中英文双语安装向导

## 依赖

- Windows 10/11（64 位）
- [Visual Studio 2022](https://visualstudio.microsoft.com/) 或更新版本（支持 VS 2026），含 C++ CMake 工具集
- [Python 3.x](https://www.python.org/) + nanobot-ai（`pip install nanobot-ai`）

## 构建

```bat
cd nanobot-manager
scripts\build.bat
```

构建产物在 `nanobot-manager\build\Release\` 下。

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

运行 `nanobot-manager-setup.exe`，按照向导完成安装即可。

## 使用

```
nanobot-manager.exe              打开 GUI 面板
nanobot-manager.exe --autostart  静默模式：启动 nanobot 后退出（用于开机自启）
```

## 配置

首次运行时在 exe 同目录生成 `config.json`：

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

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `nanobot_path` | string \| null | null | 覆盖自动检测的命令路径 |
| `autostart_nanobot` | bool | false | 管理器启动时是否自动启动 nanobot |
| `stay_in_tray` | bool | false | 最小化到系统托盘而非退出 |
| `health_check_port` | int | 18790 | `/health` 检查端口 |
| `webui_port` | int | 8765 | gateway WebUI 端口（聊天/WebSocket），与 health 端口相互独立 |
| `webui_auth` | string | "" | 可选的引导认证 secret（gateway `tokenIssueSecret`） |
| `theme_mode` | int | 0 | 0 = 跟随系统，1 = 深色，2 = 浅色 |
| `language` | string | "zh" | 界面语言：`zh` 或 `en` |

## 项目结构

```
nanobot-manager/
├── CMakeLists.txt          # 构建配置
├── assets/app.ico          # 应用图标（猫表情）
├── installer/installer.nsi # NSIS 安装脚本
├── scripts/
│   ├── build.bat           # 一键构建脚本
│   └── package.bat         # 构建 + 打包安装程序
├── src/
│   ├── main.cpp            # 入口、UI 回调、托盘、主题/语言、健康定时器、聊天接线
│   ├── process.c/h         # 进程管理 + WinHTTP 健康检查
│   ├── config.c/h          # JSON 配置（cJSON）读写
│   ├── autostart.c/h       # 注册表自启（`HKCU\...\Run`）
│   ├── chat.c/h            # WinHTTP WebSocket 聊天客户端（连接/发送/流式接收）
│   ├── chat_ui.c/h         # 聊天 UI 状态 + 消息数组管理
│   ├── resources.rc        # Windows 资源文件（应用图标）
│   └── version.h.in        # 版本模板
└── ui/
    └── dashboard.uix       # Core UI 布局
```

## 测试

单元测试使用内置的 C 测试框架，通过 CTest 运行。覆盖 config JSON 的往返（加载/保存/校验）和开机自启注册表逻辑。

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
