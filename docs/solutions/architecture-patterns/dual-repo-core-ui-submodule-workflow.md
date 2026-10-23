---
title: "Dual-repo architecture: parent product pin vs core-ui submodule"
date: 2026-07-31
last_updated: 2026-10-24
category: architecture-patterns
module: submodule/core-ui
problem_type: architecture_pattern
component: development_workflow
severity: medium
applies_when:
  - "Implementing UI framework APIs in core-ui then consuming them in nanobot-manager"
  - "Bumping or changing the core-ui git submodule pin or remote URL"
  - "Cloning the parent repo and initializing submodules after a pin or remote change"
  - "Mistaking local core-ui working-tree commits for content recorded in a parent commit"
tags:
  - core-ui
  - git-submodule
  - gitlink
  - dual-repo
  - submodule-pin
  - gitmodules
  - development-workflow
---

# Dual-repo architecture: parent product pin vs core-ui submodule

## Context

`nanobot-windows-gui` is a dual-repo product:

| Path | Role |
|------|------|
| `nanobot-manager/` | Parent product (Win32 manager, chat, process, config) |
| `core-ui/` | **Git submodule** — shared UI framework, not product source |

Parent records only a **gitlink SHA** (pin) for `core-ui`. Framework commits live in the core-ui repository. Product work that needs new Core UI APIs must land in core-ui first, then bump the pin in the parent.

> **The pin moves over time.** The concrete SHAs below are historical examples from when this workflow was first documented (the remote-retarget + submit/scroll-API session). Don't treat them as the current pin — verify with `git ls-tree HEAD core-ui` (currently `75aaaa8` = upstream v1.8.0 `126330e` + the chat-scroll port, at last update). The **workflow** is what's durable; the example SHAs are illustrative.

The original session retargeted the submodule remote from `ghboke/core-ui` to `https://github.com/aiguozhi123456/core-ui`, kept the pin at the submit/scroll-APIs commit (`f0a9537` at the time), and cleaned feature branches after merge. Parent history of note:

- `06f26cb` — bump pin to `e0298ca` (v1.7.0)
- `0045496` — pin `e0298ca`→`f0a9537` while hardening chat (uses submit/scroll APIs)
- `33b2cd6` — `.gitmodules` URL change only
- Later: pin advanced through the IME-caret (`442461d`) and chat-bubble-scroll-layout (`b594ec0`) framework fixes.
- 2026-10: fork `main` **rebased onto upstream v1.8.0** (`126330e`) — the chat scroll APIs were re-ported as `75aaaa8` (submit API dropped: upstream build 287's `ui_widget_on_submit` supersedes it; IME + scroll-layout fixes absorbed upstream). Old lineage preserved on fork branch `legacy-v1.7-fork`. This is the "upstream moved a lot, cherry-pick your deltas" variant of workflow (b).

## Guidance

### 1. Two-repo mental model

- **Parent** stores a single tree entry for `core-ui` (mode `160000` gitlink) → one SHA.
- **core-ui** owns framework history, tags, and API surface.
- Editing files under `core-ui/` in the parent working tree does **not** ship with a parent commit unless you commit inside the submodule repo and then update the parent gitlink.

```powershell
# Parent gitlink (pin) only
git ls-tree HEAD core-ui
# e.g. 160000 commit f0a9537...	core-ui

# Actual framework HEAD
git -C core-ui rev-parse HEAD
```

### 2. Remote URL vs pin SHA are independent

Changing `.gitmodules` / `origin` does **not** move content. The pin SHA still points at whatever commit object that hash names. Retargeting remote only changes where `git submodule update` and `fetch` look for that object.

| Change | Affects | Does not affect |
|--------|---------|-----------------|
| `.gitmodules` + `submodule sync` + `remote set-url` | Fetch/push URL | Pin SHA, tree content |
| `git add core-ui` after submodule commit | Parent pin SHA | Remote URL |
| core-ui commit on its main | Objects available once pushed | Parent pin until parent bumps |

### 3. Safe change workflows

#### a) Retarget submodule remote only

```powershell
# Edit .gitmodules url = https://github.com/aiguozhi123456/core-ui
git submodule sync --recursive
git -C core-ui remote set-url origin https://github.com/aiguozhi123456/core-ui
git -C core-ui remote -v
# Pin unchanged — verify:
git -C core-ui rev-parse HEAD
git ls-tree HEAD core-ui
```

Commit only `.gitmodules` (and any recorded submodule URL metadata) if that is the sole intent — same pattern as parent `33b2cd6`.

#### b) Land API/feature in core-ui first

```powershell
cd core-ui
git checkout main
git pull origin main
# ... implement, e.g. submit/scroll APIs ...
git add -A
git commit -m "feat(ui): add submit and scroll APIs for native chat"
git push origin main
# Note SHA for parent pin, e.g. f0a9537
git rev-parse --short HEAD
```

Prefer merge (or FF) to core-ui `main` (or a published release branch) before the parent depends on the commit. Feature branch example from this session: `feat/chat-input-scroll-apis` → FF-merged to main, then deleted.

#### c) Then parent: bump pin + consume + test

```powershell
cd <parent-repo>
git -C core-ui fetch origin
git -C core-ui checkout <published-sha>   # or main after pull
git add core-ui
# Wire product code (main.cpp, dashboard.uix, tests) to new APIs
git add nanobot-manager/
git commit -m "chore: bump core-ui pin to <sha> for chat submit/scroll APIs"
```

Build and CTest from manager scripts; do not treat submodule tree edits as monorepo product source.

### 4. Verify the pin exists (clone/CI hazard)

```powershell
$pin = (git ls-tree HEAD core-ui) -replace '.*commit ([0-9a-f]+).*','$1'
$head = git -C core-ui rev-parse HEAD
if ($pin -ne $head) { Write-Error "Mismatch: parent pin=$pin core-ui HEAD=$head" }

# Object must be reachable on the configured remote before others clone
git -C core-ui cat-file -t $pin          # should be "commit"
git -C core-ui branch -r --contains $pin # should list origin/main (or release branch)
git ls-remote origin $pin                # non-empty if remote has the object
```

**Network note:** GitHub fetch/push over 443 can fail in a session; that is environmental. Declining push leaves the pin as a **clone hazard** until the commit is on the remote — not a code bug. Agents must not claim the pin is “safe for CI/clones” without a successful remote presence check.

### 5. What not to do

- Edit `core-ui/` as if it were monorepo product source and only commit the parent — framework blobs never leave the developer machine.
- Assume a parent commit “includes” core-ui file contents; it only records the gitlink SHA.
- Change only the submodule URL and expect pin content or APIs to move.
- Bump the parent pin to a SHA that exists only locally (unpushed feature branch).
- Leave long-lived feature branches after merge; clean them once mains are aligned.

### 6. Dual-repo checkout hygiene

After a dual-repo change lands:

```powershell
# Both repos on main, pins match
git status
git -C core-ui status
git rev-parse --abbrev-ref HEAD          # main
git -C core-ui rev-parse --abbrev-ref HEAD
git -C core-ui rev-parse HEAD
git ls-tree HEAD core-ui                 # same SHA

# Delete merged feature branches (local + remote as appropriate)
git branch -d chore/core-ui-remote
git -C core-ui branch -d feat/chat-input-scroll-apis
# git push origin --delete <branch> when remote still has them
```

Session outcome pattern: parent and core-ui HEAD both pointed at the same SHA (the submit/scroll-APIs commit at the time); the `chore/core-ui-remote` and `feat/chat-input-scroll-apis` feature branches were removed after merge to their respective mains. The same end state applies after any pin bump: `git ls-tree HEAD core-ui` (parent pin) must equal `git -C core-ui rev-parse HEAD` (submodule HEAD).

## Why This Matters

- Wrong mental model produces “works on my machine” parents that fail `git submodule update` for every other clone/CI.
- URL retarget without remote objects for the pin breaks fresh checkouts even when history looks fine.
- Consuming Core UI APIs before the framework commit is published couples product to private SHAs.
- Treating submodule files as parent source silently drops framework changes from release artifacts.

## When to Apply

- Any change that touches `core-ui/` sources, Core UI public APIs, or `.gitmodules`.
- Bumping, retargeting, or auditing the submodule pin.
- Chat/UI work that needs new submit, scroll, or other Core UI entry points.
- Onboarding or AGENTS.md guidance for agents that might edit `core-ui/` in-tree.
- Pre-PR / pre-release checks that clones and CI can resolve the gitlink.

## Examples

**Retarget only (pin stays `f0a9537`):**

```powershell
# .gitmodules → url = https://github.com/aiguozhi123456/core-ui
git submodule sync
git -C core-ui remote set-url origin https://github.com/aiguozhi123456/core-ui
git add .gitmodules
git commit -m "chore: retarget core-ui submodule remote"
# Verify pin unchanged
git ls-tree HEAD core-ui   # still f0a9537...
```

**API then pin (chat submit/scroll):**

```powershell
# In core-ui: land feat, merge to main, push → f0a9537
# In parent:
git -C core-ui checkout f0a9537
git add core-ui
# Product uses Core UI submit/scroll from main.cpp / dashboard.uix
git commit -m "chore: pin core-ui f0a9537; harden chat with submit/scroll APIs"
```

**Mismatch detection:**

```powershell
git -C core-ui rev-parse HEAD
git ls-tree HEAD core-ui
# If SHA differs: either checkout the pin in core-ui, or intentionally bump and commit the gitlink
```

## Related

- `AGENTS.md` — layout: product in `nanobot-manager/`, `core-ui/` is submodule-only; build needs `git submodule update --init --recursive`.
- [core-ui Overview](../reference/core-ui-overview.md) — capability map and pin independence from manager version (not dual-repo workflow).
- [core-ui input-read pattern](../design-patterns/core-ui-input-read-pattern.md) — manager consuming submodule APIs.
- [Testing and CI setup](../infrastructure/testing-and-ci-setup.md) — recursive submodule checkout for builds.
- Parent commits: `06f26cb` (pin e0298ca), `0045496` (pin f0a9537 + chat), `33b2cd6` (URL only).
- core-ui `f0a9537` — `feat(ui): add submit and scroll APIs for native chat` (via `feat/chat-input-scroll-apis` → main).
- Prefer `core-ui/docs/uix-ai-guide.md` and `core-ui/include/ui_core.h` over inventing UI APIs when consuming the framework.
