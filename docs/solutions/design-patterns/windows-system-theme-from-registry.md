---
title: "Reading the Windows system theme from the registry (AppsUseLightTheme) before first paint"
date: 2026-08-01
category: design-patterns
module: nanobot-manager/main
problem_type: design_pattern
component: tooling
severity: medium
related_components:
  - nanobot-manager/config
  - nanobot-manager/ui
applies_when:
  - "Picking the initial light/dark theme of a Win32 app to match the user's Windows personalization"
  - "Implementing a 'follow system theme' (theme_mode=0) option on Windows where there is no clean theme API"
  - "Avoiding a one-frame flash of the wrong theme at startup"
  - "Wondering why a theme=light default in a UI template silently defeats your C-side theme detection"
tags: [theme, registry, appsuselighttheme, regqueryvalueex, system-theme, dark-mode, win32, cpp, startup, ui-init]
---

# Reading the Windows system theme from the registry (AppsUseLightTheme) before first paint

## Context

The manager exposes a three-way theme setting — `theme_mode` (`0` = system, `1` = dark, `2` = light in `app_config`) — persisted to `config.json`. For modes 1 and 2 the choice is explicit and easy. The non-obvious case is **mode 0 (system)**: "match whatever Windows is using." Windows has no clean public API for "is the user in dark mode?" — the answer lives in a registry value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`. This documents that lookup, the two places it must run, and a silent-override trap in the UI template that defeats it.

## Guidance

### The registry value is `AppsUseLightTheme`

There is no `GetSystemTheme()` Win32 API. The personalization state is a `DWORD` under the user's `Themes\Personalize` key:

```c
HKEY hk;
DWORD val = 0, sz = sizeof(val);
if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hk) == ERROR_SUCCESS) {
    RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
    RegCloseKey(hk);
}
/* val != 0 → light; val == 0 → dark */
```

- `AppsUseLightTheme` (not `SystemUsesLightTheme`) is the one for *app* theme; `SystemUsesLightTheme` follows the system-wide (taskbar/Start) setting. For matching other apps' chrome, `AppsUseLightTheme` is the right value.
- `val` defaults to `0` (dark) if the read fails (key absent, value absent, access denied). Defaulting to dark is a safe choice on modern Windows; defaulting to light would flash a bright window in dark mode.
- `KEY_READ` is the least privilege needed; this never writes.

### Two call sites, identical logic

The lookup must run in **two** places, and they must stay in sync:

1. **Startup (before `ui_init_with_theme`)** — to pick the *initial* theme so the window never paints the wrong one:
   ```c
   int initial_theme = g_cfg.theme_mode;
   if (initial_theme == 0) {
       /* …the registry read above… */
       initial_theme = val ? 2 : 1;          /* resolve system → concrete mode */
   }
   ui_init_with_theme(initial_theme == 1 ? UI_THEME_DARK : UI_THEME_LIGHT);
   ```
2. **The "system" theme button handler / `apply_theme(0)`** — when the user picks "system" at runtime, resolve it the same way and apply:
   ```c
   static void apply_theme(int mode) {
       /* ... */
       if (mode == 1)      ui_theme_set_mode(UI_THEME_DARK);
       else if (mode == 2) ui_theme_set_mode(UI_THEME_LIGHT);
       else {              /* mode == 0: resolve from registry, same read */
           /* …the registry read… */
           ui_theme_set_mode(val ? UI_THEME_LIGHT : UI_THEME_DARK);
       }
       /* ...persist theme_mode + config_save... */
   }
   ```

If you add a third site (e.g. a WM_SETTINGCHANGE handler to follow live theme switches), extract the read into a helper first — duplicating the `RegOpenKeyExW` block a third time is the kind of drift that quietly desyncs the three.

### Order matters: read before `ui_init_with_theme`

The registry read at startup must complete **before** `ui_init_with_theme(...)`. If the window is created with the wrong theme and you correct it after, the user sees a one-frame flash of the wrong theme — a visible regression on every launch for dark-mode users whose app default is light (or vice versa). Resolving `initial_theme` to a concrete value first means the window is created with the right theme on the first paint.

### The silent-override trap: remove `theme=` from the `.uix` window tag

This is the subtle one. A `.uix` SFC can carry a hard-coded theme attribute on its root element:

```html
<!-- DANGEROUS: silently overrides whatever C decided -->
<window theme="light" ...>
```

If that attribute is present, the framework applies it when the page loads — **after** your C-side `ui_init_with_theme` — and silently wins. The registry read fires, you pick dark, the window opens light anyway, and there is no error anywhere. The original system-theme fix (`52a85d4`) included **removing the hard-coded `theme=light`** from the window tag precisely because it was defeating the detection. If "system theme doesn't work" ever resurfaces, the first thing to check is whether a `theme=` attribute crept back into `dashboard.uix`.

### Persist-on-every-change model

`apply_theme` writes `theme_mode` to `g_cfg` and calls `config_save` on *every* theme button click (including clicking "system"). This matches the rest of the settings page (autostart, tray, language all save immediately). The deliberate tradeoff: there is no "apply theme" button, every click is latching. The cost is a tiny atomic config write per click (cheap, and config_save is atomic — see the credential-manager doc); the benefit is no "did I forget to save my theme?" UX.

## Why This Matters

- **There is no cleaner way on Win32.** `AppsUseLightTheme` is it. Anyone implementing "follow system theme" will rediscover this registry path; documenting it saves the search.
- **The one-frame flash is the regression.** Reading the registry *after* window creation is the easy mistake; the window opens wrong-themed and snaps correct. Ordering the read before `ui_init_with_theme` is the fix, and it's not obvious from the API surface.
- **The `.uix` `theme=` override is invisible.** No log, no error — the detection just quietly doesn't take effect. A future contributor re-adding a default theme to the SFC would reintroduce a "system theme is broken" bug that presents as "the registry read must be wrong."
- **Two sites that must agree.** Startup and the button handler both resolve `mode==0`; if they diverge, clicking "system" gives a different theme than restarting into "system." Keeping the logic duplicated-but-identical (or factored) is the discipline.

## When to Apply

- **Implementing "follow system theme" on Win32** — use `AppsUseLightTheme` under `Themes\Personalize`; there is no cleaner API.
- **Any startup theme decision** — resolve the concrete theme *before* creating/painting the window, not after.
- **Reviewing a `.uix` change that adds a `theme=` attribute to the window** — this will override C-side detection; reject it unless the intent is a hard-coded theme (no "system" support).
- **Adding a live-switch handler (WM_SETTINGCHANGE)** — Windows does broadcast a setting-change message when the user toggles dark mode in Settings; hooking it lets the manager follow the switch without restart. If added, resolve via the same registry read (the message doesn't carry the new value).

## Examples

### Resolve system theme → concrete mode (helper-shaped)

```c
/* Returns 1 (dark) or 2 (light), matching theme_mode's concrete values. */
static int resolve_system_theme(void) {
    HKEY hk;
    DWORD val = 0, sz = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&val, &sz);
        RegCloseKey(hk);
    }
    return val ? 2 : 1;
}

/* startup */
int initial_theme = (g_cfg.theme_mode == 0) ? resolve_system_theme() : g_cfg.theme_mode;
ui_init_with_theme(initial_theme == 1 ? UI_THEME_DARK : UI_THEME_LIGHT);
```

### Avoid the silent override in the SFC

```html
<!-- GOOD: no theme attribute; C-side ui_init_with_theme decides -->
<window :title="..." ...>
```
```html
<!-- BAD: silently overrides C-side detection -->
<window theme="light" ...>
```

## Related

- `docs/solutions/architecture-patterns/config-system.md` — the `theme_mode` field (0/1/2) in `app_config` and its validation.
- `docs/solutions/architecture-patterns/credential-manager-secret-storage.md` — the atomic `config_save` that makes persist-on-every-click safe.
- `nanobot-manager/src/main.cpp` — `apply_theme` (~line 1260) and the startup theme resolution (~line 1590); both contain the registry read.
- `nanobot-manager/ui/dashboard.uix` — the window root element; must NOT carry a `theme=` attribute.
- Commit `52a85d4` — the original fix (`feat: persist theme mode to config and fix system theme on startup`), which included both the registry read and removing the hard-coded `theme=light`.
