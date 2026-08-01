---
title: "i18n convention: embed zh/en strings as literals in main.cpp, not external .lang files"
date: 2026-08-01
category: conventions
module: nanobot-manager/main
problem_type: convention
component: documentation
severity: medium
related_components:
  - nanobot-manager/ui
applies_when:
  - "Adding or changing any user-visible UI string (label, button, status, tooltip) in the manager"
  - "Looking for the .lang / language files and finding none"
  - "Considering adding external .lang resource files to the project"
  - "Wiring a new status_* i18n key and wanting the $t() and C++ push to stay in sync"
tags: [i18n, localization, embedded-strings, language-strings, ui-page-load-language-string, core-ui, convention, cpp]
---

# i18n convention: embed zh/en strings as literals in main.cpp, not external .lang files

## Context

The manager is bilingual (zh / en). New contributors looking for where UI copy lives will, by default, hunt for `.lang` / `.json` / resource files — that's how most UI frameworks do i18n. **There are none in this project.** Both language blobs are embedded as C string literals inside `main.cpp` and handed to core-ui's `ui_page_load_language_string` at startup. UI copy that changes often needs edits in *two* places (the `.uix` `$t('key')` reference and the `main.cpp` literal), and for status text there is a third concern — keeping the strings that C++ pushes directly in sync with the strings the template looks up via `$t()`.

This convention record exists so contributors don't (a) invent `.lang` files that the loader never reads, or (b) add a status string in one place but not the other.

## Guidance

### core-ui's i18n API: `ui_page_load_language_string`

core-ui offers `ui_page_load_language_string(page, lang_code, string)` — give it a language code and a newline-separated `key=value\nkey=value` string, and `$t('key')` lookups in the `.uix` resolve to those values for that language. There is no file loading required by the app; the *string* is the input. The manager uses this to inject both zh and en blobs at startup.

### Two embedded blobs, one per language

UI strings (labels, buttons, settings rows) live as multi-line C string literals in `main.cpp`, structured as `key=zh_or_en\n`:

```c
/* zh block — Chinese as byte-escaped UTF-8 OR raw UTF-8 source */
const char* zh =
    "app_title=nanobot manager\n"
    "chat_empty=开始对话\n"
    "btn_start=启动\n"
    "btn_stop=停止\n"
    "btn_refresh=刷新\n"
    "settings_network=网络\n"
    "setting_webui_port=WebUI 端口\n"
    /* ... */
    ;
/* en block — parallel keys */
const char* en =
    "app_title=nanobot manager\n"
    "chat_empty=Start a conversation\n"
    "btn_start=Start\n"
    /* ... */
    ;
ui_page_load_language_string(g_page, "zh", zh);
ui_page_load_language_string(g_page, "en", en);
```

Every key in the zh block **must** appear in the en block and vice versa. The `.uix` references these via `$t('btn_start')`, `$t('settings_network')`, etc. Adding a visible label means editing both the `.uix` (a `$t('new_key')`) and *both* language blobs in `main.cpp`.

### `/utf-8` lets Chinese be raw, but byte-escaped is the legacy form

