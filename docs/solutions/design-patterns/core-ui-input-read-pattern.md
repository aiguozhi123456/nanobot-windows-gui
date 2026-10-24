---
title: "Reading core-ui text/number input values from C via an Apply button and ui_page_get_json"
date: 2026-07-11
last_updated: 2026-08-01
category: design-patterns
module: nanobot-manager/settings
problem_type: design_pattern
component: tooling
severity: low
related_components:
  - nanobot-manager/ui
  - nanobot-manager/chat
  - nanobot-manager/credential
applies_when:
  - "Exposing an editable text or number setting in a core-ui page and persisting it from C"
  - "core-ui lacks a per-keystroke ui_text_input_on_changed C callback, so changes cannot be pushed live to C"
  - "You need to read a TextInput value (port, path) into C state for validation and config_save"
  - "A toggle (ui_toggle_on_changed) applies per-change but a text/number input cannot"
tags: [core-ui, settings, text-input, ui-page-get-json, apply-button, win32, cpp, read-on-click]
---

# Reading core-ui text/number input values from C via an Apply button and ui_page_get_json

## Context

The nanobot-manager is a native Win32/C++ app built on the `core-ui` submodule — a reactive SFC framework with a Vue3-Options-API flavor (`data()` with reactive fields, `v-model` two-way binding, `$t('key')` i18n lookups). Its job is to manage a local Python LLM gateway (nanobot) and chat with it over a WebSocket endpoint on the gateway's WebUI port.

The settings page exposes the two gateway **port** fields (`health_check_port` default `18790`, `webui_port` default `8765`) as `v-model`'d `<input type="number">` rows, committed through a single **Apply button** that reads both inputs at once. The reusable insight is not the section itself but the *input-read mechanism* the Apply button forces you into — and the *apply-immediately* settings model it slots into.

> **Migration note (2026-08-01):** This doc previously used a `webui_auth` text input as its flagship example. That field has been **removed from the UI and from `app_config` serialization** — the bootstrap secret now lives in **Windows Credential Manager**, surfaced via Manage/Clear buttons rather than a text input. The example below has been updated to reflect the current port-only Network section. For the secret-storage architecture (Credential Manager, migrate-verify-delete, atomic config save), see [`architecture-patterns/credential-manager-secret-storage.md`](../architecture-patterns/credential-manager-secret-storage.md). The generic `ui_page_get_json` read-on-click pattern documented here is unchanged and still load-bearing for both port inputs.

## Guidance

### core-ui has no per-change C callback for text inputs

This is the load-bearing constraint. core-ui exposes a change callback for toggles:

```c
ui_toggle_on_changed(w, cb, ud);   /* cb signature: void cb(UiWidget, int val, void*) */
```

…but there is **no** `ui_text_input_on_changed`. (Verified in `core-ui/include/ui_core.h`: `on_changed` exists for checkbox/slider/toggle/combobox only; the text-input API surface is `get_text`/`set_text`/`set_read_only`.) Text and number `<input>`s update their `v-model`-bound reactive field on the JS/script side, but C has no notification when that happens. You cannot, in the general case, react to a user typing into an input from C.

### Read the reactive field back on demand: `ui_page_get_json`

The established workaround — already used by the chat send flow to read the draft input (`chat_ui.c:208`) — is to read the reactive field back from C when you actually need it:

```c
char* raw = ui_page_get_json(g_page, "draft");
if (!raw) return;
cJSON* j = cJSON_Parse(raw);
ui_page_free(raw);                 /* caller frees the malloc'd return */
const char* text = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
```

`ui_page_get_json` returns a **malloc'd UTF-8 JSON string** representing the field's current value. Two consequences fall out of that:

1. You must `cJSON_Parse` it to get the value — it is JSON, not a raw string.
2. You must `ui_page_free(raw)` it on every path. The caller owns the allocation.

### An Apply button is the natural fit

Because C can't react per-keystroke, the cleanest UX for a *discrete* value the user wants to commit deliberately (a port number) is a button that reads the inputs on click. This is exactly the Network section pattern:

- Reactive fields in `data()` (`ui/dashboard.uix`):
  ```js
  healthPort: "18790",
  webuiPort: "8765",
  ```
- Two `v-model` number inputs plus an Apply button:
  ```html
  <input id="health_port_input" v-model="healthPort" type="number" min="1" max="65535"
         class="port-input" placeholder="18790"/>
  <input id="webui_port_input" v-model="webuiPort" type="number" min="1" max="65535"
         class="port-input" placeholder="8765"/>
  ...
  <div id="btn_apply_network" class="apply-btn">
    <label class="apply-btn-label">{{ $t('setting_apply') }}</label>
  </div>
  ```
