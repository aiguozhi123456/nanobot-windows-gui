---
title: "core-ui Overview — UI 框架子模块的整体画像"
date: 2026-07-11
last_updated: 2026-10-27
category: reference
module: submodule/core-ui
problem_type: reference_overview
component: tooling
severity: info
applies_when:
  - 在 nanobot-manager 中新增 UI 页面、控件、样式或交互模式
  - 升级 core-ui 子模块 pin 或排查框架行为变化
  - 评估某项 UI 能力是否被 core-ui 支持（避免撞能力边界）
  - 利用 core-ui 未被 manager 使用的能力（自定义绘制、菜单、MsgBox、自动化测试）
tags: [core-ui, ui-framework, win32, direct2d, quickjs, uix, reactive, submodule]
---

# core-ui Overview — UI 框架子模块的整体画像

## Overview

core-ui 是一个 Windows 原生桌面 UI 框架（作为本仓库的 git submodule 存在于 `core-ui/`）。其定位是 **"Electron 开发体验 + 原生性能 + Fluent 2 外观 + Vue 响应式模型"**。当前 pin 为 fork `main` 的 **7193235** = upstream **v1.8.0 build 294**（126330e）+ 六个 manager 专有提交：chat scroll API 移植（75aaaa8）、translucent 主窗口模式（df655f5）、single-source 焦点 + tabNavigation（0d89b0f）、滚轮路径免 relayout 修复（03e6f70）、label 测量缓存 / DWrite wrap 高度（11d7bd4）、绝对控件 `:left`/`:top` move-only 快路径（7193235）。1.7.0 把渲染提交迁移到独立渲染线程；v1.8.0 增收了 chips-input / OCR 划词组件 / HTML5 拖放 / widget 级 contextmenu 与 submit 回调 / TextArea 选区 / SVG 引擎与长列表性能一揽子工作。

它在 nanobot-manager 中的角色：提供 `dashboard.uix`（SFC）的加载、响应式状态绑定、原生控件渲染、i18n、主题切换等全部 UI 能力。manager 通过动态链接 `core-ui.dll` + 外部 `.uix` 文件路径消费它（非单 exe 内嵌）。本文档为"更好地利用 core-ui"提供完整的能力地图与边界认知。

## 技术栈

| 层 | 技术 | 说明 |
|----|------|------|
| 渲染 | **Direct2D + Direct3D 11** 硬件加速 | 非 GDI、非 WebView；Per-Monitor DPI V2 内建；Win10 1709+ |
| 脚本引擎 | **QuickJS-NG v0.14.0**（ES2020+） | 求值 `.uix` 的 `<script>`；**无 DOM / 无 document / 无 fetch / 无 npm** |
| SVG | D2D `ID2D1SvgDocument` 主路径 + LunaSVG/plutovg（vendored `third_party/`）回退 | — |
| 语言 | C11 + C++17 | 仅 MSVC / clang-cl（**不支持 MinGW**），`/utf-8` |
| 构建 | CMake 3.20+ | 产物：单 DLL（4.1 MB）或静态库（~2.2 MB） |
| 性能 | 启动 < 200 ms，内存 < 30 MB | — |

### 核心理念

- 纯 C ABI：`uint64_t` 句柄，`ui_<noun>_<verb>` 命名约定。
- `.uix` 即 Vue3 SFC（LLM 友好）：CSS + Flexbox 布局。
- 可自我验证：60+ `ui_debug_*` 事件 + 命名管道 IPC。

## UIX 标记语言（.uix = Vue3 SFC）

### 顶层结构（四块，顺序任意）

| 块 | 说明 |
|----|------|
| `<window>` | 窗口属性 |
| `<link>` | 资源链接 |
| `<script>` | Options API（须 `export default {}`） |
| `<style>` | CSS 子集 |
| `<template>` | **必须恰好一个根元素，所有标签显式闭合** |

### SFC 工作流

```
ui_page_load_file
  → SFC 解析器切四块
  → QuickJS module-eval <script>（须 export default {}）
  → 模板解析器 + 表达式重写器（自动加 this.）
  → 编译成 widget 树 + 响应式 binding
  → CSS 解析
```

### 关键语法