The project compiles with `/utf-8` (`CMakeLists.txt`), so raw UTF-8 Chinese in the source compiles directly. Some strings still appear as `\xe7\xbd\x91\xe7\xbb\x9c` byte-escaped literals — that form predates the `/utf-8` switch and is harmless. Either form is acceptable; prefer raw UTF-8 for new strings (it's readable in the editor and in diffs).

### Status strings: a single source that $t and C++ both read (anti-drift)

Status text (running, stopped, starting…) has an extra failure mode: it is both `$t('status_running')` in the template *and* pushed directly from C++ via `page_set`. If those drifted, the template would show one wording and the C++ push another.

The manager solves this with a `status_i18n_entry` table that is the **only** wording source for `status_*` keys:

```c
/* main.cpp */
typedef struct { const char* key; const char* zh; const char* en; } status_i18n_entry;
static const status_i18n_entry g_status_i18n[] = {
    { "status_running",         "运行中",            "Running" },
    { "status_stopped",         "已停止",            "Stopped" },
    { "status_starting",        "启动中…",           "Starting…" },
    /* ... */
};
```

- `t_status(key, lang)` looks up a key for C++ to push via `page_set` / `set_status_i18n`.
- `append_status_i18n_lang(lang)` **generates** the `status_*` portion of the language blob passed to `ui_page_load_language_string` — so the `$t()` lookups and the C++ pushes are reading the *same table*. They cannot drift, because there is only one set of strings.

**When adding a new status:** add one row to `g_status_i18n`. It is automatically available to both `$t()` (via the generated blob) and C++ push (via `t_status`). Do not hand-write the new `status_*` line into the zh/en string literals — that would create a second source.

### Non-status keys live in the static blobs; status keys live in the table

The split is deliberate:
- **Static keys** (labels, buttons, settings headers) are referenced only from the `.uix` via `$t()`. They live in the zh/en string literals, one entry per language.
- **Status keys** are referenced from *both* the template and C++. They live in `g_status_i18n` and are generated into the blob so both sides agree.

Don't move static keys into the table "for consistency" — the table exists specifically to solve the dual-reference problem, and static keys don't have it.

### Don't invent `.lang` files

If a guide or habit suggests creating `assets/zh.lang` / `en.lang`: **don't.** The loader is `ui_page_load_language_string(page, code, string)` — it takes a string, not a path. There is no code path that reads a `.lang` file. Adding one would be dead weight and would not be loaded.

## Why This Matters

- **Avoids the missing-files hunt.** The single most common i18n onboarding confusion here is "where are the language files?" Answering it once, in the convention doc, saves every future contributor the search.
- **The status anti-drift table is load-bearing.** Without it, `$t('status_running')` in the template and the C++ push of the running string would be two independent edits, and they would eventually disagree (one says "Running", the other "Active"). The table makes drift impossible by construction.
- **Edits-in-two-places is the recurring cost.** A label change touches the `.uix` key reference (rarely) and *both* language blobs (always). Forgetting the second language leaves a key unresolved — `$t()` falls back to the key itself, so the UI shows the raw key string. The fix is mechanical (edit zh and en), but the discipline is "always both."
- **Keeping it in-source keeps the build simple.** No resource embedding step, no install rule for language files, no missing-file failure mode. The strings compile into the binary.

## When to Apply

- **Adding any user-visible string** — add the key to the `.uix` (`$t('key')`), then add `key=zh` to the zh blob and `key=en` to the en blob in `main.cpp`. Both languages, every time.
- **Adding a status string** (referenced from both template and C++) — add one row to `g_status_i18n`; do not hand-edit the zh/en blobs for it.
- **Considering external `.lang` / `.json` language files** — don't; the loader takes in-source strings. If the string count ever makes in-source unwieldy, that's a deliberate refactor decision, not a default.
- **Seeing `$t('some_key')` render as the literal `some_key`** — the key is missing from the active language blob; add it to both blobs.
- **Reviewing an i18n PR** — confirm: every new key exists in zh *and* en; status keys went into the table, not the blobs; no `.lang` file was added.

## Examples

### Add a non-status label (both blobs)

```c
/* zh */
"setting_advanced=高级\n"
/* en */
"setting_advanced=Advanced\n"
```
```html
<!-- dashboard.uix -->
<div>{{ $t('setting_advanced') }}</div>
```

### Add a status (table only)

```c
{ "status_restarting", "重启中…", "Restarting…" },
```
Now both `$t('status_restarting')` and `t_status("status_restarting", lang)` work, from the same string.

### Wrong: inventing a .lang file

```c
/* DO NOT DO THIS — the loader never reads files */
ui_page_load_language_file(g_page, "assets/en.lang");  /* no such API in this project */
```

## Related

- `docs/solutions/reference/core-ui-overview.md` — core-ui's i18n API surface, including the absence of `ui_text_input_on_changed` and the `ui_page_load_language_string` contract.
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — references the embedded-literal convention for its settings-section i18n; this doc is the canonical statement.
- `nanobot-manager/src/main.cpp` — the zh/en string blobs, `g_status_i18n` table, `t_status` / `append_status_i18n_lang`, and the `ui_page_load_language_string` calls.
- `nanobot-manager/ui/dashboard.uix` — the `$t('key')` references that resolve against these blobs.
- Commit `96d8b1b` — `feat(ui): add i18n with Chinese/English language switching` (the origin of this convention).
