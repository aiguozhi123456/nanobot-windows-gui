---
title: "nanobot Gateway Overview — 上游被托管网关的整体画像"
date: 2026-07-11
category: reference
module: upstream/nanobot
problem_type: reference_overview
component: tooling
severity: info
applies_when:
  - 适配 nanobot 新版本变更（端口、API 帧、bootstrap 结构、配置字段）
  - 排查 manager 与 gateway 之间的健康检查 / 聊天 WebSocket / bootstrap 握手问题
  - 扩展 chat 集成（新增事件帧处理、历史会话管理、reasoning 流显示等）
  - 评估上游破坏性变更对 manager 兼容层的风险等级
tags: [nanobot, gateway, websocket, http-api, upstream, integration, python]
---

# nanobot Gateway Overview — 上游被托管网关的整体画像

## Overview

nanobot 是 HKUDS 的开源、超轻量、可自托管的个人 AI agent 运行时（MIT，作者 Xubin Ren / re-bin）。其核心是一个极简 agent loop：消息 → LLM 决定调用工具 → 将记忆与技能作为上下文注入。支持多入口（WebUI / CLI / Telegram / Discord / Slack / WeChat / Feishu / Email 等），丰富的工具集（文件 / shell / web / MCP / cron / 图像生成 / subagents），以及持久化（会话历史 + Dream 长期记忆 + 长程目标 `/goal`）。

pip 包名为 **`nanobot-ai`**（注意不是 `nanobot`），可执行命令为 `nanobot`，要求 Python ≥ 3.11，Windows 官方支持（v0.1.5.post2 起）。当前稳定版 **v0.2.2** "Durability Release"（2026-06-21），节奏极快（几乎每日更新），33+ contributors。

本仓库 `nanobot-manager` 是 nanobot 的 **Windows 原生 GUI 管理器**：它负责启动 / 停止 `nanobot gateway` 长驻进程、健康检查、并提供一个内嵌的原生聊天页面。本文档为"适配 nanobot 版本变化"提供完整的事实基线。

## Gateway 子命令与进程模型

`nanobot gateway` 启动一个长驻网关进程，承载两类端点：

| 端点 | 默认端口 | 职责 | 认证 |
|------|----------|------|------|
| **WebUI / WebSocket / API** | **8765** | WebUI 页面 + 聊天 WS（路径=ws_path 默认 `/`）+ `/api/*` 路由 + `/webui/bootstrap` | bootstrap 需 `token_issue_secret` |
| **Health** | **18790** | 仅 `GET /health` | 无需认证 |

> **关键事实：WebUI 端口 8765 与健康端口 18790 是两个独立端口，不是 health+1。** 这是 manager 集成中最容易踩的误解。

### 启动方式

```bash
# manager 主用（优先 nanobot.exe）
nanobot.exe gateway

# 回退到 Python 模块
python.exe -m nanobot gateway

# 后台模式：fork 子进程跑前台
nanobot gateway --background
# 实际子进程：
python -m nanobot gateway --foreground --port N ...
```

### Typer 参数

| 参数 | 说明 | 默认 |
|------|------|------|
| `--port` / `-p` | 健康端口 | `config.gateway.port`（= **18790**） |
| `--workspace` / `-w` | 工作区路径 | — |
| `--verbose` / `-v` | 详细日志 | off |
| `--config` / `-c` | 配置文件路径 | `~/.nanobot/config.json` |
| `--foreground` | 前台运行（后台模式内部用） | — |
| `--background` | 后台 fork | — |

### gateway 子命令

| 子命令 | 说明 | Windows 支持 |
|--------|------|--------------|
| `status` | 查询运行状态 | ✓ |
| `logs` | 查看日志 | ✓ |
| `stop` | 停止进程 | ✓ |
| `restart` | 重启 | ✓ |
| `install-service` | 安装系统服务 | ✗（仅 systemd / launchd） |

