---
title: "Reading core-ui text/number input values from C via an Apply button and ui_page_get_json"
date: 2026-07-11
category: design-patterns
module: nanobot-manager/settings
problem_type: design_pattern
component: tooling
severity: low
related_components:
  - nanobot-manager/ui
  - nanobot-manager/chat
applies_when:
  - "Exposing an editable text or number setting in a core-ui page and persisting it from C"
  - "core-ui lacks a per-keystroke ui_text_input_on_changed C callback, so changes cannot be pushed live to C"
  - "You need to read a TextInput value (port, secret, path) into C state for validation and config_save"
  - "A toggle (ui_toggle_on_changed) applies per-change but a text/number input cannot"
tags: [core-ui, settings, text-input, ui-page-get-json, apply-button, win32, cpp, read-on-click]
---

# Reading core-ui text/number input values from C via an Apply button and ui_page_get_json

## Context

The nanobot-manager is a native Win32/C++ app built on the `core-ui` submodule — a reactive SFC framework with a Vue3-Options-API flavor (`data()` with reactive fields, `v-model` two-way binding, `$t('key')` i18n lookups). Its job is to manage a local Python LLM gateway (nanobot) and chat with it over a WebSocket endpoint on the gateway's WebUI port.

Two gateway configuration fields were already fully plumbed in the backend:

- `webui_port` (default `8765`) — the WebSocket port
- `webui_auth` (default `""`) — the gateway's `tokenIssueSecret`, sent as the bootstrap handshake secret

Both were defined in `config.h` (`app_config` struct), loaded/validated/saved in `config.c`, and consumed by the chat subsystem via `chat_set_endpoint(port, auth)` and `chat_ui_set_gateway(health_fn, port, auth)`. But they were **never exposed in the settings UI** — users could only change them by hand-editing `config.json`.

This document covers the pattern used to surface those two fields in the settings page: a new "Network / 网络" section with a port input, an auth input, and an **Apply button**. The reusable insight is not the section itself but the *input-read mechanism* the Apply button forces you into — and the *apply-immediately* settings model it slots into.

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

Because C can't react per-keystroke, the cleanest UX for a *discrete* value the user wants to commit deliberately (a port number, a secret) is a button that reads both inputs on click. This is exactly the Network section pattern:

- Two reactive fields in `data()` (`ui/dashboard.uix`):
  ```js
  webuiPort: "8765",
  webuiAuth: "",
  ```
- Two `v-model` inputs plus an Apply button:
  ```html
  <input id="webui_port_input" v-model="webuiPort" type="number" min="1" max="65535"
         class="port-input" placeholder="8765"/>
  <input id="webui_auth_input" v-model="webuiAuth" class="auth-input" placeholder="(optional)"/>
  ...
  <div id="btn_apply_network" class="apply-btn">
    <label class="apply-btn-label">{{ $t('setting_apply') }}</label>
  </div>
  ```
- A click handler that reads both fields, validates, saves, and re-syncs (`src/main.cpp`):

