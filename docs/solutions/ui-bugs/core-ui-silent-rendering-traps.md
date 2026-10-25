---
title: "core-ui 静默视觉陷阱：height-0 隐藏回退、子树 opacity、百分比圆角、transition 作用域与 CSS 解析盲区（8 例）"
date: 2026-10-24
category: ui-bugs
module: core-ui / dashboard UIX bindings
problem_type: ui_bug
component: tooling
severity: medium
symptoms:
  - "导航下划线刻度用 `:height=\"cond ? 3 : 0\"` 隐藏时渲染成 24 DIP 高的竖向色条（core-ui DoLayout 把 <=0 高度回退为 24px 默认值）"
  - "LED 状态点整体不可见——脉冲环 wrapper 初始 `:opacity=\"0\"`，widget opacity 作用于整棵子树，子级圆点一起被隐藏"
  - "圆点与圆环渲染为直角方块——`border-radius: 50%` 百分比写法 core-ui 不解析，需用 9999px"
  - "hover 透明度过渡是静默 no-op——CSS transition 只动画绑定驱动的属性变化，`:hover` 伪类改属性不经过该路径"
  - "`:enabled=\"false\"` 绑定不产生视觉禁用态——div 上的 enabled 只拦输入，需显式 `.btn-x:disabled` CSS（含 label 单独规则）"
  - "`radial-gradient(ellipse at 12% 0%, ...)` 的 `at` 位置前缀被静默忽略——渐变中心永远停在默认 50%/50%"
  - "多层逗号背景 `background: g1, g2` 不支持——值分词器把函数参数里的逗号也切开，多层写法整个失效"
  - "`backdrop-filter: blur()` 写在 ScrollView 上是静默 no-op——ScrollViewWidget 覆写 DrawTree 跳过基类的背景模糊/投影绘制"
  - "绝对定位 `left:0; right:0` 不拉伸宽度——宽度取 fixedW 或 SizeHint，posRight 只在 posLeft 缺席时作锚点"
  - "标题栏上方多出一条背景色带——净零 margin div 的边距算术差了 +20 DIP，把整个流内容顶了下来"
root_cause: wrong_api
resolution_type: code_fix
tags: [core-ui, dashboard-uix, uix-binding, opacity-subtree, border-radius, css-transition, disabled-style, silent-failure, radial-gradient, backdrop-filter, absolute-positioning, liquid-glass]
---

# core-ui 静默视觉陷阱：两次视觉迭代暴露的八个渲染缺陷与目检方法

## Problem

在两次 dashboard 视觉迭代（重设计、液态玻璃）中，我们按"常规 CSS/绑定直觉"写出的八处 UI 代码在 core-ui 中全部**静默失效或渲染错形**——不报错、不告警，只能靠截图目检发现。本文记录八个缺陷的修法、根因（落在 core-ui 的具体行为上），以及一套 DPI 感知 + 像素普查的截图验证方法。

## Symptoms

全部症状都由对运行窗口的截图检查发现（肉眼/视觉模型均未见任何日志线索）：

- **侧边栏出现神秘的高条**：导航项下方的"当前页"下划线小刻度（tick），非激活页本应隐藏，却渲染成 24 DIP 高的竖向色条，看起来像错位的强调色块（缺陷 1：height-0 回退）。
- **LED 状态点整体消失**：状态指示灯（dot + pulse ring）完全不可见——绿色像素普查证实整块区域无 LED 色（缺陷 2：父容器 opacity 拖垮子树）。
- **圆点变方块**：LED 圆点与圆环被渲染为直角方块，"圆"完全丢失（缺陷 3：`border-radius: 50%` 不解析）。
- **按钮无悬停/禁用反馈**：按钮 hover 无淡入淡出；`:enabled="false"` 的按钮视觉上与可用态完全相同（缺陷 4：transition 只消费绑定驱动属性 + `:enabled` 不改样式）。
- **渐变位置不生效**：`radial-gradient(ellipse at 12% 0%, …)` 写了 `at` 中心，三团色晕全部渲染在默认中心（缺陷 5）。
- **多层背景整体消失**：`background: radial-gradient(…), radial-gradient(…)` 多层写法连第一层都不渲染（缺陷 6）。
- **设置页玻璃底板做不出来**：`backdrop-filter` 写在 ScrollView 上无任何模糊效果（缺陷 7）。
- **悬浮玻璃输入栏只占内容区 72% 宽**：`position:absolute; left:0; right:0` 没有把 composer 拉到满宽（缺陷 8）。
- **标题栏上方一条背景色带**：净零边距定位的色场 div 把流内容整体顶下来 20 DIP（缺陷 9：边距算术，见 What Didn't Work）。
- **假警报（不是 bug）**：首批截图显示"卡片被窗口边缘裁切、第三个 metric 被截断"——实际是截图进程 DPI 不感知导致的采集裁切，UI 本身完好（见 What Didn't Work）。