> README 另推 `nanobot webui`（= 准备 WS + 启 gateway + 自动开浏览器）。manager 不需要浏览器，故继续用 `gateway`。**未来 gateway 子命令有被 webui 合并的远期可能**（见 Risks）。

## HTTP / WebSocket API 面（manager 集成核心）

### 1. `GET /health`（端口 18790，无需认证）

gateway 前台用极简 HTTP/1.0 handler。成功返回：

```http
HTTP/1.0 200 OK
Content-Type: application/json

{"status": "ok"}
```

**manager `process.c` 的 `GET /health` + HTTP 200 判定完全正确。**

### 2. `GET /webui/bootstrap`（端口 8765，可选认证）

当 `token_issue_secret` 非空时，请求须携带以下任一头：

```
X-Nanobot-Auth: <secret>      # manager 用此格式
Authorization: Bearer <secret>
```

成功 200 返回 JSON：

| 字段 | 类型 | 说明 |
|------|------|------|
| `token` | string | WS 握手 token，前缀 `nbwt_`（格式 `nbwt_{secrets.token_urlsafe(32)}`） |
| `api_token` | string | HTTP `/api/*` 用 token，**与 `token` 不同** |
| `ws_path` | string | WS 路径，默认 `"/"` |
| `ws_url` | string | 完整 WS URL |
| `expires_in` | int | token 有效期秒数，默认 300，范围 30–86400 |
| `model_name` | string | 当前模型名 |
| `runtime_surface` | object | 运行时界面能力声明 |
| `runtime_capabilities` | object | 运行时能力声明 |

错误码：`401`（认证失败）/ `403`（localhost-only 限制被触发）/ `429`（too many tokens）。

> **manager `chat.c` 解析 `token`（回退 `api_token`）+ `ws_path` 的逻辑正确。**

### 3. 聊天 WebSocket（端口 8765，路径=ws_path）

握手 URL：

```
ws://host:8765/?token=<bootstrap_token>
```

#### 服务端 → 客户端帧（key = `"event"`）

| event | 关键字段 | 说明 |
|-------|----------|------|
| `ready` | `chat_id`, `client_id` | 连接就绪 |
| `attached` | — | 客户端附着 |
| `message` | `chat_id`, `text`, `media?`, ... | 完整消息 |
| `delta` | `chat_id`, `text`, `stream_id?` | 流式增量 |
| `stream_end` | `chat_id`, `text?`, `stream_id?` | 流结束（`text` 为媒体 URL 改写后的完整文本） |
| `reasoning_delta` | — | 思考链增量 |
| `reasoning_end` | — | 思考链结束 |
| **`turn_end`** | `chat_id`, `latency_ms?`, `goal_state?` | **一轮真正结束的权威信号** |
| `goal_state` | — | 目标状态 |
| `goal_status` | — | 目标状态详情 |
| `session_updated` | — | 会话更新 |
| `runtime_model_updated` | — | 运行时模型切换 |
| `file_edit` | — | 文件编辑事件 |
| `error` | `detail`, `reason?`, `chat_id?` | 错误 |

#### 客户端 → 服务端帧（key = `"type"`）

| type | 关键字段 | 说明 |
|------|----------|------|
| `message` | `chat_id`, `content` | 发送消息 |
| `new_chat` | — | 新建会话 |
| `attach` | — | 附件 |
| `fork_chat` | — | 分叉会话 |
| `set_workspace_scope` | — | 设置工作区作用域 |
| `transcribe_audio` | — | 语音转写 |

> 兼容旧格式：无 `type` 的纯文本 / `{"content": ...}` 也被接受。

> **manager `chat.c` 出站用 `{"type":"message","chat_id","content"}`、入站以 `event` 为主键（回退 `type`）、处理 `ready` / `delta` / `message` / `stream_end`（+ 自加 `done` 别名）/ `error` —— 正确。**

### 认证汇总

