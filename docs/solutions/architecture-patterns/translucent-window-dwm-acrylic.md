---
title: "透窗（液态玻璃）模式：core-ui DComp 逐像素 alpha + manager 侧 DWM 亚克力与降级链"
date: 2026-10-25
category: architecture-patterns
module: core-ui / main.cpp window setup
problem_type: architecture_pattern
component: tooling
severity: medium
root_cause: async_timing
symptoms:
  - "窗口需要透出模糊桌面（真液态玻璃），但 core-ui 主窗口默认走不透明 flip swapchain，CSS 玻璃只能糊窗内色场"
  - "启动后透明效果在不手动切主题时无效（100% 复现）：平灰底+色场，切主题后壁纸才穿透——DWM backdrop 采样死"
  - "仅显示后首次设置 backdrop 的变体：冷启动首启全黑"
resolution_type: code_fix
tags: [core-ui, translucent, directcomposition, dwm-acrylic, premultiplied-alpha, noredirectionbitmap, liquid-glass, theme-conditional-css]
---

# 透窗模式：core-ui DComp 逐像素 alpha + DWM 亚克力

> **快速定位（2026-10-25 三轮：启动透窗采样死，当前最终形态）**——若症状是"透明效果不手动切一下主题就无效 / 冷启动首启全黑（平灰底+色场，切主题后壁纸才穿透）"，修法三段式：`ui_page_prepare_window` 隐藏创建 → 显示前 `apply_window_backdrop`（首帧有雾防黑）→ 显示后 `TIMER_BACKDROP_KICK` 250/800ms 两次翻转 immersive-dark 踢活桌面采样，`on_window_shown()` 覆盖一切隐藏→显示循环（含任务栏还原）。两层根因、实测矩阵与否定方案见下文 Solution 末节《启动透窗"无效"（三轮，最终版）》；抓屏测量必须零接触 + 壁纸判别（动窗=踢，白背景掩盖死活）。

## Problem

液态玻璃 UI 需要窗口透出模糊化的桌面。core-ui 主窗口默认走 `CreateSwapChainForHwnd`（`DXGI_ALPHA_MODE_IGNORED`，不透明）+ 每帧主题色清屏——DWM 看不到任何 alpha，CSS `backdrop-filter` 只能糊窗内已绘制内容，永远糊不到桌面。框架内部其实已有一条生产级逐像素 alpha 管线（上下文菜单弹窗在用），只是没接到主窗口。

## Symptoms

- CSS `backdrop-filter` 只糊窗内内容，桌面永远不进玻璃（初版管线问题 → Solution core-ui 侧）。
- 深色主题下玻璃后是亮白雾、壁纸被洗成粉彩——"暗色模式坏了"（immersive-dark 旗标缺失 → Solution 两个必须成对/跟随的关系）。
- 亮色模式玻璃板偏暗发闷，旗标全对（二轮 → Solution 亮色变体 / 亚克力雾底两节）。
- **启动后透明效果"不手动切一下主题就无效"（100% 复现）：平灰底 + 色场，切主题后壁纸模糊穿透——DWM backdrop 采样死**（三轮 → Solution 三轮节 / What Didn't Work）。
- 仅"显示后才首次设置 backdrop"的变体：热启动正常，冷启动首启全黑（三轮否定方案）。

## What Didn't Work

- **显示后首次设置 backdrop（无踢）**：热启动采样活（201），但冷启动首启全黑——不可单独依赖（三轮节"否定掉的替代方案"）。
- **隐藏期设 + 显示后同值重设**：死（228.6）——同值重设对 DWM 是 no-op。
- **SWP_FRAMECHANGED**：无效。
- **抓屏前移动/置顶窗口再截图**：任何窗口操作 = 一次踢，把死采样踢活——早期"零复现"结论全部被采集流程污染；透窗抓屏必须零接触。
- **白背景做判别背景**：雾≈白，死活数值差 ±5——判别采样只能用壁纸。
- **色场 α 0.85/0.75/0.70 首版**：亚克力画面被搅成不透明粉彩糊（后收敛 0.3~0.45）。
- **亮色白膜 0.8**："太白、失透"被否决（0.6 → 采样修复后 0.45）。
- **MinimizeAll + 外部 SW_RESTORE 测试循环**：在 DComp 窗口上打出内容+材质全无的幽灵态——测试伪影，勿当产品 bug。

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
- `g_win` 就绪后取 HWND（`ui_window_hwnd`，ui_core.h:218）打 DWM 背景——**此时窗口必须仍未显示**（启动路径统一 `ui_page_prepare_window`，首次显示在 backdrop 之后，见下文"启动透窗无效"节），降级链：
  1. 先 `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE, 应用主题)`（属性 20；22621 前的版本同义旗标在 19），**再** `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_TRANSIENTWINDOW)`（Win11 22621+ 文档 API）；
  2. 失败 → `SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)`（Win10 1803+ 未文档化；tint 烧死了主题色，`apply_theme` 里 `backdrop_reapply_for_theme` 重涂）；
  3. 都失败或 config 关闭 → `ui_window_set_translucent(g_win, 0)` 回不透明清屏。
