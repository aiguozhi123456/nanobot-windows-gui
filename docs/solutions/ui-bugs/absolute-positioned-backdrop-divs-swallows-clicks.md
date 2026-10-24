---
title: "绝对定位色场吞掉整页输入：core-ui HitTest 覆盖层优先序、注入输入假象与 hitTransparent 逃逸口"
date: 2026-10-25
category: ui-bugs
module: dashboard UIX bindings / core-ui hit-testing
problem_type: ui_bug
component: core
severity: high
symptoms:
  - "设置页整页无响应——所有 toggle/button/input 都点不动，滚轮也失效（用户真实鼠标复现）"
  - "聊天/仪表盘页在无色场覆盖的角落点击仍正常——'处处死'而非'某个控件死'的分布即是诊断线索"
  - "注入滚轮测试时好时坏：热实例能滚、冷实例静默失败，一度误导出'注入伪影，不是 app 回归'的结论"
root_cause: wrong_api
resolution_type: code_fix
tags: [core-ui, hit-test, hit-transparent, absolute-positioning, overlay, input-interception, liquid-glass, injected-input]
related_components: [dashboard.uix, main.cpp]
---

# 绝对定位色场吞掉整页输入：core-ui HitTest 覆盖层优先序、注入输入假象与 hitTransparent 逃逸口

## Problem

液态玻璃底景的三团"色场"div（`backdrop-a/b/c`，约 300x300 DIP 的大块 radial-gradient 色晕）原本走 flow 布局 + 净零边距算术（`margin-top + height + margin-bottom = 0`）。这套算术历史上就翻过车——漏算一次留下 +20 DIP 残留流高，把标题栏整体顶下来，窗口顶部出现一条幽灵色带（见姊妹篇 core-ui-silent-rendering-traps）。

为了让色晕"活"起来，feature commit **dbb60fc** 把三团色场从 flow 布局改成 `position: absolute`，由 `src/main.cpp` 新增的 `TIMER_FIELD_DRIFT`（33ms Win32 定时器，delta 跳帧，窗口隐藏/最小化时暂停）驱动缓慢的 Lissajous 漂移，通过 6 个响应式 float（`fieldAx..fieldCy`）写进 `:left`/`:top` 绑定；三层 alpha 0.32/0.28/0.25 保持不变。

改动本身看起来是纯视觉的：布局方式换了、动画加上了，没动任何 handler、没碰任何控件。但它悄悄把三团色场从"流内子节点"变成了"绝对定位子节点"——而 core-ui 的 `HitTest` 对这两类子节点的**探测优先级完全不同**。紧接着用户用真实鼠标一碰，设置页整页死掉。

## Symptoms

- **设置页整页无响应**：用户复现"滚不动，到设置页啥都点不动了"——没有任何 toggle/button/input 可点，滚轮也死。
- **分布模式（关键线索）**：聊天/仪表盘页在无色场覆盖的角落点击仍然正常。不是某个控件坏了，而是"被色场矩形罩住的区域"全死——三团 ~300x300 DIP 的色场几乎罩住了全部控件。
- **注入测试时好时坏**：注入滚轮在"热"实例上能滚、全新实例上静默失败——这个不稳定的假象后来被误判为"注入伪影"，把真回归又埋了一层（见 What Didn't Work）。

## What Didn't Work

1. **注入滚轮测试（SendInput/mouse_event，滚轮 delta 放 `dwData`）时灵时不灵**：热实例上工作、冷实例上静默失败，一度得出"是注入伪影，不是 app 回归"的**错误结论**，掩盖了真回归相当一段时间。失败原因：这台机器的测试基建对全新启动实例存在滚轮注入盲区，注入结果本身就是混杂变量——用不可靠的证据源对"输入类 bug"下结论，等于在真问题上再盖一层噪声。

2. **A/B 实验（禁掉 drift 定时器、重编、重测）依然滚不动**：这一步"正确地"排除了定时器嫌疑，但实验设计本身无效——A/B 两臂的色场**都已经是绝对定位**，无论定时器开还是关，色场照样拦截输入。它测的是"定时器是否干扰滚轮投递"，而真问题是"色场是否拦截输入"。被怀疑的机制（定位方式的改变）在两个臂里都没有被移除，实验从设计上就不可能命中根因。

3. **自动化验证点击全落在无色场的屏幕角落**（左下角齿轮按钮、导航图标间隙）：固定坐标点击造成**采样偏差**——所有采样点恰好都避开了色场矩形，回归在自动验证里完全隐形。

4. **决定性信号来自用户的真实鼠标复现**（"滚不动，到设置页啥都点不动了"）。"整页死"（EVERYTHING dead，而不是某一个控件死）这个症状模式本身就在指向一个全局拦截层（overlay hit-test），而不是逐控件的功能破坏——真实用户复现的优先级高于一切注入/自动化的间接证据。

