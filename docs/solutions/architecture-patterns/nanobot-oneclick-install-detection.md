---
title: "nanobot 一键安装形态的发现与对齐（不落 PATH 的安装如何被检测）"
date: 2026-10-24
last_updated: 2026-10-24
category: architecture-patterns
module: nanobot-manager/process
problem_type: integration_alignment
component: process
severity: medium
applies_when:
  - 上游 nanobot 改变安装形态（venv 位置、入口脚本、PATH/shim 策略）
  - manager "未找到 nanobot" 但用户确认已安装
  - 新增解析候选层或调整候选优先级
  - 维护应用内一键安装按钮或 per-user 安装器
tags: [nanobot, install, venv, one-click, resolution, installer, nsis, per-user, upstream, integration]
---

# nanobot 一键安装形态的发现与对齐（不落 PATH 的安装如何被检测）

## Context

上游 nanobot（HKUDS）推出 Windows 一键安装（PowerShell 一行命令）：

```powershell
irm https://raw.githubusercontent.com/HKUDS/nanobot/main/scripts/install.ps1 | iex
```

install.ps1 的安装形态与此前 manager 假设的所有安装方式都不同：

- **需要系统 Python 3.11+**（不下载自带 Python；经 `PYTHON` → `python` → `py` 查找）
- 在 **`%USERPROFILE%\.nanobot\venv`** 创建独立 venv（`NANOBOT_VENV` 环境变量可覆盖），从 PyPI 安装 `nanobot-ai`
- **不改 PATH、不写注册表、不建 shim** —— 对 `SearchPathW` 完全不可见
- 入口：`venv\Scripts\nanobot.exe`（wheel console script）或 `venv\Scripts\python.exe -m nanobot`
- 安装凭据 `venv\nanobot-install.json`（`owner: nanobot-installer`）；结尾自动跑 `webui --yes` 或 `onboard --wizard`
- 也支持 uv tool / pipx 接管已有安装（那两类会往 PATH 放 shim，原探测层可见）

改动前的解析顺序是「config 路径 → PATH `nanobot.exe` → PATH 上的 py/python `-m nanobot`」——一键安装三者全探不到：不进 PATH，系统 Python 里也没有 nanobot 模块。用户装完官方一键包后 manager 显示"未找到"，只能手填 `nanobot_path`。

## Solution

三层对齐（manager v1.6.0）：

### 1. 解析器新增"托管 venv"探测层

`process.c` `nanobot_resolve_candidates` 的候选顺序变为：

```
config nanobot_path
→ PATH nanobot.exe                    （存量行为不动，优先级保留）
→ 托管 venv（新）                      %NANOBOT_VENV% 或 ~\.nanobot\venv
                                       Scripts\nanobot.exe (is_python=0)
                                       Scripts\python.exe   (is_python=1)
→ PATH py.exe / python{,3,w}.exe -m nanobot
```

要点：

- **`managed_venv_dir()` 与 install.ps1 的解析语义严格一致**：`NANOBOT_VENV` 非空即用（either/or，不做双探测），否则 `SHGetKnownFolderPath(FOLDERID_Profile)` + `\.nanobot\venv`。
- venv python 候选会被既有的 `gateway --help` 探测门控过滤——一个恰好位于该路径但没装 nanobot 模块的无关 venv 会在数秒探测后被拒绝，与 PATH python 层同代价。
- **候选来源随候选返回**（`nm_source` 数组，新增 `NM_SRC_MANAGED_VENV`）：归因不再靠 `nanobot_resolve` 里事后猜（`!py` 就标 PATH_NANOBOT 的做法会误标 venv 候选）。`nanobot_resolve_candidates` 的签名因此带上 `nm_source** out_sources`。
- `nanobot_stop` 的兜底解析（quick=1）和状态文件 `command` 推导自动受益，无需改动。

### 2. 应用内一键安装按钮

`!installed` 横幅（dashboard.uix）里加 `btn_install_nanobot`：

