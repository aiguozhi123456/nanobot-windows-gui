# nanobot-manager

Windows 原生 GUI 管理面板，用于管理 [nanobot](https://github.com/HKUDS/nanobot) 进程的启动、停止和状态监控。

## 功能

- 启动/停止 nanobot 进程（`python -m nanobot gateway`）
- 实时健康检查（WinHTTP GET `/health`）
- 开机自启（注册表 HKCU Run）
- 自动检测 Python + nanobot 安装路径
- 静默自启模式（`--autostart`）

## 依赖

- Windows 10/11
- [core-ui](https://github.com/ghboke/core-ui)（Win32 Direct2D UI 框架）
- Visual Studio 2022 + MSVC（CMake 构建）
- Python 3.x + nanobot-ai（`pip install nanobot-ai`）

## 构建

```bat
cd nanobot-manager
scripts\build.bat
```

构建产物在 `nanobot-manager/build/Release/` 下。

## 使用

直接运行 `nanobot-manager.exe`，或通过命令行参数：

- `nanobot-manager.exe` — 打开 GUI 面板
- `nanobot-manager.exe --autostart` — 静默模式：检测并启动 nanobot 后退出（用于开机自启）

## 配置

配置文件位于 `%APPDATA%\nanobot-manager\config.json`：

```json
{
  "nanobot_path": null,
  "autostart_nanobot": false,
  "health_check_port": 18790
}
```

## 项目结构

```
nanobot-manager/
├── CMakeLists.txt
├── scripts/build.bat
├── src/
│   ├── main.cpp          # 入口：窗口创建 + 消息循环
│   ├── process.c/h       # 进程管理 + 健康检查
│   ├── autostart.c/h     # 注册表自启读写
│   └── config.c/h        # JSON 配置读写
└── ui/
    └── dashboard.uix     # Core UI 状态卡片
```

## License

[MIT](LICENSE)