- 主题切换时 `backdrop_reapply_for_theme`：kind=1 重打 immersive-dark 旗标（材质雾的深浅跟着它走），kind=2 重涂 accent tint。
- 逃生开关：config `window_transparent`（默认 1），驱动/安全软件冲突时改 0。

**两个必须成对/跟随的关系：**

- **不打 DWM 背景的透窗窗口透出的是纯黑**——core-ui 的透明清屏和 DWM 亚克力必须成对出现（日志 `acrylic backdrop kind=1/2` vs `opaque fallback` 的由来）。
- **系统亚克力材质的深浅跟随 immersive-dark 旗标，不跟随应用主题**。只设 `DWMWA_SYSTEMBACKDROP_TYPE` 时材质按系统默认（多为浅色）渲染——深色主题的应用后面是一层亮白雾，深色壁纸透出来被洗成粉彩，用户看到的就是"暗色模式坏了"。`DwmSetWindowAttribute` 返回 S_OK 只说明 API 接受了，不代表视觉正确。

**色场 alpha 纪律**：透窗后"玻璃要糊的对象"是真实桌面，窗内色场（`radial-gradient` 色块）只能当色相点缀——0.3~0.45 是实测可用的区间（0.85/0.75/0.70 的首版盖满窗口后亚克力透出的画面被搅成不透明粉彩糊，用户观感"压根没透"而管线其实在工作；跌破 ~0.3 则设计感消失——此区间在暗色亚克力上实测；亮色白底自亮、下限不同（亮色变体用 ~0.15，见下文））。同理，玻璃卡上的输入框/字段容器不要用实色 `--input-bg`/`--bg-2`（玻璃上贴色块），TextInput 直接不写 background/border 走框架的 WinUI 状态机（烟熏填充+底线下划），div 类容器用白烟熏 alpha（0.07 底 + 0.16 描边）。

### 色场需要亮色变体（2026-10-25 亮色发暗）

用户反馈"亮色模式玻璃板偏暗"，但 kind=1 / immersive-dark=FALSE 全部正确。根因在色场本身：**同一组中间调色块（0.25~0.32 alpha 的青/紫/绿）叠在暗色亚克力上是提亮的色相点缀，叠在亮色亚克力白底上就是灰蒙蒙的污渍**（如青色块核心叠白后 ≈ #C0D6DE，局部亮度降 ~25%）——暗色越看越对、亮色越看越闷，正是这种不对称的来源。修法是同色相向白提亮 + alpha 降到 ~0.15（见 dashboard.uix `.theme-light .backdrop-*`）。

主题条件样式在 uix 里的正确接法：core-ui 匹配选择器时把静态 `class` 与 `:class` 动态类**合并**进同一个类列表，且支持祖先链（compiler.cpp 的 MatchNode 构建），所以根容器 `:class="isDark ? 'theme-dark' : 'theme-light'"` + `.theme-light .backdrop-a { … }` 后代选择器即可覆盖。两个坑：

- uix data 里的 `theme` 字段是**用户偏好**（"system"），跟随系统时它分辨不出有效深浅——必须由 C++ 推送**有效**暗色布尔（`apply_theme` 里 `ui_page_set_bool(g_page, "isDark", dark)`；apply_theme 是启动初始化与三个主题按钮的唯一漏斗，切换即时生效）。
- data 默认值给 `isDark: true`（旧观感），页面加载到 C++ 推送之间的窗口期不会闪一帧亮色。

### 底色发暗的真正层级：亚克力雾本身（2026-10-25 二轮）

色场变体上线后用户仍报"暗"，并给出关键对照：**标题条与设置页输入框（框架主题色的半透明白）比底色亮一大截**——标题条是不透明 `#ffffff`（titleBarBg = windowBg），它们和底色之间的落差说明问题在 DWM 亮色亚克力的雾底本身，不在任何窗内图层。immersive-dark 旗标全对也救不了：材质雾的浓度应用层不可调，壁纸偏暗时白雾被压成灰。

修法是**亮色下给根容器垫一层消色差白罩**（`.theme-light { background: rgba(255,255,255,0.8) }`），把底色抬向主题表面色。要点：

