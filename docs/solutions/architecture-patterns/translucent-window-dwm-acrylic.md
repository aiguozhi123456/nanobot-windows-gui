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
tags: [core-ui, translucent, directcomposition, dwm-acrylic, premultiplied-alpha, noredirectionbitmap, backdrop, liquid-glass, theme-conditional-css]
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
  1. 先 `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE, 应用主题)`（属性 20；22621 前的版本同义旗标在 19），**再** `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_TRANSIENTWINDOW)`（Win11 22621+ 文档 API）；
  2. 失败 → `SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)`（Win10 1803+ 未文档化；tint 烧死了主题色，`apply_theme` 里 `backdrop_reapply_for_theme` 重涂）；
  3. 都失败或 config 关闭 → `ui_window_set_translucent(g_win, 0)` 回不透明清屏。
- 主题切换时 `backdrop_reapply_for_theme`：kind=1 重打 immersive-dark 旗标（材质雾的深浅跟着它走），kind=2 重涂 accent tint。
- 逃生开关：config `window_transparent`（默认 1），驱动/安全软件冲突时改 0。

**两个必须成对/跟随的关系：**

- **不打 DWM 背景的透窗窗口透出的是纯黑**——core-ui 的透明清屏和 DWM 亚克力必须成对出现（日志 `acrylic backdrop kind=1/2` vs `opaque fallback` 的由来）。
- **系统亚克力材质的深浅跟随 immersive-dark 旗标，不跟随应用主题**。只设 `DWMWA_SYSTEMBACKDROP_TYPE` 时材质按系统默认（多为浅色）渲染——深色主题的应用后面是一层亮白雾，深色壁纸透出来被洗成粉彩，用户看到的就是"暗色模式坏了"。`DwmSetWindowAttribute` 返回 S_OK 只说明 API 接受了，不代表视觉正确。

**色场 alpha 纪律**：透窗后"玻璃要糊的对象"是真实桌面，窗内色场（`radial-gradient` 色块）只能当色相点缀——0.3~0.45 是实测可用的区间（0.85/0.75/0.70 的首版盖满窗口后亚克力透出的画面被搅成不透明粉彩糊，用户观感"压根没透"而管线其实在工作；跌破 ~0.3 则设计感消失）。同理，玻璃卡上的输入框/字段容器不要用实色 `--input-bg`/`--bg-2`（玻璃上贴色块），TextInput 直接不写 background/border 走框架的 WinUI 状态机（烟熏填充+底线下划），div 类容器用白烟熏 alpha（0.07 底 + 0.16 描边）。

### 色场需要亮色变体（2026-10-25 亮色发暗）

用户反馈"亮色模式玻璃板偏暗"，但 kind=1 / immersive-dark=FALSE 全部正确。根因在色场本身：**同一组中间调色块（0.25~0.32 alpha 的青/紫/绿）叠在暗色亚克力上是提亮的色相点缀，叠在亮色亚克力白底上就是灰蒙蒙的污渍**（如青色块核心叠白后 ≈ #C0D6DE，局部亮度降 ~25%）——暗色越看越对、亮色越看越闷，正是这种不对称的来源。修法是同色相向白提亮 + alpha 降到 ~0.15（见 dashboard.uix `.theme-light .backdrop-*`）。

主题条件样式在 uix 里的正确接法：core-ui 匹配选择器时把静态 `class` 与 `:class` 动态类**合并**进同一个类列表，且支持祖先链（compiler.cpp 的 MatchNode 构建），所以根容器 `:class="isDark ? 'theme-dark' : 'theme-light'"` + `.theme-light .backdrop-a { … }` 后代选择器即可覆盖。两个坑：

- uix data 里的 `theme` 字段是**用户偏好**（"system"），跟随系统时它分辨不出有效深浅——必须由 C++ 推送**有效**暗色布尔（`apply_theme` 里 `ui_page_set_bool(g_page, "isDark", dark)`；apply_theme 是启动初始化与三个主题按钮的唯一漏斗，切换即时生效）。
- data 默认值给 `isDark: true`（旧观感），页面加载到 C++ 推送之间的窗口期不会闪一帧亮色。

### 底色发暗的真正层级：亚克力雾本身（2026-10-25 二轮）