| 语法 | 说明 |
|------|------|
| `data()` | 函数返回对象（**仅 Options API**） |
| `{{ expr }}` | 文本插值（不写 `this.`） |
| `:attr` | 属性绑定 |
| `@event` | 事件绑定（`click` / `mousedown` / `mousemove` / ... / `submit` / `focus` / `blur` / `change` / `input`） |
| `$t('key')` 或 `@key` | i18n 语法糖 |
| `$locale` | 当前 locale |
| `v-for` | 须配 `:key` |
| `v-if` / `v-show` | 条件渲染 |
| `v-model` | checkbox / select 取索引数字 |

### 约束（重要边界）

- **仅 Options API**：无 `script setup` / `ref` / `watch` / lifecycle hooks。
- **无 props / emit / slot / import 子组件系统**（1.5.0 已删）。
- **数组原地 mutation 不触发响应式**（须整体替换 `this.items = [...this.items, x]`）。

### 内置 widget 标签映射

| 标签 | 映射 |
|------|------|
| `div` / `section` / `nav` | `VBox` / `HBox`（看 `flex-direction`） |
| `label` / `span` / `p` / `h1`–`h6` | `LabelWidget` |
| `button` | `ButtonWidget` |
| `input` | text / password / checkbox / radio / range / number |
| `textarea` / `select` / `toggle` / `progressbar` | 对应控件 |
| `ScrollView` / `TitleBar` / `Expander` / `tabs` | 容器控件 |
| `img` / `svg`（+ `path` / `circle` / `rect`） | 图像 / 矢量 |
| `custom` | `CustomWidget` |
| `menu` / `menuitem` / `separator` | `ContextMenu` |

### CSS 子集

**支持**：盒模型 / flex（**不支持 grid / float**）/ `box-shadow` / `backdrop-filter` / `transform` / `transition` / `opacity` / `var(--*)` / `calc()` / 伪类（`:hover` / `:active` / `:focus` / `:disabled` / `:checked`）/ 选择器（tag / `.class` / `#id` / 后代 / 子代 / 属性 / 复合）。

**不支持**：`+` / `~` 兄弟选择器、`:nth-*`、`::before` / `::after`、`:not()`、`*`、`@media`、`@keyframes`、`!important`、CSS 动画。

## C API 面（来自 `include/ui_core.h`，400+ 函数）

所有句柄为 `uint64_t`：`UiWidget` / `UiWindow` / `UiMenu` / `UiPage` / `UiLayout`，`UI_INVALID = 0`。

### 生命周期 / 消息循环

```c
ui_init(int argc, char** argv);
ui_init_with_theme(int argc, char** argv, int theme);  /* UI_THEME_DARK=0, UI_THEME_LIGHT=1 */
ui_shutdown(void);
ui_run(void);     /* 阻塞消息循环 */
ui_quit(void);
```

### 页面 / 挂载

```c
ui_page_load_file(const char* path);
ui_page_load_string(const char* utf8);
ui_page_destroy(UiPage page);
ui_page_reload(UiPage page);   /* 重解析，但按名保留响应式变量值 */
ui_page_root(void);
ui_page_open_window(...);
ui_page_prepare_window(...);   /* 隐藏窗预创建，首帧零闪烁（正常启动与 autostart 均用此） */
ui_page_last_error(void);
```

### 响应式状态写入（触发模板重求值）

```c
ui_page_set_bool(UiPage page, const char* field, int val);
ui_page_set_int(UiPage page, const char* field, int val);
ui_page_set_float(UiPage page, const char* field, double val);
ui_page_set_text(UiPage page, const char* field, const wchar_t* val);  /* 支持点号路径 "user.name" */
ui_page_set_text_list(...);
ui_page_set_json(UiPage page, const char* field, const char* utf8_json); /* 推任意嵌套 */

char* ui_page_get_json(UiPage page, const char* field);  /* 反向读，malloc 的 UTF-8 JSON，caller 须 ui_page_free */
void ui_page_free(void* p);
```

### 生命周期回调（v-if / v-for 子树安全挂回调）

```c
ui_page_on_widget_mount(UiPage page, const char* widget_id,
                        UiWidgetMountCallback cb, void* userdata);
ui_page_on_widget_unmount(UiPage page, const char* widget_id,
                          UiWidgetUnmountCallback cb, void* userdata);
```