- 罩层必须绑在 `.theme-light`（根容器）上而非各卡片——一次抬整窗底色，卡片/侧栏的白烟熏叠在上面仍然保持相对层次。
- 消色差白不受"色场 ≤ ~0.32"纪律约束——那条纪律防的是**彩色**场把玻璃搅成不透明粉彩，白雾不与背板争色相。浓度定档 **0.6**（次日晚随采样修复降至 0.45，见下文三轮节）：0.8 能把底色完全抬白但用户判"太白、失透"否决——亮度让位于透感，接受底色保留轻微灰调。教训：白罩浓度是透感/亮度的直接交换，不要单方向追求"贴近表面色"，起测从 0.6 档上下微调。
- 教训：**校准必须像素实测**（A/B 抓屏对比标题条/输入框/底色均值），本轮因环境的屏捕路径全部失效（沙箱 shell 的 GDI CopyFromScreen 全黑；CUA 窗口截图对 NOREDIRECTIONBITMAP 窗口的可见性误判拒绝）只能按惯例取值 + 用户目检标定——下次在正常环境先补测量。

### 启动透窗"无效"：两层根因 + 显示前设/翻转踢（2026-10-25 三轮，最终版）

症状：用户报告"透明效果在不手动切一下主题时无效"（100% 复现）。用户两张截图定了案：启动态是**平灰底 + 色场**（材质在但采样死了——不是纯黑），切主题后壁纸的暖橙/青绿色块**模糊穿透**玻璃（采样活了）。

**两层根因：**

1. **材质存在性**：backdrop 属性在显示前一次性设置（旧序甚至晚于显示），材质会合成但——
2. **采样激活**（主因）：DWM 对 NRDB 窗口的系统 backdrop 材质，**只在"属性设置落在已显示的窗口上"时才接线桌面采样**。实测矩阵（零接触抓屏 + 壁纸判别）：
   - 属性只存在于隐藏期 → 材质在，采样死（平雾，黑/白半屏差 ≤1.4，壁纸 144 vs 窗口下 234）；
   - 隐藏期设 + 显示后**同值重设** → 仍死（**同值重设对 DWM 是 no-op**，228.6）；
   - 显示后**首次设置**（此前从未设过，热启动）→ 活（201，壁纸穿透）——但冷启动首启全黑，见下方否定方案；
   - 隐藏期设 + 显示后 attr 20 翻转对（踢）→ 活（201）——踢有效但多余。

**最终修法（两层，缺一不可）**：

1. **显示前设置**（材质存在性 + 防黑）：`ui_page_prepare_window` 创建隐藏窗口后、显示前 `apply_window_backdrop`——首帧即有雾，冷启动（AV 扫描/D2D 冷建拖慢首帧）也绝不黑屏。只设不激活采样。
2. **显示后翻转踢**（采样激活）：`TIMER_BACKDROP_KICK` 在显示后 250ms 与 800ms 两次翻转 immersive-dark（`!dark`→`dark`，值变化才触发 DWM 重评估）。调度点：正常路径 show 后 + `on_window_shown()`（覆盖托盘/热键唤出、任务栏还原等一切隐藏→显示循环；`g_decor_paused` 早退保证只在真循环时触发）。坏态下翻转不可见（两种变体都没采样），活态下最多 1 帧色偏。

**否定掉的替代方案（实测矩阵，勿再走回头路）**：显示后首次设置（无踢）——热启动活（201）但**冷启动首启全黑**（用户实测：首次安装启动黑/不透明，之后正常）；隐藏设+显示后同值重涂——死（228.6，同值重设对 DWM 是 no-op）；SWP_FRAMECHANGED——无效。

**测量教训（本轮最大弯路）：**

- **抓屏前的任何窗口操作（置顶/移动）= 一次踢**，会把死采样踢活——早期"零复现"结论全部被自己的采集流程污染。透窗抓屏必须零接触：只 GetWindowRect（纯查询）+ CopyFromScreen（纯读屏）。
- **白色背景会掩盖采样死活**（雾≈白，死活数值差 ±5）——判别采样只能用桌面壁纸（雾被拉向壁纸色 150→201，死雾恒定 228-242）。用户指路"退到桌面去测"是关键转折。
- **MinimizeAll + 外部 SW_RESTORE 的测试循环本身会在 DComp 窗口上打出幽灵态**（内容+材质全无的完全穿透）——那是测试伪影，不要当产品 bug 追。
- 用户实测截图 > 一切间接推断：两张对照图（平灰 vs 壁纸穿透）一次性终结了所有假设拉扯。