```c
static void on_apply_network(UiWidget w, void* ud) {
    (void)w; (void)ud;
    /* Read webuiPort — a v-model'd <input type="number"> may serialize as
       a JSON string OR a JSON number; parse both. */
    int new_port = 0;
    char* raw_port = ui_page_get_json(g_page, "webuiPort");
    if (raw_port) {
        cJSON* jp = cJSON_Parse(raw_port);
        if (jp && cJSON_IsString(jp))
            new_port = atoi(jp->valuestring);
        else if (jp && cJSON_IsNumber(jp))
            new_port = jp->valueint;
        cJSON_Delete(jp);
        ui_page_free(raw_port);
    }
    if (new_port < 1 || new_port > 65535) {
        /* Invalid: restore the current value and bail — don't save garbage. */
        wchar_t wpw[16];
        swprintf_s(wpw, _countof(wpw), L"%d",
                   g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
        ui_page_set_text(g_page, "webuiPort", wpw);
        return;
    }
    /* Read webuiAuth (optional). */
    char new_auth[128] = {0};
    char* raw_auth = ui_page_get_json(g_page, "webuiAuth");
    if (raw_auth) {
        cJSON* ja = cJSON_Parse(raw_auth);
        if (ja && cJSON_IsString(ja) && ja->valuestring) {
            strncpy(new_auth, ja->valuestring, sizeof(new_auth) - 1);
            new_auth[sizeof(new_auth) - 1] = '\0';
        }
        cJSON_Delete(ja);
        ui_page_free(raw_auth);
    }
    g_cfg.webui_port = new_port;
    strncpy(g_cfg.webui_auth, new_auth, sizeof(g_cfg.webui_auth) - 1);
    g_cfg.webui_auth[sizeof(g_cfg.webui_auth) - 1] = '\0';
    config_save(&g_cfg);
    /* Sync into the chat subsystem and reconnect. */
    chat_ui_set_gateway(health_running_thunk, g_cfg.webui_port, g_cfg.webui_auth);
    chat_set_endpoint(g_cfg.webui_port, g_cfg.webui_auth);
    chat_connect();
}
```

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

Every other setting on the page — autostart toggle, tray toggle, theme, language, path — already mutates `g_cfg` and calls `config_save` *immediately* in its own mount hook / handler. There is no batch Save button. The Network section follows that same model: it just bundles two fields behind one Apply button because the port and the auth must be read *together* before reconnecting (reading and applying them separately would cause two reconnects and a half-applied state in between). This is the one place the page deviates from "one control, one immediate apply" — and the deviation is deliberate.

### Push initial values C→UI at startup

Reactive fields have defaults in `data()`, but the source of truth at startup is `g_cfg`. Push the configured values into the fields alongside the rest of the settings init block:

```c
/* WebUI port + auth initial values */
{
    wchar_t wpw[16];
    swprintf_s(wpw, _countof(wpw), L"%d",
               g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
    ui_page_set_text(g_page, "webuiPort", wpw);
    page_set("webuiAuth", g_cfg.webui_auth[0] ? g_cfg.webui_auth : "");
}
```

`ui_page_set_text` takes a `wchar_t*`; the `page_set` helper is the UTF-8→wide wrapper used elsewhere in the file. Use wide-string formatting for numbers, `page_set` for UTF-8 strings.

### i18n: inline UTF-8 byte-escaped strings in main.cpp

UI copy lives in **two** places and both must change together. The `.uix` references keys via `$t('settings_network')`, `$t('setting_webui_port')`, `$t('setting_webui_auth')`, `$t('setting_apply')` (plus their `_sub` variants). The actual strings are defined in `main.cpp` in zh and en blocks, with Chinese embedded as `\xXX` UTF-8 byte-escaped literals (project convention — not external `.lang` files):

```c
/* zh block — Chinese as byte-escaped UTF-8 */
"settings_network=\xe7\xbd\x91\xe7\xbb\x9c\n"
"setting_webui_port=WebUI \xe7\xab\xaf\xe5\x8f\xa3\n"
...
/* en block */
"settings_network=Network\n"
"setting_webui_port=WebUI Port\n"
"setting_webui_auth=Auth Secret\n"
"setting_apply=Apply\n"
```

These are loaded via `ui_page_load_language_string`. Adding a visible label means editing both files.

### Known limitation: no password masking

`core-ui` does not implement `type="password"` — its input factory falls through to a plain `TextInput` with no masked draw path. The auth secret is rendered in plaintext. This is a framework limitation, not a choice in this code. Flag it wherever a secret input is added; do not assume `type="password"` will hide the value.

### No backend changes were needed

`config.c` and `chat.c` already fully supported both fields. The entire change was UI markup + CSS in `dashboard.uix` and i18n + wiring in `main.cpp`. When a backend is already correct, the work of "exposing" a setting is purely the read-on-click pattern above.