## What Didn't Work

- **` :height="page=='chat' ? 3 : 0"` 隐藏刻度**：core-ui 的 VBox 布局把 `<=0` 的高度替换为 24px 默认值，"隐藏"的刻度反而变成 24 DIP 高的竖条。绑定 0 不是隐藏手段。
- **用父容器 `:opacity` 做脉冲淡入**：脉冲环 wrapper 初始 `:opacity="ringOp"` 为 0，结果子级 LED dot 也从未渲染——opacity 作用于整棵子树的合成，不是只作用于自身背景/边框。
- **`border-radius: 50%`**：core-ui 的 radius 解析只接受长度值，百分比被静默丢弃（编译无错、渲染为直角）。
- **CSS `:hover` + `transition` 的组合**：core-ui 的 transition 只在 `PageState::ApplyBindingToWidget` 的 *MaybeAnimated setter 路径上被消费，`:hover` 伪类驱动的属性变化完全不经过这条路径，动画是静默 no-op——与 AGENTS.md 已记录的 `:placeholder` 静默 no-op 同一家族。
- **只写 `.btn-x:disabled { background; color; }`**：`:enabled="false"` 作用在 div 上只拦截输入，不改视觉；且按钮 label（子元素）不继承父级的 disabled color，还必须额外给 label 写一条规则。（旧代码里存在一个从未被引用的死类 `.btn-start-disabled`——那就是旧 bug 的残骸。）
- **页面切换淡入动画（已尝试并移除）**：core-ui 的 PaintedOnce 挂载门会在首帧前"拍实"（snap）所有 pre-paint 绑定，任何入场动画启动前都会强制出现约 40ms 的完全不可见拍；且页面脚本里没有 JS 定时器可用，不存在无闪烁的挂载动画路径。该方案整段移除，作为已否决方案记录在案。
- **`radial-gradient(ellipse at 12% 0%, …)` 定位色晕**：`at` 前缀被静默忽略（值分词器跳过前缀桶），中心永远 50%/50%——写 `at` 与不写结果相同，无告警。
- **多层背景一次写完**：`background: g1, g2` 的逗号被值分词器当参数分隔符切开，多层语法整体失效（连第一层都不渲染）。每层渐变必须独占一个元素。
- **`backdrop-filter` 直接写在 ScrollView 上**：`ScrollViewWidget::DrawTree`（controls.cpp:2481）覆写后直接调 `OnDraw`，跳过 `Widget::DrawTree` 基类路径里的 backdrop-blur / box-shadow 绘制——模糊静默 no-op。玻璃必须放在包住 ScrollView 的普通 div 上。
- **`position:absolute; left:0; right:0` 期望左右拉伸**：绝对定位宽度只取 `fixedW` 或 `SizeHint()`（widget.cpp:411-421），`posRight` 仅在 `posLeft` 缺席时当定位锚点，不参与宽度求解。满宽必须显式 `width`（窗口固定尺寸时可直接算出）。
- **净零边距算术只加不减**：`.backdrop-b` 写了 `margin-top:220 + height:300 + margin-bottom:-500`，净流高 = +20 DIP——把标题栏整体顶下来，窗口顶部出现一条背景色带。净零 div 的 margin 必须满足 `margin-top + height + margin-bottom = 0`（此例 -520 才对）。
- **DPI 不感知的截图（假警报来源）**：PowerShell PrintWindow（`PW_RENDERFULLCONTENT=2`）在一个 150% 缩放的机器上，采集进程 DPI 不感知，窗口真实物理尺寸 645x750，DC 却硬裁到 430px——"卡片被裁切、第三个 metric 截断"完全是采集侧假象。修正：在 GetWindowRect/PrintWindow 之前先 P/Invoke `SetProcessDPIAware()`。
- **PrintWindow 采集 DComp 窗口拿到空/旧内容**：透窗版（WS_EX_NOREDIRECTIONBITMAP + DirectComposition）不走重定向表面，PrintWindow 抓不到合成内容——改用 `CopyFromScreen` 按窗口屏幕区域拷屏（代价：重叠窗口/光标会入镜）。**采图前零接触**（2026-10-25 修订）：移动/前置/还原窗口本身就是一次 backdrop 踢，会把死采样踢活、伪造"采集正常"——只做纯查询（GetWindowRect）+ 纯读屏（CopyFromScreen），判别采样死活用壁纸对照，见 architecture-patterns/translucent-window-dwm-acrylic.md 三轮。
- **视觉模型读小图标不可靠**：对小尺寸图标的判读反复互相矛盾；曾把渲染内容"读"成不存在的"fox logo"（幻觉）。像素扫描才稳定。
- **鼠标位置污染截图**：首批截图时鼠标恰好停在某个导航项上，截到的是 hover 态。采图前需 `SetCursorPos` 把光标移开。