- A click handler that reads both ports, validates, saves atomically, and re-syncs (`src/main.cpp`):

```c
static void on_apply_network(UiWidget w, void* ud) {
    (void)w; (void)ud;
    int new_health_port = 0, new_webui_port = 0;
    /* read_port_input wraps ui_page_get_json + the dual-parse below and
       restore-on-invalid. Returns 0 if either port failed validation. */
    if (!read_port_input("healthPort", &new_health_port) ||
        !read_port_input("webuiPort",  &new_webui_port)) {
        restore_health_port_input(); restore_webui_port_input();
        return;
    }
    /* The gateway health port cannot be changed while the gateway is running. */
    if (new_health_port != g_cfg.health_check_port && !can_edit_gateway_port()) {
        restore_health_port_input();
        /* ...MessageBox: stop the gateway before changing its port... */
        return;
    }
    /* Build a proposed config, save atomically, only then mutate g_cfg. */
    app_config proposed = g_cfg;
    proposed.health_check_port = new_health_port;
    proposed.webui_port        = new_webui_port;
    if (config_save(&proposed) != 0) {
        /* ...MessageBox with config_get_last_error(); inputs kept, not saved... */
        return;
    }
    g_cfg = proposed;
    /* Re-sync the chat subsystem's WebUI target if it changed. */
    if (webui_changed) {
        chat_set_endpoint(g_cfg.webui_port, /*secret from Credential Manager*/);
        chat_reconnect_async();
    }
}
```

> The bootstrap secret is **not** read here — it lives in Windows Credential Manager and is managed through separate Manage/Clear buttons (`on_manage_credential` / `on_clear_credential` → `credential_*`). The Network Apply only commits the two ports. See [`credential-manager-secret-storage.md`](../architecture-patterns/credential-manager-secret-storage.md) for why the secret never enters a `v-model` field or `config.json`.

### Parse the port as string *or* number

The port input has `type="number"`, but `ui_page_get_json` on a `v-model`-bound number input does not guarantee a JSON number — depending on the framework's serialization path it may come back as a quoted JSON string. Defensive dual-parse covers both:

```c
if (jp && cJSON_IsString(jp))      new_port = atoi(jp->valuestring);
else if (jp && cJSON_IsNumber(jp)) new_port = jp->valueint;
```

Skip this and you get a silent 0 the day the serialization changes.

### Wire the button through a mount hook, not a one-shot find

The `.uix` widget id must match the mount registration exactly:

```c
/* main.cpp — registration alongside the other settings buttons */
ui_page_on_widget_mount(g_page, "btn_apply_network", on_btn_apply_network_mount, NULL);

/* trampoline binds the click */
static void on_btn_apply_network_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_apply_network, NULL);
}
```

Use `ui_page_on_widget_mount` rather than `ui_widget_find_by_id` because the settings panel can be re-rendered (it lives behind a `v-if`); the mount hook fires fresh on every (re)mount, so the click binding survives navigation and stays correct.

### The settings page is apply-immediately

Every other setting on the page — autostart toggle, tray toggle, hotkey toggle, theme, language, path — already mutates `g_cfg` and calls `config_save` *immediately* in its own mount hook / handler. There is no batch Save button. The Network section follows that same model: it just bundles the two ports behind one Apply button because the gateway health port and the WebUI port must be read *together* before committing (reading and applying them separately could leave a half-applied state in between, and changing the health port while the gateway is running must be rejected as one unit). This is the one place the page deviates from "one control, one immediate apply" — and the deviation is deliberate.

### Push initial values C→UI at startup

Reactive fields have defaults in `data()`, but the source of truth at startup is `g_cfg`. Push the configured values into the fields alongside the rest of the settings init block:

```c
/* Gateway + WebUI port initial values (numbers → wide) */
{
    wchar_t hpw[16], wpw[16];
    swprintf_s(hpw, _countof(hpw), L"%d",
               g_cfg.health_check_port > 0 ? g_cfg.health_check_port : 18790);
    swprintf_s(wpw, _countof(wpw), L"%d",
               g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
    ui_page_set_text(g_page, "healthPort", hpw);
    ui_page_set_text(g_page, "webuiPort",  wpw);
}
```

`ui_page_set_text` takes a `wchar_t*`; format numbers wide with `swprintf_s`. The credential status (`hasCredential` / `credentialStatus`) is pushed separately from the Credential Manager read — it is a boolean + label, never the secret itself.

### i18n: zh/en UTF-8 literals in src/i18n.c

