# nanobot-windows-gui

Windows 原生 GUI 管理面板，用于管理 [nanobot](https://github.com/HKUDS/nanobot) 进程的启动、停止和状态监控。

[English](README.md)

## 功能

- 启动/停止 nanobot gateway（支持 `nanobot gateway` 和 `python -m nanobot gateway` 两种方式）
- 实时健康检查（WinHTTP `GET /health`）
- 自动检测 `nanobot.exe`（pip 入口点）或回退到 Python + 模块方式
- 开机自启（注册表 `HKCU\...\Run`）
- 静默自启模式（`--autostart`），用于无人值守启动
- 便携式 — 配置文件位于 exe 同目录，不在系统留痕

## 依赖

- Windows 10/11（64 位）
- [Visual Studio 2022](https://visualstudio.microsoft.com/) + C++ CMake 工具集
- [Python 3.x](https://www.python.org/) + nanobot-ai（`pip install nanobot-ai`）

## 构建

```bat
cd nanobot-manager
scripts\build.bat
```

构建产物在 `nanobot-manager\build\Release\` 下。

> 构建使用 CMake + MSVC。`core-ui` 作为 git submodule 自动参与构建。

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
  "health_check_port": 18790
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `nanobot_path` | string \| null | 覆盖自动检测的命令路径 |
| `autostart_nanobot` | bool | 管理器启动时是否自动启动 nanobot |
| `health_check_port` | int | `/health` 检查端口（默认 `18790`） |

## 项目结构

```
nanobot-manager/
├── CMakeLists.txt          # 构建配置
├── scripts/build.bat       # 一键构建脚本
├── src/
│   ├── main.cpp            # 入口、UI 回调、异步工作线程
│   ├── process.c/h         # 进程管理 + 健康检查
│   ├── autostart.c/h       # 注册表自启读写
│   └── config.c/h          # JSON 配置读写
└── ui/
    └── dashboard.uix       # Core UI 布局
```

## 许可证

[MIT](LICENSE)