> 每次 `v-if` remount / `v-for` 重建都会重新 fire，传**新的** `UiWidget` 实例。这是 v-if 子树挂 `ui_widget_on_click` 的**官方姿势**（manager 的 27 个 `ui_page_on_widget_mount` 注册均用此，main.cpp:2353-2379、2419）。

### Widget 通用 / 树

```c
ui_widget_set_id / width / height / size / visible / enabled / opacity / bg_color / tooltip / rect;
ui_widget_find_by_id(UiPage page, const char* id);
ui_widget_add_child / remove_child;
ui_widget_get_rect / get_basic / get_text;   /* get_text 返回内部指针，不要 free */
```

### 事件

```c
ui_widget_on_click(UiWidget w, UiClickCallback cb, void* ud);
ui_widget_on_mouse_move / on_mouse_leave / on_wheel / on_focus / on_blur;
ui_checkbox_on_changed(UiWidget w, UiCheckboxCallback cb, void* ud);
ui_slider_on_changed / ui_toggle_on_changed / ui_combobox_on_changed;
```

> **注意：无 `ui_text_input_on_changed`**（仅 checkbox/slider/toggle/combobox 有 change 回调）。文本 / 数字 input 须用 Apply 按钮读回（见 Related 文档）。

### 窗口回调

`on_close` / `on_close_request`（可取消）/ `on_resize` / `on_drop` / `on_key` / `on_right_click` / `on_menu` / `on_titlebar_drag`。

### i18n

```c
ui_page_load_language_string(const char* locale, const char* utf8_content);  /* manager 用此传入 i18n.c 组装的语言表 */
ui_page_load_language_file(const char* locale, const char* path);
ui_page_load_translations(...);
ui_page_set_locale(const char* locale);   /* 切换 → $t() 自动重 fire */
```

> **Replace-not-merge**：同 locale 二次 load 替换整表；missing key fallback 返回 key 本身。

### 主题

```c
ui_theme_set_mode(int mode);                /* 0=dark, 1=light */
ui_theme_get_mode(void);
ui_theme_bg / content_bg / accent / text / ...;
ui_theme_set_accent_hex(const char* hex);   /* 如 "#2563EB" */
ui_theme_set_default_font(...);
ui_theme_set_cjk_font(const char* latin, const char* cjk);  /* 中英分离 */
ui_theme_set_text_render_mode(...);
```

> `.uix` 中用 CSS 变量（`--bg` / `--fg` / `--accent` / `--card-bg` / ...）。

### 其他模块（简述）

| 模块 | 说明 |
|------|------|
| 绘制 API | `ui_draw_*`，须在 `UiCustomDrawCallback` 内调用 |
| `CustomWidget` | `ui_custom` + `on_draw` / `mouse_*` / `key` |
| `ImageView` 家族 | `image_view` / `image_view_plus` / `gh_img_view`（瓦块金字塔，适合大图 / 图表） |
| 无边框画布模式 | 自定义标题栏 |
| 资源解析器 | `ui_asset_register_dir` / `register_blob` / `register_resolver` |
| Toast / MsgBox | `ui_toast` / `ui_msgbox`（模态确认框） |
| Menu | 声明式 + 磨砂材质 |
| trace sink | 日志钩子 |
| Debug / 自动化 | 60+ `ui_debug_*` 事件 + 命名管道 IPC `ui_debug_server_start` |

### 注意事项

- 所有 API 须在**主线程**调用。
- `get_text` 返回内部指针，**不要 free**。
- 几何单位是 **DIP**（1/96 inch，同 CSS px）。

## 响应式系统

`data()` 返回的对象被 QuickJS 用 **Proxy + WatchEffect** 包装成响应式。模板编译时，`{{}}` / `:attr` / `v-if` / `v-for` / `v-model` / `@click` 转成"求值时自动收集依赖"的 binding。

```
C 端 ui_page_set_int("count", 5)
  → 写入 JS state
  → Proxy setter
  → 标记依赖该字段的 WatchEffect dirty
  → 重求值 binding
  → ApplyBindingToWidget → ui_widget_set_*
```

### 要点

