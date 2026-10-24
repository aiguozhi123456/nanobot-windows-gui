---
title: "透窗（液态玻璃）模式：core-ui DComp 逐像素 alpha + manager 侧 DWM 亚克力与降级链"
date: 2026-10-24
category: architecture-patterns
module: core-ui / main.cpp window setup
problem_type: architecture_pattern
component: core
severity: info
symptoms:
  - "窗口需要透出模糊桌面（真液态玻璃），但 core-ui 主窗口默认走不透明 flip swapchain，CSS 玻璃只能糊窗内色场"
resolution_type: code_fix
tags: [core-ui, translucent, directcomposition, dwm-acrylic, premultiplied-alpha, noredirectionbitmap, backdrop, liquid-glass]
---

# 透窗模式：core-ui DComp 逐像素 alpha + DWM 亚克力

## Problem

液态玻璃 UI 需要窗口透出模糊化的桌面。core-ui 主窗口默认走 `CreateSwapChainForHwnd`（`DXGI_ALPHA_MODE_IGNORED`，不透明）+ 每帧主题色清屏——DWM 看不到任何 alpha，CSS `backdrop-filter` 只能糊窗内已绘制内容，永远糊不到桌面。框架内部其实已有一条生产级逐像素 alpha 管线（上下文菜单弹窗在用），只是没接到主窗口。

## Solution

### core-ui 侧（需要改框架；本项目已随 pin 生效）

四个接线点（全部是"接已有管线"，无新渲染代码）：

1. **UIX 属性**：`<window translucent="true"/>` → `WindowHintsFromAttrs`（page_api.cpp）填 `hints.translucent`，`page_open_or_prepare_` 合并进 `UiWindowConfig.translucent`。
2. **窗口创建**（ui_window.cpp `Create`）：translucent → `CreateWindowExW` 加 `WS_EX_NOREDIRECTIONBITMAP`（无重定向表面，DComp 内容直接合成）；同时强制 `skipOpenAnimation_`（`WS_EX_LAYERED` 的 `SetLayeredWindowAttributes` 淡入与 NOREDIRECTIONBITMAP 上的 DComp 内容不兼容，会黑闪）。
3. **渲染面**（`RegisterRenderWindow`）：`RenderThread::RegisterWindow` 传 `RenderSurfaceKind::LayeredComposition` → `RenderWindow::RenderFrame` 选 `Renderer::CreateRenderTargetForLayered`（renderer.cpp:471：`CreateSwapChainForComposition` + `DXGI_ALPHA_MODE_PREMULTIPLIED` + `IDCompositionTarget/Visual` 树——上下文菜单弹窗同款、已生产验证的路径）。
4. **透明清屏**：复用休眠的 `bgMode_` 钩子——`Create` 里 translucent 置 `SetBackgroundMode(1)`，`OnPaint`（ui_window.cpp:2299）已有的 `bgMode_!=0 → Clear({0,0,0,0})` 分支生效。

配套守卫：`Show()`/`StartWindowOpenAnimation()`/`StartWindowCloseAnimation()` 三处在 translucent 时直通（不加 `WS_EX_LAYERED`、跳过 alpha 动画，关闭直接 `DestroyWindow`）；公开 API `ui_window_set_translucent(win, 0)` 供运行期降级（恢复不透明清屏；DComp 表面保留，内容全不透明时视觉等同普通窗口）。

### manager 侧（main.cpp）

- `dashboard.uix`：`<window … translucent="true"/>`。
- `g_win` 就绪后取 HWND（`ui_window_hwnd`，ui_core.h:218）打 DWM 背景，降级链：
  1. `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_TRANSIENTWINDOW)`（Win11 22621+ 文档 API，主题自适应，切换主题无需重涂）；
  2. 失败 → `SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)`（Win10 1803+ 未文档化；tint 烧死了主题色，`apply_theme` 里 `backdrop_reapply_for_theme` 重涂）；
  3. 都失败或 config 关闭 → `ui_window_set_translucent(g_win, 0)` 回不透明清屏。
- 逃生开关：config `window_transparent`（默认 1），驱动/安全软件冲突时改 0。

**不打 DWM 背景的透窗窗口透出的是纯黑**——core-ui 的透明清屏和 DWM 亚克力必须成对出现，这也是日志里 `acrylic backdrop kind=1/2` 与 `opaque fallback` 两类信息的由来。

## Why This Works

`WS_EX_NOREDIRECTIONBITMAP` 让 DWM 不再为窗口保留重定向表面，swapchain 内容经 `IDCompositionVisual` 直接进合成树；`DXGI_ALPHA_MODE_PREMULTIPLIED` 让每像素 alpha 语义生效；`Clear({0,0,0,0})` 留出透明像素；最后 DWM 亚克力在窗口之下绘制模糊桌面——四层叠出来"玻璃透桌面"。模糊发生在 DWM（桌面内容），`backdrop-filter`（窗内内容），两层互不替代、叠加成立。

## Verification

- 壁纸可见性：截图空白区应透出模糊壁纸色彩（视觉模型确认了壁纸山峦轮廓）；用 **CopyFromScreen 拷屏**而不是 PrintWindow——NOREDIRECTIONBITMAP 窗口没有重定向表面，PrintWindow 抓不到（见 ui-bugs/core-ui-silent-rendering-traps.md 采集条目）。
- 回归清单：聊天页悬浮 composer、输入法键入、最小化→恢复（DComp 表面不破损）、深浅主题切换（accent tint 重涂）。
- 日志断言：启动出现 `[app] translucent window: acrylic backdrop kind=1`。

## Related Issues

- `docs/solutions/ui-bugs/core-ui-silent-rendering-traps.md` — 采集方法同源（PrintWindow vs CopyFromScreen）。
- `docs/solutions/architecture-patterns/dual-repo-core-ui-submodule-workflow.md` — core-ui 改动的提交流程（先 core-ui commit，manager bump pin）。