| 资源 | 认证方式 | 默认 |
|------|----------|------|
| `/health` | 无需认证 | — |
| `/webui/bootstrap` | `tokenIssueSecret` → `X-Nanobot-Auth` 头 | 空（不强制） |
| WS 握手 | bootstrap 签发的 `nbwt_` 短期 token（query `?token=`）或静态 `token` 配置 | `websocket_requires_token=true` |
| `/api/*` | `api_token`（来自 bootstrap） | — |

## 配置文件 `~/.nanobot/config.json`

> **重要更正：是 `~/.nanobot/config.json`，不是 `bot_config.yaml`**（onboard 创建）。manager 侧的 `config.json`（exe 目录旁）是独立的，仅保存 manager 自己的设置。

### 顶层结构

```
agents.defaults      # model, provider, modelPreset, workspace, bot_name, ...
channels.websocket   # 见下（GUI manager 最关心）
providers            # 40+ 提供商
api                  # port 8900
gateway              # host 127.0.0.1, port 18790, heartbeat
tools
modelPresets         # v0.2.0+ 模型预设范式
```

### `channels.websocket`（WebSocketConfig）— manager 集成关键字段

| 字段 | 默认值 | manager 对应 |
|------|--------|--------------|
| `enabled` | — | — |
| `host` | `"127.0.0.1"` | — |
| `port` | **8765** | `config.webui_port` |
| `path` | `"/"` | chat.c 用此拼 WS URL |
| `token` | `""` | 静态 WS token |
| `token_issue_path` | `""` | — |
| **`token_issue_secret`** | `""` | `config.webui_auth` |
| `token_ttl_s` | `300` | — |
| `websocket_requires_token` | `true` | — |
| `allow_from` | `["*"]` | — |
| `streaming` | `true` | — |
| `max_message_bytes` | 36 MB | — |
| `ping_interval_s` | `20` | — |
| `ping_timeout_s` | `20` | — |

> 校验规则：`host` 为 `0.0.0.0` / `::` 时必须设 `token` 或 `token_issue_secret`。

### 模型新范式（v0.2.0+）

模型配置从直接写 `model` / `provider` 迁移到 **`modelPresets.<name>` + `agents.defaults.modelPreset`**。默认模型 `anthropic/claude-opus-4-5`。

## 路线图与活跃度

v0.2.2 "Durability Release"（2026-06-21），几乎每日更新，33+ contributors。路线图方向：多模态、长期记忆（Dream）、更强推理、更多集成。

## Integration Points with nanobot-manager

| manager 侧 | 职责 | 对接的 nanobot 资源 |
|------------|------|---------------------|
| `process.c` | 启动 / 停止 | `nanobot.exe gateway` / `python -m nanobot gateway`，`CREATE_NO_WINDOW \| CREATE_NEW_PROCESS_GROUP` |
| `process.c` | 健康检查 | WinHTTP `GET /health` on **18790**，HTTP 200 = running |
| `chat.c` | 聊天连接 | `GET /webui/bootstrap`（`X-Nanobot-Auth`）→ WS 升级 → 帧解析 |
| `config.c` | 端口 / 密钥配置 | `webui_port`（8765）/ `health_check_port`（18790）/ `webui_auth` 与 nanobot 默认端口一致 |

## Risks / Future Adaptation

按风险等级分类：

### 🟢 已对齐（低风险）

- health / bootstrap / 基础帧 / 启动命令 / 默认端口 —— 全部与 v0.2.2 兼容。
- 端口默认值已是 config 驱动，manager 读自己的 `config.json` 即可。

### 🟡 中等风险（可增强，非破坏性）