| 行为 | 是否触发响应式 |
|------|----------------|
| `this.count = 5`（顶层赋值） | ✓ 触发 |
| `this.items.push(x)`（嵌套原地 mutation） | ✗ **不触发** |
| `this.items = [...this.items, x]`（整体替换） | ✓ 触发 |
| `computed`（lazy memo，可链式） | ✓ 按需 |
| 切 `locale` 改 `$locale` | ✓ 触发所有 `$t()` 重 fire |
| `ui_page_reload` | 重解析模板，但**按名保留**响应式变量值 |

## 构建集成

### CMake target

| target | 条件 |
|--------|------|
| `core-ui`（SHARED，默认） | 产出 `core-ui.dll`（4.1 MB） |
| `core-ui-static` | `UI_CORE_STATIC=ON`（~2.2 MB） |

### 依赖

- **FetchContent** 拉 QuickJS-NG v0.14.0。
- LunaSVG + plutovg vendored 折进 dll（`third_party/`）。
- 系统库：`d2d1 d3d11 dxgi dxguid dwrite dwmapi uxtheme windowscodecssh shlwapi gdiplus`。

### CMake 选项

| 选项 | 说明 |
|------|------|
| `UI_CORE_STATIC` | 编静态库 |
| `UI_CORE_BUILD_STATIC_ARCHIVE` | DLL 模式额外编自包含 `core-ui-static.lib`（含 QuickJS），发布到 `lib/static/` |
| `UI_CORE_MSVC_STATIC_CRT` | `/MT` 单 dll 零依赖 |

### 辅助宏（`UiCoreHelpers.cmake`）

```cmake
ui_core_embed_text(FILE <path> OUT <var>)     # 烤文本资源成单 exe
ui_core_embed_binary(FILE <path> OUT <var>)   # 烤二进制资源
```

### nanobot-manager 消费方式

```cmake
# nanobot-manager/CMakeLists.txt
set(UI_CORE_BUILD_STATIC_ARCHIVE OFF)              # 加速 dev（跳过胖静态 archive）
add_subdirectory(../core-ui)                       # 拉子模块
target_link_libraries(nanobot-manager PRIVATE core-ui)
# POST_BUILD 拷贝 ui/ 目录和 core-ui.dll 到 exe 旁
```

运行时从 exe 同级 `ui\dashboard.uix` 加载（**DLL 动态链接 + 外部 `.uix` 路径，非单 exe embed**）。缺失 `ui\dashboard.uix` → load failure MessageBox。

## 能力边界

### 能

Fluent2 级原生 UI（25+ 控件）、Vue3 风格声明式响应式、CSS 子集 + Flexbox + CSS 变量主题、内建 i18n、深浅主题一行切换、SVG 原生矢量、大图金字塔查看、无边框画布 + 自定义标题栏、声明式菜单、模态 MsgBox / Toast、60+ debug 事件自动化、单 exe 打包、Per-Monitor DPI V2。

### 不能（重要边界）

| 类别 | 不支持项 |
|------|----------|
| Web API | 无 DOM / WebView / `window` / `document` / `fetch` / `XHR` / `npm` |
| 文本流 | 无 inline 文本流（每个 `label` / `span` 是独立 widget 矩形；嵌套 inline 报错；横排用 `flex-row`） |
| CSS | 无 grid / float / `@keyframes` / CSS 动画 / `!important` / `@media` / `::before` / `::after` |
| 组件 | 无 `script setup` / Composition API（仅 Options API）；无 lifecycle hooks；无 props / emit / slot / import 子组件 |
| 响应式 | 无嵌套对象 / 数组原地 mutation 响应式 |
| 滚动 | 无横向滚动（`ScrollView` 只竖向） |
| 定位 | 无 `position: sticky` |
| 平台 / 编译器 | 仅 Win10 1709+，仅 MSVC / clang-cl |

### 与 Web 的本质区别

`.uix` 模板**像 HTML 但编译成原生 D2D widget，不是 DOM 节点**。无事件冒泡 / 捕获、无 CSSOM、无真实 `canvas` / `video` / `audio` / `iframe`。优势：原生性能 + 4.1 MB + 无 v8 / Chromium 巨型依赖。

## Integration Points with nanobot-manager

### 当前使用的能力