色场变体上线后用户仍报"暗"，并给出关键对照：**标题条与设置页输入框（框架主题色的半透明白）比底色亮一大截**——标题条是不透明 `#ffffff`（titleBarBg = windowBg），它们和底色之间的落差说明问题在 DWM 亮色亚克力的雾底本身，不在任何窗内图层。immersive-dark 旗标全对也救不了：材质雾的浓度应用层不可调，壁纸偏暗时白雾被压成灰。

修法是**亮色下给根容器垫一层消色差白罩**（`.theme-light { background: rgba(255,255,255,0.8) }`），把底色抬向主题表面色。要点：

- 罩层必须绑在 `.theme-light`（根容器）上而非各卡片——一次抬整窗底色，卡片/侧栏的白烟熏叠在上面仍然保持相对层次。
- 消色差白不受"色场 ≤ ~0.32"纪律约束——那条纪律防的是**彩色**场把玻璃搅成不透明粉彩，白雾不与背板争色相。浓度定档 **0.6**：0.8 能把底色完全抬白但用户判"太白、失透"否决——亮度让位于透感，接受底色保留轻微灰调。教训：白罩浓度是透感/亮度的直接交换，不要单方向追求"贴近表面色"，起测从 0.6 档上下微调。
- 教训：**校准必须像素实测**（A/B 抓屏对比标题条/输入框/底色均值），本轮因环境的屏捕路径全部失效（沙箱 shell 的 GDI CopyFromScreen 全黑；CUA 窗口截图对 NOREDIRECTIONBITMAP 窗口的可见性误判拒绝）只能按惯例取值 + 用户目检标定——下次在正常环境先补测量。

## Why This Works

`WS_EX_NOREDIRECTIONBITMAP` 让 DWM 不再为窗口保留重定向表面，swapchain 内容经 `IDCompositionVisual` 直接进合成树；`DXGI_ALPHA_MODE_PREMULTIPLIED` 让每像素 alpha 语义生效；`Clear({0,0,0,0})` 留出透明像素；最后 DWM 亚克力在窗口之下绘制模糊桌面——四层叠出来"玻璃透桌面"。模糊发生在 DWM（桌面内容），`backdrop-filter`（窗内内容），两层互不替代、叠加成立。

## Verification

**目检会被骗**：窗内色场本身就是彩色渐变，深色壁纸透出来也是彩色渐变——"看起来像透"和"真的透"在肉眼层面无法区分（本项目就因此把浅色雾误判成壁纸透出放行了）。透窗验证必须用 A/B 像素对照：

1. **CopyFromScreen 拷屏**（不是 PrintWindow——NOREDIRECTIONBITMAP 窗口没有重定向表面，见 ui-bugs/core-ui-silent-rendering-traps.md 采集条目）抓窗口矩形；
2. `SetWindowPos` 把窗口平移 ~600px，等 600ms 让亚克力稳定，抓**同一屏幕区域**的纯桌面参照，再移回；
3. 分区求均值对比：真透窗的"玻璃区"窗口均值应与桌面均值强相关（本项目实测：中心区 129,79,37 vs 桌面 144,88,37；右缘 107,75,59 vs 114,82,66——只差一层薄罩）；不透时窗口区是自己的主题色，与桌面无关。
4. 放大目检用 2-3x 最近邻裁片，别看整窗缩略图。

回归清单：聊天页悬浮 composer、输入法键入、最小化→恢复（DComp 表面不破损）、深浅主题切换（immersive-dark 旗标重打 / accent tint 重涂）、设置页 ScrollView 滚动（注意：注入滚轮的 delta 要放 `mouse_event` 第 4 参 dwData，放第 3 参 dy 等于滚 0）。

日志断言：启动出现 `[app] translucent window: acrylic backdrop kind=1`。**注意 S_OK ≠ 视觉正确**——kind=1 只证明 API 接受，材质是否真的深色要靠 A/B 截图确认。

## Related Issues

- `docs/solutions/ui-bugs/core-ui-silent-rendering-traps.md` — 采集方法同源（PrintWindow vs CopyFromScreen）。
- `docs/solutions/architecture-patterns/dual-repo-core-ui-submodule-workflow.md` — core-ui 改动的提交流程（先 core-ui commit，manager bump pin）。