| 项 | 现状 | 建议 |
|----|------|------|
| `turn_end` 帧未处理 | manager 只处理到 `stream_end` | 长任务（多轮工具调用）下仅靠 `stream_end` 可能过早解锁输入框；建议监听 `turn_end` 作 `canSend` 强信号 |
| `reasoning_delta` / `reasoning_end` 未显示 | 丢弃 | 思考链不可见，可加可折叠 reasoning 区块 |
| `file_edit` / tool events / `goal_state` 未渲染 | 丢弃 | 可加工具执行反馈区 |
| bootstrap 的 `model_name` / `api_token` 未用 | 仅取 `token` + `ws_path` | `api_token` 可调 `/api/sessions` 做历史会话管理 |
| `stream_end` 的 `text` 字段被丢弃 | 仅用 delta 累积 | 该字段是媒体 URL 改写后的完整文本，可作最终校验 / 回退 |

### 🔴 高风险（需监控）

| 项 | 影响 | 对冲策略 |
|----|------|----------|
| 端口默认值变更 | 健康 / 聊天全断 | 已 config 驱动 ✓；建议 bootstrap 返回含端口时同步 |
| `stream_end` 语义被 `turn_end` 弱化 | 完成判定不准 | 同时监听两个完成帧 |
| bootstrap 返回结构变更 | 聊天连接失败 | 关键字段缺失时加日志，容错降级 |
| WS 握手 token 传递方式从 query 改 header | 连接失败 | 关注 CHANGELOG |
| `tokenIssueSecret` vs `token_issue_secret` 字段名 | 配置不生效 | 注意 camelCase 序列化方向 |
| gateway 子命令未来被 webui 合并 | 启动命令失效 | 关注 README / CLI 变更 |

### 📌 总结

manager 集成层与 v0.2.2 **完全兼容，无紧急破坏**。适配债主要集中在**功能增强层**（turn_end / reasoning / 工具事件 / 历史会话）。结构性风险靠"config 读端口 + 容错解析 bootstrap + 监听多完成帧"三重对冲。

## Reference Index

### 上游 nanobot 关键源码（HKUDS/nanobot，main 分支）

| 文件 | 作用 |
|------|------|
| [`nanobot/cli/gateway.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/cli/gateway.py) | gateway 子命令 Typer 入口 |
| [`nanobot/gateway/runtime.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/gateway/runtime.py) | gateway 运行时 |
| [`nanobot/config/schema.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/config/schema.py) | 配置 schema（含 WebSocketConfig） |
| [`nanobot/webui/ws_http.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/webui/ws_http.py) | WebUI HTTP + WS 服务 |
| [`nanobot/channels/websocket.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/channels/websocket.py) | WebSocket channel 实现 |
| [`nanobot/webui/gateway_tokens.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/webui/gateway_tokens.py) | bootstrap token 签发（`nbwt_` 前缀） |
| [`nanobot/bus/outbound_events.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/bus/outbound_events.py) | 出站事件帧定义（event key） |
| [`nanobot/cli/commands.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/cli/commands.py) | CLI 命令集 |
| [`nanobot/api/server.py`](https://github.com/HKUDS/nanobot/blob/main/nanobot/api/server.py) | `/api/*` HTTP 服务（port 8900） |

### 本仓库 manager 侧

| 文件 | 作用 |
|------|------|
| `nanobot-manager/src/chat.c` | WS 聊天客户端（bootstrap + 连接 + 流式） |
| `nanobot-manager/src/process.c` | 进程管理 + `/health` 健康检查 |
| `nanobot-manager/src/config.c` | manager config.json（webui_port / health_check_port / webui_auth） |

### 包

- [PyPI: `nanobot-ai`](https://pypi.org/project/nanobot-ai/) — 注意包名带 `-ai` 后缀

## Related

- `docs/solutions/architecture-patterns/native-websocket-chat-integration.md` — manager 侧 WS 聊天集成的三层架构（transport / UI state / wiring）
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — `webui_port` / `webui_auth` 在 settings UI 的暴露模式
- `docs/solutions/reference/core-ui-overview.md` — companion reference：core-ui 子模块整体画像
- `CONCEPTS.md` — 共享领域词汇（实体、命名进程、状态概念）
- `AGENTS.md` — manager 运行时行为速查（启动命令、端口、config 字段）