| 能力 | manager 用法 |
|------|--------------|
| 生命周期 | `ui_init_with_theme` / `ui_run` / `ui_page_load_file` |
| 首帧零闪烁 | `ui_page_prepare_window`（正常启动与 autostart 统一走此，main.cpp:2385；`ui_page_open_window` 已不再被 manager 调用） |
| 响应式 C→JS | `set_bool` / `set_text` / `set_json` 大量使用（status / uptime / pid / canStart / messages / canSend / draft） |
| 响应式 JS→C | `get_json` + `free`（如读取 draft / healthPort / webuiPort——空串即 0=auto 哨兵；secret 不经此路径，在 Credential Manager） |
| 生命周期回调 | 27 个 `ui_page_on_widget_mount` 挂 native click / submit（`btn_start` / `btn_stop` / `toggle_*` / `btn_change_path` / `btn_webui` / `btn_apply_network` / `*_port_input` / `btn_send` / `chat_input` / `theme_*` / `lang_*` / `log_*`） |
| 主题 | `set_mode` / `set_cjk_font`（Segoe UI + Microsoft YaHei UI）/ `set_accent_hex("#4a7a8c")` |
| i18n | `load_language_string` 接收 `src/i18n.c` 组装的 zh / en 语言表（**非外部 `.lang` 文件**）；`set_locale` |
| 键盘焦点 / Tab 导航 | `tabNavigation="true"`（dashboard.uix:1）；`ui_window_focus_widget`（main.cpp:276、1773，chat_input 挂载即聚焦） |
| 窗口级按键 | `ui_window_on_key`（main.cpp:2421，Esc 还原设置页端口编辑） |
| 透窗模式 | `<window translucent="true"/>` + `ui_window_set_translucent`（main.cpp:2404，无 DWM 亚克力时降级不透明）；DWM 背景由 manager 侧涂（见 `docs/solutions/architecture-patterns/translucent-window-dwm-acrylic.md`） |

### 未利用的能力（潜在扩展点）

| 能力 | 潜在用途 |
|------|----------|
| 声明式 `menu` / `ui_widget_on_context_menu`（build 287） | 右键菜单 / submenu；逐行挂回调可绕开 `trigger` selector 的 v-if 时序坑 |
| `ui_msgbox` | 模态确认框（如"确认停止 nanobot？"） |
| `ui_toast` | 通知（如健康状态变化） |
| `custom` 自绘 widget + `ui_draw_*` | 自定义可视化（如图表） |
| `image_view_plus` / `gh_img_view`（v1.8.0 加 checkerboard / shadow / fit-padding / DPI scale） | 大图 / 图表显示 |
| `chips_input`（v1.8.0） | 芯片式输入：模板占位 + 已选芯片，适合模型/端口多选或过滤条件 |
| `ocr_img_view` + `ocr_text_model`（v1.8.0） | 图片 + OCR 文本叠加划词选择、缩放/平移/旋转，-manager 暂无场景 |
| HTML5 语义拖放（v1.8.0：`set_draggable` / `set_drop_target` / `on_drop` + `.uix` 的 `draggable`/`drop-target`/`@drop`） | 聊天输入框拖文件进来等 |
| `ui_widget_on_submit`（build 287，TextInput/TextArea Enter） | 已用于 chat_input 替代 fork 版 `ui_text_input_on_submit` |
| `ui_text_area_set/get_selection` + `on_selection_changed`（v1.8.0） | 聊天记录选中文本复制 / 代码块选择 |
| `ui_asset_invalidate`（v1.8.0） | 图标热更新（无需重启换资源） |
| `ui_debug_*` 自动化 / IPC | UI 集成测试（本仓库已加 CTest 单测，未来可用 core-ui 的 debug 注入） |
| 单 exe 打包（`embed_text`） | 烤资源进 exe（目前用外部 `.uix` + 拷贝 dll） |

## Risks / Future Adaptation