## Solution

**根因**：`core-ui/src/ui/widget.cpp` 的 `Widget::HitTest`（widget.cpp:575-586）**先按逆 DOM 序探测所有绝对定位子节点，然后才探测 flow 子节点**。这是覆盖层（overlay）语义——聊天 composer（同为绝对定位）正是靠同一优先序浮在消息之上并接住输入。改动前三团色场是 flow 子节点，排在内容之后被探测，对输入无害；改成 `position: absolute` 后它们变成了"最先被探测"的覆盖层，三团矩形几乎罩住所有控件——命中落在色场上，冒泡链 field→root 上没有任何 handler，点击/滚轮被整个吞掉。

HitTest 的优先序（widget.cpp:575-586）：

```
for (int i = (int)children_.size() - 1; i >= 0; --i) {
    if (!children_[i]->positionAbsolute) continue;
    if (auto* hit = children_[i]->HitTest(x, y)) return hit;   // 绝对定位先测（覆盖层）
}
for (int i = (int)children_.size() - 1; i >= 0; --i) {
    if (children_[i]->positionAbsolute) continue;
    if (auto* hit = children_[i]->HitTest(x, y)) return hit;   // flow 兄弟在后
}
return hitTransparent ? nullptr : this;
```

**修复（commit 40f7876）**：给三个色场 div 加 `hitTransparent="true"`：

```html
<div class="backdrop-a" :left="fieldAx" :top="fieldAy" hitTransparent="true"></div>
```

框架本来就带着这个逃生口：`Widget` 有 `bool hitTransparent` 成员（注释原文："true 时 HitTest 只看子不返回自身，让事件穿透到下层"），并以 UIX attribute 形式暴露（markup_builder.cpp 的 key 白名单 + widget_factory.cpp 的解析）。HitTest 的终结分支 `return hitTransparent ? nullptr : this;`（widget.cpp:584）让色场在自己的子树无命中时从 hit 链上消失——**继续绘制、继续漂移，但永远不接输入**。

## Why This Works

- **hit-test 序与 paint 序共享同一套覆盖层语义**：绝对定位子节点既画在流内容之上，也先于流内容被命中。`hitTransparent` 只把色场从"命中结果"里摘掉，完全不触碰 paint 顺序——所以视觉上的分层（卡片上方那层淡淡的颜色 wash）原样保留，而输入直接落到下层的控件上。
- 换句话说：`position: absolute` 一次改变了"画在哪"和"谁接输入"两件事，而 `hitTransparent` 把第二件事单独剥离出来还给流内容——这正是装饰性覆盖层想要的行为，也是框架作者预留该开关的意图所在。

## Prevention

1. **仓内硬规则**：`.uix` 里任何绝对定位的装饰性 overlay 一律写 `hitTransparent="true"`，除非它本意就是接输入（composer、context menu、modal 层）。
2. **"整页死"症状 ⇒ 先怀疑全局拦截层**（overlay hit-test），而不是逐控件破坏。症状的分布模式（处处死 vs. 死角落）本身就是最好的分诊信号。
3. **影响输入的 UI 改动，验证时必须逐个"点穿"每个可交互控件**，而不是固定方便坐标——固定位置自动点击制造采样偏差，让回归隐形。
4. **注入输入的不稳定性（冷实例滚轮盲区）是这台机器测试基建的已知混杂因素**——仅凭 harness 得出的结论对输入类 bug 是弱证据；真实用户复现的权重更高。
5. **A/B 实验必须让"被怀疑的机制本身"在两臂间不同**——只禁定时器而保留定位改动，不可能为定位翻案；先想清楚"我在检验哪个变量"再动手。

## Related Issues

- `docs/solutions/ui-bugs/core-ui-silent-rendering-traps.md` — 同族"core-ui 静默语义"缺陷集（含净零边距 +20 DIP 幽灵色带——正是本次把色场改离 flow 布局的动因；其 composer 绝对定位条目与本条同属覆盖层家族）。
- `docs/solutions/architecture-patterns/translucent-window-dwm-acrylic.md` — 色场为何存在（液态玻璃底景）+ field-alpha 规则；其回归清单宜补"点穿色场覆盖区"一项。
- `docs/solutions/ui-bugs/chat-bubbles-overlap-after-scroll.md` — 同为 core-ui 滚轮路径的 core-ui 内部分析，症状互补（那里是滚轮到达后行为错，这里是滚轮根本到不了）。
- Dual-Repo workflow — 本例不需要动 core-ui：`hitTransparent` 是框架已有能力，纯 `.uix` 修复。