UI copy lives in **two** places and both must change together. The `.uix` references keys via `$t('settings_network')`, `$t('setting_gateway_port')`, `$t('setting_webui_port')`, `$t('setting_apply')` (plus their `_sub` variants). The actual strings are defined in `src/i18n.c` (`s_prefix_zh` / `s_prefix_en`), with Chinese embedded as raw UTF-8 (or `\xXX` byte-escaped UTF-8 in older entries — project convention, not external `.lang` files):

```c
/* zh block */
"settings_network=\xe7\xbd\x91\xe7\xbb\x9c\n"
"setting_gateway_port=Gateway \xe7\xab\xaf\xe5\x8f\xa3\n"
"setting_webui_port=WebUI \xe7\xab\xaf\xe5\x8f\xa3\n"
"setting_apply=\xe5\xba\x94\xe7\x94\xa8\n"
...
/* en block */
"settings_network=Network\n"
"setting_gateway_port=Gateway Port\n"
"setting_webui_port=WebUI Port\n"
"setting_apply=Apply\n"
```

These are loaded via `ui_page_load_language_string`. Adding a visible label means editing both files (and both languages — see [`conventions/i18n-embedded-literals-not-lang-files.md`](../conventions/i18n-embedded-literals-not-lang-files.md)).

### Known limitation: no password masking

`core-ui` does not implement `type="password"` — its input factory falls through to a plain `TextInput` with no masked draw path. This is exactly **why the bootstrap secret is not a text input at all**: it lives in Windows Credential Manager and is managed via Manage/Clear buttons, so the value is never rendered in the page. Don't add a secret as a `v-model` text input expecting `type="password"` to mask it; route it through Credential Manager instead.

## Why This Matters

### Why a button instead of live binding

With no `ui_text_input_on_changed` callback, the only ways to observe a text input from C are (a) poll on a timer — wasteful and laggy, or (b) read on a discrete event. A button is the cleanest discrete event and matches user expectations for a destructive/latching action like changing a port or a secret. Live-binding each keystroke into `g_cfg` and `config_save` would also thrash the config file and trigger reconnects mid-typing.

### Why dual-parse the port

`ui_page_get_json` is a serialization boundary, and serialization of a number-as-string is legal JSON. Hard-coding `cJSON_IsNumber` works today but breaks silently if the framework's number-input binding starts quoting the value. The two-line dual-parse is cheap insurance against a class of bug that presents as "the port always resets to 8765."

### Why free on every path

`ui_page_get_json` returns malloc'd memory and documents (via the chat draft precedent) that the caller frees it. Early returns after the read — the invalid-port bail, the empty-auth skip — must still `ui_page_free` what they parsed, or the app leaks on every interaction. The handler above frees `raw_port` before the validation bail and `raw_auth` before the copy.

### Why restore-on-invalid instead of error UI

The rest of the app pushes C→UI values via `ui_page_set_text`; there's no toast/snackbar infrastructure. Restoring the last good value into the input on validation failure is consistent with that model, needs no new UI, and gives immediate feedback ("my edit reverted, so it was rejected"). A separate error label would require new markup, a new reactive field, and i18n strings for the error text.

### Why the port Apply is UI-only (but the secret migration wasn't)

The two **port** fields were already in the `app_config` struct, already persisted, already consumed. Surfacing them was pure UI + wiring — which is the payoff of having plumbed config through to its consumers first. The **secret**, by contrast, was a real backend change: it moved out of `app_config`/`config.json` entirely into Credential Manager, with a migrate-verify-delete path and an elevated scrub helper. The lesson is the split itself: ports are config (apply-on-click via this pattern), secrets are credentials (Credential Manager + buttons, not a text input).

## When to Apply

- **Adding any text or number input to a core-ui settings page** — there is no change callback, so you need either an Apply button (read on click) or an external trigger (e.g. a Send button) to pull the value into C. Use the Apply button when the value commits a discrete config change.
- **Reading any `v-model`-bound field from C in core-ui** — always go through `ui_page_get_json` + `cJSON_Parse` + `ui_page_free`. This is the framework's intended C↔script value bridge.
- **Surfacing an already-plumbed config field in the UI** — if the backend loads/saves/consumes it, the change is UI + wiring only; don't touch `config.c`.
- **Any reconnect-triggering setting** (port, auth, endpoint) — batch the related fields behind one Apply so you reconnect once from a fully consistent state, not per-field.
- **Adding a settings section that must survive panel re-render** — register click handlers via `ui_page_on_widget_mount(id, trampoline, ud)`, not `ui_widget_find_by_id`.