## Solution

### 缺陷 1：隐藏元素用 `:visible`，高度只写常量 CSS

`nanobot-manager/ui/dashboard.uix`：

```html
<!-- before：height 0 落进 24px 回退，隐藏失败变竖条 -->
<div class="nav-tick" :height="page=='chat' ? 3 : 0"></div>

<!-- after：高度固定 3px（CSS），可见性由绑定切换 -->
<div class="nav-tick" :visible="page=='chat'"></div>
```

```css
.nav-tick {
  width: 16px;
  height: 3px;   /* 常量，不再参与绑定 */
  border-radius: 1px;
}
```

### 缺陷 2：脉冲只做尺寸编排，祖先 opacity 永远为 1

`nanobot-manager/ui/dashboard.uix`（尺寸由绑定驱动，透明度不动）：

```html
<div :class="statusRingClass" :width="ringSize" :height="ringSize">
  <div :class="statusDotClass" :opacity="ambientGlow"></div>
</div>
```

```css
.pulse-ring {
  border-radius: 9999px;
  border: 2px solid transparent;      /* 静止时 16px 环 = 10px 圆点外的光晕 */
  transition: width 450ms ease-out, height 450ms ease-out;
}
```

`nanobot-manager/src/main.cpp`：C 侧一次性重复定时器驱动 `ringSize` 16→28→16：

```c
#define TIMER_PULSE   1007
#define PULSE_TICK_MS 470   /* 450ms CSS transition 落在相邻两拍之间 */

on_timer_pulse:                       /* 3 拍序列 */
  t0      reset   ui_page_set_float(g_page, "ringSize", 16.0f);
  t0+1tk  bloom   ui_page_set_float(g_page, "ringSize", 28.0f);
  t0+2tk  retract ui_page_set_float(g_page, "ringSize", 16.0f);  /* KillTimer */

led_pulse_start(): ringSize=16 → SetTimer(TIMER_PULSE, PULSE_TICK_MS, on_timer_pulse);
```

### 缺陷 3：圆形用 `border-radius: 9999px`