## Why This Matters

### Why a button instead of live binding

With no `ui_text_input_on_changed` callback, the only ways to observe a text input from C are (a) poll on a timer — wasteful and laggy, or (b) read on a discrete event. A button is the cleanest discrete event and matches user expectations for a destructive/latching action like changing a port or a secret. Live-binding each keystroke into `g_cfg` and `config_save` would also thrash the config file and trigger reconnects mid-typing.

### Why dual-parse the port

`ui_page_get_json` is a serialization boundary, and serialization of a number-as-string is legal JSON. Hard-coding `cJSON_IsNumber` works today but breaks silently if the framework's number-input binding starts quoting the value. The two-line dual-parse is cheap insurance against a class of bug that presents as "the port always resets to 8765."

### Why free on every path

`ui_page_get_json` returns malloc'd memory and documents (via the chat draft precedent) that the caller frees it. Early returns after the read — the invalid-port bail, the empty-auth skip — must still `ui_page_free` what they parsed, or the app leaks on every interaction. The handler above frees `raw_port` before the validation bail and `raw_auth` before the copy.

### Why restore-on-invalid instead of error UI

The rest of the app pushes C→UI values via `ui_page_set_text`; there's no toast/snackbar infrastructure. Restoring the last good value into the input on validation failure is consistent with that model, needs no new UI, and gives immediate feedback ("my edit reverted, so it was rejected"). A separate error label would require new markup, a new reactive field, and i18n strings for the error text.

### Why this was a UI-only change

Both fields were already in the `app_config` struct, already persisted, already consumed by the chat client. The cost of "make this configurable from the UI" was zero backend work — which is the payoff of having plumbed config through to its consumers first. When you find yourself adding a UI for an existing field, the read-on-click Apply pattern is the whole job.

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
webuiPort: "8765",
webuiAuth: "",
```
```html
<input v-model="webuiPort" type="number" min="1" max="65535" class="port-input"/>
<input v-model="webuiAuth" class="auth-input" placeholder="(optional)"/>
<div id="btn_apply_network" class="apply-btn">
  <label class="apply-btn-label">{{ $t('setting_apply') }}</label>
</div>
```

### Pushing startup values C→UI

```c
{
    wchar_t wpw[16];
    swprintf_s(wpw, _countof(wpw), L"%d",
               g_cfg.webui_port > 0 ? g_cfg.webui_port : 8765);
    ui_page_set_text(g_page, "webuiPort", wpw);          /* number → wide */
    page_set("webuiAuth", g_cfg.webui_auth[0] ? g_cfg.webui_auth : ""); /* utf8 → wide */
}
```

## Related

- `docs/solutions/architecture-patterns/native-websocket-chat-integration.md` — the chat subsystem that *consumes* `webui_port`/`webui_auth` via `chat_set_endpoint` / `chat_ui_set_gateway`; the `ui_page_get_json` read pattern originates there (the draft input read at `chat_ui.c:208`).
- `nanobot-manager/src/config.h` — `app_config` struct (`webui_port`, `webui_auth[128]`); already load/save/free in `config.c`.
- `nanobot-manager/src/main.cpp` — `on_apply_network` handler, `on_btn_apply_network_mount` trampoline, mount registration, i18n zh/en blocks, and the startup value-push block. Also `page_set` / `page_set_int` helpers.
- `nanobot-manager/src/chat_ui.c` — the original `ui_page_get_json` + `ui_page_free` read-on-action pattern (the send handler reading `draft`).
- `nanobot-manager/ui/dashboard.uix` — the Network settings section markup and CSS (`.setting-col`, `.port-input`, `.auth-input`, `.apply-row`, `.apply-btn`).
- `core-ui/docs/uix-ai-guide.md` — core-ui SFC reference (`data()`, `v-model`, `$t()`); `core-ui/include/ui_core.h` is the authoritative source for the missing `ui_text_input_on_changed` and the `type="password"` limitation.