Avoid this pattern when a toggle will do: toggles *do* have `ui_toggle_on_changed`, so a binary setting should apply immediately with no button (as autostart/tray already do).

## Examples

### Minimal read-on-click from a v-model input

```c
static void on_apply(UiWidget w, void* ud) {
    (void)w; (void)ud;
    char* raw = ui_page_get_json(g_page, "myField");
    if (!raw) return;
    cJSON* j = cJSON_Parse(raw);
    ui_page_free(raw);                       /* free the JSON envelope */
    const char* v = (j && cJSON_IsString(j)) ? j->valuestring : NULL;
    if (v) { /* use v */ }
    cJSON_Delete(j);                         /* free the parsed tree */
}

static void on_btn_mount(UiPage p, UiWidget w, void* ud) {
    (void)p; (void)ud;
    ui_widget_on_click(w, on_apply, NULL);
}

/* at init: */
ui_page_on_widget_mount(g_page, "btn_apply", on_btn_mount, NULL);
```

### Number input: handle both JSON shapes

```c
int n = 0;
char* raw = ui_page_get_json(g_page, "countField");
if (raw) {
    cJSON* j = cJSON_Parse(raw);
    if (j && cJSON_IsString(j))      n = atoi(j->valuestring);
    else if (j && cJSON_IsNumber(j)) n = j->valueint;
    cJSON_Delete(j);
    ui_page_free(raw);
}
```

### Validation + restore on failure

```c
if (n < 1 || n > 65535) {
    wchar_t buf[16];
    swprintf_s(buf, _countof(buf), L"%d",
               g_cfg.last_good > 0 ? g_cfg.last_good : DEFAULT);
    ui_page_set_text(g_page, "countField", buf);   /* revert visible value */
    return;                                         /* do NOT save */
}
```

### The reactive field + input pair in `.uix`

```js
// data()
healthPort: "18790",
webuiPort: "8765",
```
```html
<input v-model="healthPort" type="number" min="1" max="65535" class="port-input"/>
<input v-model="webuiPort" type="number" min="1" max="65535" class="port-input"/>
<div id="btn_apply_network" class="apply-btn">
  <label class="apply-btn-label">{{ $t('setting_apply') }}</label>
</div>
```

### Pushing startup values C→UI

```c
{
    wchar_t hpw[16], wpw[16];
    swprintf_s(hpw, _countof(hpw), L"%d",
               g_cfg.health_check_port > 0 ? g_cfg.health_check_port : 18790);
    swprintf_s(wpw, _countof(wpw), L"%d",
               g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
    ui_page_set_text(g_page, "healthPort", hpw);          /* number → wide */
    ui_page_set_text(g_page, "webuiPort",  wpw);          /* number → wide */
}
```

## Related

- `docs/solutions/architecture-patterns/credential-manager-secret-storage.md` — **why the bootstrap secret is not a text input**: it lives in Windows Credential Manager with migrate-verify-delete and an elevated scrub helper. Read this before adding any secret field.
- `docs/solutions/architecture-patterns/native-websocket-chat-integration.md` — the chat subsystem that *consumes* `webui_port` via `chat_set_endpoint` / `chat_ui_set_gateway`; the `ui_page_get_json` read pattern originates there (the draft input read at `chat_ui.c:208`).
- `docs/solutions/conventions/i18n-embedded-literals-not-lang-files.md` — the project's i18n convention (embedded literals in `src/i18n.c`, not `.lang` files) referenced by the i18n guidance above.
- `nanobot-manager/src/config.h` — `app_config` struct (`webui_port`, `health_check_port`); the legacy `legacy_webui_auth` field is migration-only and never serialized.
- `nanobot-manager/src/main.cpp` — `on_apply_network` handler, `read_port_input` / `restore_*_port_input` helpers, the credential Manage/Clear handlers, the i18n consumers (`ui_page_load_language_string` calls + `t_status` wrapper), and the startup value-push block. Also `page_set` / `page_set_int` helpers.
- `nanobot-manager/src/chat_ui.c` — the original `ui_page_get_json` + `ui_page_free` read-on-action pattern (the send handler reading `draft`).
- `nanobot-manager/ui/dashboard.uix` — the Network settings section markup and CSS (`.setting-col`, `.port-input`, `.apply-row`, `.apply-btn`).
- `core-ui/docs/uix-ai-guide.md` — core-ui SFC reference (`data()`, `v-model`, `$t()`); `core-ui/include/ui_core.h` is the authoritative source for the missing `ui_text_input_on_changed` and the `type="password"` limitation.