**其他**：`.theme-light` 白膜从 0.6 降到 0.45（采样复活后雾色带壁纸信息，白膜承担的提亮可让位给透感；0.8/0.6/0.45 标定史见 dashboard.uix 注释）。回归清单新增：冷启动零接触贴壁纸抓屏——壁纸色应模糊穿透玻璃（暖橙/青绿模糊块），平灰即坏。
## Why This Works

`WS_EX_NOREDIRECTIONBITMAP` 让 DWM 不再为窗口保留重定向表面，swapchain 内容经 `IDCompositionVisual` 直接进合成树；`DXGI_ALPHA_MODE_PREMULTIPLIED` 让每像素 alpha 语义生效；`Clear({0,0,0,0})` 留出透明像素；最后 DWM 亚克力在窗口之下绘制模糊桌面——四层叠出来"玻璃透桌面"。模糊发生在 DWM（桌面内容），`backdrop-filter`（窗内内容），两层互不替代、叠加成立。

启动三轮的两层修法各管一半：**显示前设置**解决材质存在性（DComp 合成树里有 backdrop，首帧即有雾、不黑屏）；**显示后值变化翻转**解决采样激活——DWM 只在"属性值变化落在已合成（已显示）的窗口上"时才把系统 backdrop 接线到桌面采样，隐藏期一次性设置与同值重设都不触发重评估，所以必须制造一次真实值变化（immersive-dark 翻出再翻回），两次定时（250/800ms）覆盖冷热启动的合成时序差。

## Verification

**目检会被骗**：窗内色场本身就是彩色渐变，深色壁纸透出来也是彩色渐变——"看起来像透"和"真的透"在肉眼层面无法区分（本项目就因此把浅色雾误判成壁纸透出放行了）。透窗验证必须用 A/B 像素对照：

1. **CopyFromScreen 拷屏**（不是 PrintWindow——NOREDIRECTIONBITMAP 窗口没有重定向表面，见 ui-bugs/core-ui-silent-rendering-traps.md 采集条目）抓窗口矩形；
2. `SetWindowPos` 把窗口平移 ~600px，等 600ms 让亚克力稳定，抓**同一屏幕区域**的纯桌面参照，再移回。**警告（三轮）：移动窗口本身就是一次踢，会把死采样踢活——此 A/B 只能用于"已确认采样存活"之后的材质颜色校准，绝不能用来判别采样死活；判死活只用零接触壁纸抓屏**；
3. 分区求均值对比：真透窗的"玻璃区"窗口均值应与桌面均值强相关（本项目实测：中心区 129,79,37 vs 桌面 144,88,37；右缘 107,75,59 vs 114,82,66——只差一层薄罩）；不透时窗口区是自己的主题色，与桌面无关。
4. 放大目检用 2-3x 最近邻裁片，别看整窗缩略图。

回归清单：**冷启动零接触贴壁纸——壁纸色（暖橙/青绿）应模糊穿透玻璃，平灰即采样死（2026-10-25 三轮启动踢）**、聊天页悬浮 composer、输入法键入、最小化→恢复（DComp 表面不破损）、深浅主题切换（immersive-dark 旗标重打 / accent tint 重涂）、设置页 ScrollView 滚动（注意：注入滚轮的 delta 要放 `mouse_event` 第 4 参 dwData，放第 3 参 dy 等于滚 0）。

日志断言：启动出现 `[app] translucent window: acrylic backdrop kind=1`。**注意 S_OK ≠ 视觉正确**——kind=1 只证明 API 接受，材质是否真的深色要靠 A/B 截图确认。

## Prevention

- **透窗抓屏零接触**：只 GetWindowRect（纯查询）+ CopyFromScreen（纯读屏）；移动/置顶/最小化-还原都是踢，会污染测量。
- **采样死活只用壁纸判别**：死雾恒定 228-242，活雾被拉向壁纸色；白背景掩盖死活。回归：冷启动零接触贴壁纸抓屏（见 Verification 回归清单第一条）。
- **S_OK ≠ 视觉正确**：DWM 属性设置成功不代表采样/深浅生效，必须 A/B 像素确认。
- **彩色色场 α ≤ ~0.32（暗色背板）**；玻璃卡输入框/容器不写实色背景——防"粉彩糊"回潮。
- **校准必须像素实测**；屏捕路径失效时留 TODO，不按惯例取值拍板（二轮教训）。

## Related Issues

- `docs/solutions/ui-bugs/core-ui-silent-rendering-traps.md` — 采集方法同源（PrintWindow vs CopyFromScreen）。
- `docs/solutions/architecture-patterns/dual-repo-core-ui-submodule-workflow.md` — core-ui 改动的提交流程（先 core-ui commit，manager bump pin）。