```css
/* before：百分比不解析，静默渲染为直角 */
.status-dot { border-radius: 50%; }

/* after：与 core-ui 设计系统圆形 token 一致（见 core-ui/docs/design-system.md radius 命名空间） */
.status-dot  { border-radius: 9999px; }
.pulse-ring  { border-radius: 9999px; }
```

### 缺陷 4：hover 动画走"绑定蹦床"，disabled 视觉显式写规则

`nanobot-manager/ui/dashboard.uix`（方法 + 绑定）：

```js
hin(id) { this.hover = id; },
hout(id) { if (this.hover == id) this.hover = ""; }
```

```html
<div class="btn btn-start" id="btn_start"
     :enabled="status=='stopped'"
     :opacity="hover=='btn_start' && status=='stopped' && page=='dashboard' ? 0.92 : 1"
     @mouseenter="hin('btn_start')" @mouseleave="hout('btn_start')">
```

```css
.btn-start { transition: opacity 120ms ease-out; }

.btn-start:disabled,
.btn-start:disabled:hover {
  background: var(--disabled-bg);
  color: var(--disabled-text);
}
.btn-start:disabled .btn-label {   /* label 不继承父级 disabled color */
  color: var(--disabled-text);
}
```

opacity 表达式里的 `page=='…'` 项是自愈保险：`v-if` 切页时被卸载的控件可能收不到 `mouseleave`，`hover` 会卡住；换页后 page 条件变假，透明度立即回到 1。

### 缺陷 5+6：色场定位用"净零流 div"，每 div 一层渐变

`at` 前缀被忽略、多层逗号背景不支持的组合绕法（液态玻璃的彩色底景）：

```css
/* 每团色晕一个元素：单层渐变（无 at 前缀，中心默认），
   用元素矩形 + 净零边距把渐变中心挪到目标位置 */
.backdrop-b {
  width: 300px; height: 300px;
  margin-top: 220px;
  margin-bottom: -520px;   /* -(220+300)：净零，否则 +20 流高顶下标题栏 */
  align-self: flex-end;    /* 水平位置靠交叉轴对齐 */
  background: radial-gradient(rgba(106,94,188,0.75), rgba(106,94,188,0) 70%);
}
```

```html
<div class="backdrop-a"></div><div class="backdrop-b"></div><div class="backdrop-c"></div>
```

模板里这些 div 放在 `.root` 最前（画在最底层），后续兄弟正常布局。

### 缺陷 7：玻璃放 wrapper div，不放 ScrollView

```html
<!-- before：backdrop-filter 在 ScrollView 上静默 no-op -->
<ScrollView v-if="page=='settings'" class="settings-glass">…</ScrollView>

<!-- after：玻璃在普通 div 上，ScrollView 填充其内 -->
<div v-if="page=='settings'" class="settings-glass">
  <ScrollView class="settings-scroll">…</ScrollView>
</div>
```

### 缺陷 8：绝对定位满宽写显式 width

```css
/* before：left+right 组合不参与宽度求解，composer 塌缩成内容自然宽度 */
.composer { position: absolute; left: 0; right: 0; bottom: 0; }

/* after：窗口 430x500 固定、侧栏 48 → 聊天列恒 382 DIP，直接定宽 */
.composer { position: absolute; left: 0; bottom: 0; width: 382px; }
```

- **缺陷 5 根因**：core-ui 的渐变值解析器按逗号切参数后逐桶匹配，`at` 位置前缀所在的桶被跳过（不报错、不解析），渐变中心停在默认 50%/50%。
- **缺陷 6 根因**：CSS 值分词器把函数参数内的逗号也当分隔符，`background: g1, g2` 被切成两个无效值——多层背景语法整体不支持，而非部分支持。
- **缺陷 7 根因**：`ScrollViewWidget::DrawTree`（controls.cpp:2481）为滚动裁剪覆写了绘制路径，直接调 `OnDraw`（只画滚动条），跳过 `Widget::DrawTree` 基类里的 `DrawBackdropBlur`/`DrawBoxShadow` 段（widget.cpp:518 附近）——基类支持的能力在 ScrollView 子类上静默缺失。同理适用于 box-shadow。
- **缺陷 8 根因**：绝对定位子节点的求解在 `widget.cpp:411-421`：`w = fixedW > 0 ? fixedW : (hint.width > 0 ? hint.width : cw)`，`left/right` 只决定 `x`（`pr` 仅在 `pl` 缺席时使用）——不存在"两端约束求宽度"的分支。
- **净零边距**：net-zero 流 div 依赖 `margin-top + height + margin-bottom = 0` 的算术；漏算一项就变成"带残留流高"的占位元素，把后续兄弟顶下来。