- 确认框显示**命令原文** + Python 3.11+ 前提，用户显式确认
- `SearchPathW` 找 powershell.exe，拉起**可见**窗口运行 `-NoExit -ExecutionPolicy Bypass -Command "irm ... | iex"`——`-NoExit` 是必须的：安装脚本结尾要起 webui/onboard 向导，窗口需保持到用户完成
- `RegisterWaitForSingleObject`（`WT_EXECUTEONLYONCE`）监听安装器进程，窗口关闭 → `WM_INSTALLER_EXITED` → 复位 `installing` → `kick_resolve_worker(intent=0)` 重新检测，横幅随 `installed` 反应量自动消失。注意 **"窗口关闭"≠"脚本跑完"**（-NoExit），刷新按钮仍是手动兜底
- 若 wait 注册失败：不置 `installing`（避免卡死态），直接关句柄，靠刷新按钮
- 退出清理：`installer_unwatch()`（UnregisterWaitEx 阻塞排空回调）→ CloseHandle——关句柄不杀用户的 PowerShell 窗口。与 process.c `nanobot_unwatch_handle` 同一纪律：**必须先排空 wait 再关句柄**
- 安全边界：命令是上游 README 原文、确认框可见全文、控制台全程可见、不提权

### 3. 我们自己的安装器改 per-user（对齐上游安装哲学）

`installer.nsi`：`$LOCALAPPDATA\Programs\nanobot-manager` + `RequestExecutionLevel user` + 显式 `SetShellVarContext current`（安装与卸载两个进程各声明一次）。升级路径：

- 旧位置**可非提权写入**（旧的 per-user 自定义目录）→ 沿用原地升级（`.onInit` 里 FileOpen 写测试）
- 旧位置不可写（Program Files 时代的提权安装）→ 引导先跑旧版卸载器（`ExecShell "runas"`，由它自己触发 UAC），本安装器 Abort，用户卸载后重跑
- 旧注册表项存在但 uninstall.exe 已不在（脏残留）→ 直接全新安装，`-Post` 节重写 HKCU 键

不做"静默并行安装两份"：旧安装键与新安装键同为 HKCU，并行会互相吞掉卸载入口、把旧文件变成孤儿。

## Why This Matters

- **"装了但找不到"是安装形态对齐的核心故障模式**。凡是上游改变"东西放哪"（PATH/注册表/canonical 目录）的决策，consumer 的发现逻辑都要跟着动；这次是 canonical 目录 + 环境变量覆盖，下次可能是别的——对齐点集中在 `managed_venv_dir()` 一处，改起来有明确入口。
- **优先级是有意保守的**：PATH `nanobot.exe` 压在 venv 之前，保住存量用户（pipx/uv tool/pip --user）的行为不变；config 覆盖永远最高。若上游把 venv 变成唯一官方形态，再讨论调序。
- **per-user 安装器与我们已有的 per-user 基础设施同构**：config 本来就在 `%LOCALAPPDATA%\nanobot-manager`，autostart 本来就是 HKCU Run，Credential Manager 本来就按用户隔离——exe 挪到 LOCALAPPDATA 后整套产品完全无管理员依赖。

## When to Apply

- 上游 install.ps1 改 venv 路径或 `NANOBOT_VENV` 语义 → 改 `managed_venv_dir()` + 本文档
- 上游取消 wheel console script（`Scripts\nanobot.exe` 消失）→ venv python 候选已覆盖，确认探测仍通过即可
- 上游推出独立 exe / 服务安装形态 → 需要新的候选层（不是改本层），并评估是否绕过 Python 层
- manager "未找到" 排查：看 `nanobot-manager.log` 的 `[process]` 行的候选与探测结果（候选与探测细节为 DEBUG 级，需在设置页把日志级别切到 debug），先确认 venv 层是否命中

## Related

- `docs/solutions/architecture-patterns/state-driven-gateway-lifecycle.md` — 解析候选顺序的上游文档（已同步本层）
- `docs/solutions/reference/nanobot-gateway-overview.md` — 上游安装形态事实基线
- `nanobot-manager/src/process.c` — `managed_venv_dir()`、候选构建器
- `nanobot-manager/src/main.cpp` — `on_install_nanobot()`、`WM_INSTALLER_EXITED`
- `CONCEPTS.md` — "Managed Venv Install" 词条