| 风险点 | 说明 |
|--------|------|
| 子模块 pin 独立 | core-ui 的 pin 与 manager 版本独立，可独立升级；升级时关注 `CHANGELOG.md` 的 API 签名变化（400+ 函数，命名稳定 `ui_<noun>_<verb>`）。fork `main` = upstream v1.8.0 build 294 + 六个 manager 专有提交（见 Overview）；旧 lineage 保留在 fork 的 `legacy-v1.7-fork` 分支 |
| QuickJS 仅 Options API | 限制了组件复用：多页面须复制模板，或 C 端组合多 page |
| "必须整体替换数组"约束 | 易踩；manager chat 消息用 `set_json` 整体推（正确） |
| 1.7.0 渲染线程迁移 | 是干净基线，未来 overlay / submenu 服务可能在此上演进 |
| 无 `ui_text_input_on_changed` | 文本 / 数字 input 须用 Apply 按钮读回（已是 manager 的既定模式）。v1.8.0 起可用 `ui_widget_on_submit`（build 287）做 Enter 提交，但变更回调仍缺 |
| 无 `type="password"` 渲染 | input factory 落到普通 `TextInput`，无掩码（框架限制仍在）。manager 的明文示例已移除：secret 走 Credential Manager + 原生输入对话框（`ES_PASSWORD`），不经 uix 渲染 |
| 自绘 TextInput 的 IME 候选位置 | **已于 v1.8.0 在框架内修复**（`UpdateImeCompositionPos`）；历史分析见 `docs/solutions/ui-bugs/ime-candidate-window-top-left-custom-textinput.md` |

## Reference Index

### core-ui 本地文档（子模块内）

| 文件 | 作用 |
|------|------|
| `core-ui/README.md` | 项目说明 |
| `core-ui/README-zh.md` | 中文说明 |
| `core-ui/UI_CORE_API.md` | API 概览 |
| `core-ui/include/ui_core.h` | **权威 C API 头**（400+ 函数） |
| `core-ui/docs/uix-ai-guide.md` | **AI 导向的 UIX 指南**（改 UI 时的首选参考） |
| `core-ui/docs/uix-guide.md` | UIX 完整指南 |
| `core-ui/docs/c-api.md` | C API 文档 |
| `core-ui/docs/markup.md` | 标签参考 |
| `core-ui/docs/controls.md` | 控件参考 |
| `core-ui/docs/layout.md` | 布局参考 |
| `core-ui/docs/i18n.md` | 国际化参考 |
| `core-ui/docs/design-system.md` | 设计系统 / 主题 |
| `core-ui/docs/debug-simulation.md` | Debug / 自动化 |
| `core-ui/CMakeLists.txt` | 构建配置 |
| `core-ui/CHANGELOG.md` | 变更日志（升级时必读） |
| `core-ui/VERSION` | 版本信息（纯文本文件，内容 `1.8.0.294`；不存在 version.json） |

### 本仓库 manager 侧

| 文件 | 作用 |
|------|------|
| `nanobot-manager/src/main.cpp` | UI 挂载回调、tray、主题 / locale、健康定时器、chat UI 接线 |
| `nanobot-manager/src/i18n.c` | zh / en 语言表字面量 + status 单一来源表（组装后喂 `load_language_string`） |
| `nanobot-manager/src/chat_ui.cpp`（+ `chat_ui.hpp`，`ChatUi` 单例） | 聊天 UI 状态层（`ui_page_get_json` / `set_json` 模式范例） |
| `nanobot-manager/CMakeLists.txt` | core-ui 消费方式（`add_subdirectory` + link + POST_BUILD 拷贝） |
| `nanobot-manager/ui/dashboard.uix` | 核心 UI SFC（布局 + 响应式状态 + 聊天页） |

## Related

- `docs/solutions/reference/nanobot-gateway-overview.md` — companion reference：nanobot 上游网关整体画像
- `docs/solutions/architecture-patterns/dual-repo-core-ui-submodule-workflow.md` — 父仓 pin / 子模块 remote / 双仓落地顺序（与能力画像互补）
- `docs/solutions/architecture-patterns/native-websocket-chat-integration.md` — chat 集成的三层架构（用到 `v-for` / `v-model` / `ScrollView`）
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — `ui_page_get_json` 读回模式（Apply 按钮）与 `ui_text_input_on_changed` 缺失的影响
- `AGENTS.md` — core-ui API 使用约定（"prefer `core-ui/docs/uix-ai-guide.md` 及 `core-ui/include/ui_core.h`"）