### 验证方法（目检基建）

- **DPI 感知采集**：PowerShell PrintWindow（`PW_RENDERFULLCONTENT=2`）之前先 P/Invoke `SetProcessDPIAware()`；否则 150% 缩放机器上 DC 被硬裁到逻辑像素宽（430px），产生"卡片被裁切"假警报。
- **像素普查优于视觉模型**：System.Drawing `GetPixel` 做带容差窗口的颜色普查 + 按行聚类检测，作为稳定 ground truth；视觉模型读小图标屡次自相矛盾（"fox logo"属幻觉）。两者交叉：绿色像素普查证实了 LED 缺失（真 bug），同时否决了幻觉读数。
- **采图前移走鼠标**：`SetCursorPos` 把光标挪离 UI，避免截到导航 hover 态。

## Why This Works

- **缺陷 1 根因**：`core-ui/src/ui/widget.cpp` 的 `VBoxWidget::DoLayout` 有一行 `if (childH <= 0) childH = 24.0f;`——0 高度不是"零高度"而是"未指定"，布局给它 24px 默认值。`:visible` 走的是独立的可见性开关，不经过这个回退，隐藏才是真隐藏。
- **缺陷 2 根因**：opacity 是合成属性，作用于整棵子树的最终渲染，父级 0 会让所有后代一起消失。改为只绑定 `:width/:height` 后，脉冲是纯几何编排；450ms 的 CSS transition 恰好消费这两个绑定驱动属性（见缺陷 4 根因），`470ms` 定时器拍与 `450ms` transition 的错位让"绽放→收回"自然衔接。静止的 16px 环 + 2px 语义色边框顺带成为 10px 圆点的光晕，一层结构两用。
- **缺陷 3 根因**：core-ui 的 border-radius 解析器只接受长度（length）值，百分比分支不存在——`50%` 被静默丢弃，回退到直角。`9999px` 是长度值，且大于任何直径，语义上恰为"全圆"，与 core-ui 设计系统自己的圆形 token 一致。
- **缺陷 4 根因**：transition 只在 `core-ui/src/ui/page/page_state.cpp` 的 `ApplyBindingToWidget` 中被消费——所有 `Set*MaybeAnimated`（`SetOpacityMaybeAnimated`、`SetWidthMaybeAnimated`、`SetHeightMaybeAnimated`、`SetBgMaybeAnimated`…）只在这些 setter 内启动。CSS `:hover` 改属性不经过绑定管道，动画无处发生。把 hover 状态装进响应式字段 `hover`、再用 `:opacity` 表达式输出，状态变化就回到了绑定驱动路径上，transition 生效。同理 `:enabled="false"` 在 div 上只是输入门（不拦渲染样式），视觉禁用态必须由 `:disabled` CSS 显式表达；而 label 是独立控件、不继承父级 color，故需单独的 `.btn-x:disabled .btn-label` 规则。

## Prevention

core-ui UI 编写清单（每条都对应一个已付过学费的静默陷阱）：

