---
title: App icon must use nanobot mark, multi-size ICO pipeline
date: 2026-07-31
category: tooling-decisions
module: nanobot-manager
problem_type: tooling_decision
component: tooling
severity: low
applies_when:
  - updating nanobot-manager tray/titlebar/installer icons
  - regenerating assets/app.ico from brand SVGs
  - syncing brand assets from sibling nanobot repo
tags:
  - app-icon
  - nanobot-mark
  - ico
  - tray-icon
  - resources-rc
  - nsis
  - brand-assets
---

# App icon must use nanobot mark, multi-size ICO pipeline

## Context

Windows app chrome (EXE resource, tray, taskbar, NSIS installer) needs a multi-size `.ico`. Brand assets live in the sibling nanobot repo; the manager product embeds one file:

| Asset | Role |
|-------|------|
| `nanobot/images/nanobot_logo.svg` / `.png` | Wide wordmark (~1060×220 / 1151×216): robot + “nanobot” text |
| `nanobot/images/nanobot_mark.svg` | Robot head only (viewBox ~759×718) — correct source for app icon |
| `nanobot/webui/public/brand/` | Pre-sized web icons (`nanobot_icon.png` 73×75, favicon 32, apple-touch, mark/logo copies) |
| `nanobot-manager/assets/app.ico` | Embedded app / tray / installer icon |
| `nanobot-manager/src/resources.rc` | `1 ICON "../assets/app.ico"` |
| `main.cpp` tray | `LoadImageW` resource 1, fallback `IDI_APPLICATION` |
| `installer/installer.nsi` | `MUI_ICON` / `MUI_UNICON`, ships `app.ico`, `DisplayIcon` registry |

**Failed approach:** Building the ICO from the wide wordmark (`nanobot_logo`) by squashing the logo into square slots. At 16–32 px the wordmark blurs and becomes unreadable; users reject it for tray/taskbar.

**Working approach:** Source the mark SVG, rasterize at high resolution, crop/pad into square frames, emit a real multi-size ICO, then rebuild so `resources.rc` and the installer pick up the new file.

## Guidance

### Prefer mark over wordmark for Windows chrome

- Use **`nanobot_mark.svg`** (robot head only) for `.ico`, tray, taskbar, and installer icons.
- Keep **wordmark / logo** for README, marketing, splash, and wide UI — not for small system chrome.
- When the sibling repo is available, copy brand from the nanobot checkout under `images/` and `webui/public/brand/`. Do not invent an alternate mascot.

Optional but useful: keep a product-tree copy at `nanobot-manager/assets/nanobot_mark.svg` so rebuilds do not depend on the sibling path every time.

### Multi-size ICO requirements

Ship at least **16, 32, 48, 256** (recommended full set: **16 / 24 / 32 / 48 / 64 / 128 / 256**). Windows picks sizes by context; a single 16×16-only ICO looks wrong in Explorer and the installer.

### Rasterize SVG without cairo/rsvg

If ImageMagick/`rsvg-convert`/cairo are missing, use headless Chrome:

1. HTML page: transparent background, `<img>` with `object-fit: contain` filling a fixed square viewport.
2. Capture:

```text
chrome --headless=new --window-size=1024,1024 --screenshot=out.png --default-background-color=00000000 path\to\page.html
```

3. Prefer a large capture (e.g. 1024×1024) so downscales stay sharp.

### Pillow pipeline (crop → pad → multi-size)

1. Open the PNG; `Image.getbbox()` to crop empty transparency.
2. Fit into a square canvas with ~**6% padding** so the mark does not touch the icon edge.
3. Generate each size with high-quality resample (`LANCZOS`).
4. Write a **true multi-size ICO**:

   - Do **not** rely on `save(..., append_images=...)` alone if you only get a 16×16 result (observed failure mode).
   - Prefer writing **PNG-compressed ICONDIR entries** with `struct.pack` (or a known-good multi-size ICO writer) so every size is present in the file.

### Embed and package after `app.ico` changes

1. Replace `nanobot-manager/assets/app.ico`.
2. **Rebuild Release** so `resources.rc` re-embeds the icon into `nanobot-manager.exe` (tray uses resource id 1).
3. Run packaging (`scripts\package.bat`) so NSIS ships the same `assets/app.ico` and sets installer/uninstaller/`DisplayIcon` correctly.
4. Verify tray, taskbar, Explorer properties, and installer UI — not only the file on disk.

### Windows PowerShell pitfalls

- No bash heredocs for Python: write a temporary `.py` file and run it.
- `C:\Windows\System32\convert.exe` is **not** ImageMagick; do not call bare `convert` for image work.

## Why This Matters

- Wordmarks collapse at tray/taskbar sizes; marks stay legible and match WebUI brand icons.
- A single-size or broken multi-size ICO makes the product look unfinished across Explorer, tray, and setup.
- Rebuild + package are required for users to see the icon; editing `app.ico` alone does not update an already-built EXE.
- Sourcing from the official nanobot brand tree keeps manager and WebUI visually consistent.

## When to Apply

- Creating or refreshing `nanobot-manager/assets/app.ico`.
- Tray / taskbar / installer icons look blurry, cropped, or “wrong brand.”
- Someone proposes using `nanobot_logo` (wide wordmark) as the Windows icon source.
- Brand assets change in the sibling nanobot repo and the manager icon should track them.
- Packaging a release after any icon asset change.

## Examples

### Correct source choice

```text
# Good — mark only
nanobot/images/nanobot_mark.svg  →  assets/app.ico (multi-size)

# Bad — wordmark squeezed into squares
nanobot/images/nanobot_logo.svg  →  16/32 px tray (unreadable)
```

### End-to-end rebuild checklist

1. Copy `nanobot_mark.svg` → `nanobot-manager/assets/` (optional local SoT).
2. Headless Chrome screenshot at 1024×1024, transparent background.
3. Pillow: crop bbox → square with ~6% pad → sizes 16…256.
4. Write multi-size ICO with explicit ICONDIR/PNG entries (verify file contains all sizes, not only 16×16).
5. `scripts\build.bat` (Release embeds via `resources.rc`).
6. `scripts\package.bat` → `dist/nanobot-manager-setup.exe`.
7. Smoke: tray icon, window icon, installer MUI icon, uninstaller, `DisplayIcon`.

### Code touchpoints (do not invent new paths)

- Embed: `nanobot-manager/src/resources.rc` → `1 ICON "../assets/app.ico"`
- Tray load: `main.cpp` → `LoadImageW(..., 1, ...)` with `IDI_APPLICATION` fallback
- Installer: `installer/installer.nsi` → `MUI_ICON` / `MUI_UNICON` / `File` of `app.ico` / registry `DisplayIcon`

## Related

- Sibling brand: nanobot repo under `images/` and `webui/public/brand/`
- Product layout: `nanobot-manager/assets/`, `src/resources.rc`, `installer/installer.nsi`, `scripts/build.bat`, `scripts/package.bat`
- Weak adjacency only: gateway lifecycle packaging smoke (`scripts\package.bat` / NSIS) in architecture docs — not brand guidance