- [ ] **永远不要用 `:height="0"` / `:width="0"` 隐藏元素**——VBox/HBox 布局把 `<=0` 当"未指定"回退 24px。隐藏一律 `:visible="expr"`；尺寸只写常量 CSS。
- [ ] **不要让必须保持可见的内容住在被动画化 opacity 的祖先之下**——opacity 按子树合成。淡入淡出祖先前先问：子级是否也要跟着淡？若否，动画只碰尺寸/自身背景，或把 opacity 绑定下放到每个叶子。
- [ ] **圆形一律 `border-radius: 9999px`**——百分比不解析且无告警；与 `core-ui/docs/design-system.md` 的 radius 命名空间保持一致。
- [ ] **CSS 状态（`:hover`/`:active`）驱动的属性变化不会动画**——transition 只消费绑定驱动属性。要 hover 动画就走"绑定蹦床"：`@mouseenter/@mouseleave` → 响应式 `hover` 字段 → `:opacity` 表达式（含 `page=='…'` 自愈项）+ `transition`。
- [ ] **`:enabled="false"` 不产生视觉禁用态**——必须显式写 `.x:disabled { background: var(--disabled-bg); color: var(--disabled-text); }`，并追加 `.x:disabled .label { color: var(--disabled-text); }`（label 不继承）。写完删掉一切无引用的死类（如旧 `.btn-start-disabled`）。
- [ ] **不要尝试挂载/入场动画**——PaintedOnce 门 + 页面脚本无 JS 定时器 = 无闪烁路径；该方案已尝试并移除。
- [ ] **凡做视觉验证，截图脚本必过 DPI 检查**：采集前 `SetProcessDPIAware()`（P/Invoke）；对照 `GetWindowRect` 的宽高与位图像素尺寸是否一致，不一致即采集侧假象，先修采集再下 bug 结论。
- [ ] **小图标判定以像素普查为准**：GetPixel 颜色普查（带容差窗口）+ 按行聚类；视觉模型读数只作交叉参考，两者矛盾时信像素。
- [ ] **采图前 `SetCursorPos` 移开鼠标**，避免截到 hover 态造成误读。
- [ ] **`radial-gradient` 不写 `at` 前缀**（静默忽略，中心恒默认）；要摆位置就用"净零流 div"：元素矩形 + `margin-top + height + margin-bottom = 0` 严格算平（漏算会出现幽灵色带/占位）。
- [ ] **多层逗号背景不支持**——一层渐变一个元素，不要写 `background: g1, g2`。
- [ ] **`backdrop-filter`/`box-shadow` 不要直接写在 ScrollView 上**（覆写的 DrawTree 跳过基类绘制）——玻璃/投影放包住它的普通 div。
- [ ] **绝对定位满宽写显式 `width`**——`left`+`right` 组合不拉伸宽度；固定尺寸窗口可直接算出列宽（如 430 - 48 侧栏 = 382px）。
- [ ] **采集 DComp/NOREDIRECTIONBITMAP 窗口用 CopyFromScreen 拷屏，且零接触**——PrintWindow 抓不到组合表面内容；移开光标可以，但移动/前置/还原窗口都是 backdrop 踢，会把死采样踢活、伪造通过（判采样死活用壁纸对照，见 architecture-patterns/translucent-window-dwm-acrylic.md 三轮）。

## Related Issues

- `docs/solutions/reference/core-ui-overview.md` — 框架能力总览；其 CSS 子集清单把 `transition`/`opacity`/`:hover`/`:disabled` 列为无条件支持，正是本文四个陷阱的藏身处（刷新候选，见下）。
- `docs/solutions/ui-bugs/ime-candidate-window-top-left-custom-textinput.md` — 同族"core-ui 静默降级"缺陷（未绑定测量上下文静默报 0），交叉验证哲学一致。
- `docs/solutions/ui-bugs/chat-bubbles-overlap-after-scroll.md` — 同为截图驱动诊断的静默布局缺陷，且同样排除过 DPI 因素。
- `docs/solutions/ui-bugs/refresh-button-silent-in-idle.md` — 仓内 `:enabled` + 显式 `:disabled` 样式的既有范例（本文缺陷 4 的先行实践）。
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — 同族 core-ui API 静默缺口（输入读取路径）。
- AGENTS.md "Dynamic hints in settings" 的 `:placeholder` 静默 no-op 说明 — 同一家族（CSS 相似但语义不同的 API）。
