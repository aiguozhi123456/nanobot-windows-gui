---
title: "i18n convention: embed zh/en strings as literals in src/i18n.c, not external .lang files"
date: 2026-08-01
last_updated: 2026-10-27
category: conventions
module: nanobot-manager/i18n
problem_type: convention
component: documentation
severity: medium
related_components:
  - nanobot-manager/ui
  - nanobot-manager/main
applies_when:
  - "Adding or changing any user-visible UI string (label, button, status, tooltip) in the manager"
  - "Looking for the .lang / language files and finding none"
  - "Considering adding external .lang resource files to the project"
  - "Wiring a new status_* i18n key and wanting the $t() and C++ push to stay in sync"
tags: [i18n, localization, embedded-strings, language-strings, ui-page-load-language-string, core-ui, convention, cpp]
---

# i18n convention: embed zh/en strings as literals in src/i18n.c, not external .lang files

## Context

The manager is bilingual (zh / en). New contributors looking for where UI copy lives will, by default, hunt for `.lang` / `.json` / resource files — that's how most UI frameworks do i18n. **There are none in this project.** Each language's **Language Table** (the `key=value\n` string handed to core-ui) is assembled in `src/i18n.c` from embedded C string literals — the static prefix block plus entries generated from `g_status_i18n` — and handed to core-ui's `ui_page_load_language_string` at startup (they were inline in `main.cpp` until v1.6.2, then moved to the dedicated i18n module; still compiled-in, never files). UI copy that changes often needs edits in *two* places (the `.uix` `$t('key')` reference and the `i18n.c` literal), and for status text there is a third concern — keeping the strings that C++ pushes directly in sync with the strings the template looks up via `$t()`.

This convention record exists so contributors don't (a) invent `.lang` files that the loader never reads, or (b) add a status string in one place but not the other.

## Guidance

### core-ui's i18n API: `ui_page_load_language_string`

core-ui offers `ui_page_load_language_string(page, lang_code, string)` — give it a language code and a newline-separated `key=value\nkey=value` string, and `$t('key')` lookups in the `.uix` resolve to those values for that language. There is no file loading required by the app; the *string* is the input. The manager uses this to inject both the zh and the en Language Table at startup.

### Two embedded prefix blocks, one per language

UI strings (labels, buttons, settings rows) live as multi-line C string literals in `src/i18n.c` (`s_prefix_zh` / `s_prefix_en`), structured as `key=zh_or_en\n`:

```c
/* src/i18n.c */
static const char s_prefix_zh[] =
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
static const char s_prefix_en[] =
    "app_title=nanobot manager\n"
    "chat_empty=Start a conversation\n"
    "btn_start=Start\n"
    /* ... */
    ;
/* main.cpp, at boot */
ui_page_load_language_string(g_page, "zh", i18n_table("zh"));
ui_page_load_language_string(g_page, "en", i18n_table("en"));
```

Every key in the zh block **must** appear in the en block and vice versa (enforced by `tests/test_i18n.c`). The `.uix` references these via `$t('btn_start')`, `$t('settings_network')`, etc. Adding a visible label means editing both the `.uix` (a `$t('new_key')`) and *both* prefix blocks in `src/i18n.c`.

### `/utf-8` lets Chinese be raw, but byte-escaped is the legacy form

The project compiles with `/utf-8` (`CMakeLists.txt`), so raw UTF-8 Chinese in the source compiles directly. Some strings still appear as `\xe7\xbd\x91\xe7\xbb\x9c` byte-escaped literals — that form predates the `/utf-8` switch and is harmless. Either form is acceptable; prefer raw UTF-8 for new strings (it's readable in the editor and in diffs).

### Status strings: a single source that $t and C++ both read (anti-drift)

Status text (running, stopped, starting…) has an extra failure mode: it is both `$t('status_running')` in the template *and* pushed directly from C++ via `page_set`. If those drifted, the template would show one wording and the C++ push another.

The manager solves this with a `status_i18n_entry` table that is the **only** wording source for `status_*` keys:

```c
/* src/i18n.c */
typedef struct { const char* key; const char* zh; const char* en; } status_i18n_entry;
static const status_i18n_entry g_status_i18n[] = {
    { "status_running",         "运行中",            "Running" },
    { "status_stopped",         "已停止",            "Stopped" },
    { "status_starting",        "启动中…",           "Starting…" },
    /* ... */
};
```

- `i18n_status(key, zh)` looks up a key for C++ to push via `page_set` / `set_status_i18n` (main.cpp's `t_status(key)` wrapper passes `is_zh()`).
- `i18n_table(locale)` **generates** the `status_*` portion of the assembled Language Table passed to `ui_page_load_language_string` (via the internal `append_status_i18n_lang`) — so the `$t()` lookups and the C++ pushes are reading the *same table*. They cannot drift, because there is only one set of strings.

**When adding a new status:** add one row to `g_status_i18n`. It is automatically available to both `$t()` (via the generated table portion) and C++ push (via `t_status`). Do not hand-write the new `status_*` line into the zh/en prefix literals — that would create a second source.

### Which keys live in the table: anything C++ pushes

The rule is consumption-based, not name-based:

- **Template-only keys** (labels, buttons, settings headers) are referenced only from the `.uix` via `$t()`. They live in the zh/en prefix literals, one entry per language.
- **Keys referenced from C++ — or from both sides** live in `g_status_i18n` and are generated into the assembled table so every consumer reads one wording. This is no longer only `status_*` keys: the table intentionally hosts non-status C++-pushed keys such as `auto_detect`, `chat_cancel_wait`, and the `chat_status_*` keys (per the code comment: "Non-status keys that are pushed from C++ rather than $t() live here for the same single-source reason").

Don't move template-only keys into the table "for consistency" — the table exists specifically to solve the dual-reference problem, and template-only keys don't have it.

### The chatStatusKey pattern: push the key, let the template resolve

Chat status text has a third consumption mode. `ChatUi` does not push translated text; it pushes the **key** into the `chatStatusKey` page field (`chat_ui.cpp`, `statusKey()` / `statusDetail()`), and the template renders

```html
<label class="chat-status-text">{{ chatStatusDetail || $t(chatStatusKey) }}</label>
```

Raw server error detail rides a separate non-i18n field (`chatStatusDetail`) that the template prefers when non-empty. Because the template does the `$t()` lookup on every render, a locale switch re-resolves the status line with no re-push — and the wording still comes from `g_status_i18n` (the `chat_status_*` keys live there), so it cannot drift from any other consumer.

### The literals live in the i18n module (v1.6.2+)

The prefix blocks and the status table live in `src/i18n.c` / `src/i18n.h`, not in main.cpp. The module is pure C: language is always a parameter (`is_zh()` stays in main.cpp), it touches no config and no globals — its only platform use is `InitOnceExecuteOnce`, which makes the table build thread-safe (exactly-once into static buffers, safe from any thread) — which is why a unit test can link it with one `nm_add_test` line. The whole public API is three functions:

```c
/* src/i18n.h */
const char* i18n_table(const char* locale);
/* Assembled "key=value\n" Language Table (prefix + generated status_* lines)
 * for "zh"/"en"; built exactly once via InitOnceExecuteOnce into static
 * buffers (thread-safe); prefix-only fallback on overflow; NULL for
 * unknown locale. */
int i18n_tables_complete(void);
/* 1 when both assembled tables fit in full (no overflow fallback applied);
 * asserted by tests so a key set that outgrows the table capacity fails
 * loudly instead of silently degrading every status label to its key. */
const char* i18n_status(const char* key, int zh);
/* Single-source status lookup; missing key returns key itself (same
 * fallback as $t). */
```

main.cpp keeps only consumers — the two load calls, and a thin wrapper so no existing call site churned:

```c
/* main.cpp — at boot */
ui_page_load_language_string(g_page, "zh", i18n_table("zh"));
ui_page_load_language_string(g_page, "en", i18n_table("en"));

/* main.cpp — the C++ push path keeps its shape */
static const char* t_status(const char* key) {
    return i18n_status(key, is_zh());
}
```

Scope note: the ~20 `is_zh() ? L"…" : L"…"` `MessageBoxW` dialog strings intentionally stay inline in main.cpp — they are wide-string dialog copy tied to specific dialogs, not `$t()` table data.

### The discipline is enforced by CTest, not review

`tests/test_i18n.c` parses both assembled tables and mechanically checks what this doc asks humans to remember (6 tests):

1. **Shape** — every line is `key=value\n` with non-empty key and value; the table ends with `\n`.
2. **Parity** — zh/en key sets are identical (90 = 34 prefix + 56 `g_status_i18n` entries); a mismatch fails naming the exact missing key (`key 'setting_advanced' exists in zh but is missing in en`), instead of a raw key rendering in the UI for a user to report.
3. **Status single-source** — every `status_*` key in the generated table resolves via `i18n_status` in both languages; hand-writing a `status_*` line into the prefix literals (a second wording source) makes this fail.
4. **Fallbacks** — missing key returns the key itself; unknown locale returns NULL.
5. **Duplicate-key rejection** — no key may appear twice in an assembled table; a duplicate would make core-ui's first-wins/last-wins behavior part of the contract.
6. **Capacity anti-drift** — `i18n_tables_complete()` must return 1: if the key set ever outgrows the table capacity, `build_table` silently falls back to a prefix subset and every status label degrades to its raw key while the other tests stay green; this test closes that hole (plus spot-checks that non-status C++-pushed keys like `auto_detect` ride in the same table).

Run CTest before committing string changes: `ctest --test-dir nanobot-manager/build -C Release --output-on-failure`.

### Don't invent `.lang` files

If a guide or habit suggests creating `assets/zh.lang` / `en.lang`: **don't.** The loader is `ui_page_load_language_string(page, code, string)` — it takes a string, not a path. There is no code path that reads a `.lang` file. Adding one would be dead weight and would not be loaded.

## Why This Matters

- **Avoids the missing-files hunt.** The single most common i18n onboarding confusion here is "where are the language files?" Answering it once, in the convention doc, saves every future contributor the search.
- **The status anti-drift table is load-bearing.** Without it, `$t('status_running')` in the template and the C++ push of the running string would be two independent edits, and they would eventually disagree (one says "Running", the other "Active"). The table makes drift impossible by construction.
- **The discipline is now mechanical.** The both-languages rule used to be review-only: forgetting the en side surfaced as a raw key in the UI — a runtime discovery by a user. `test_key_parity` fails at build time and names the missing key, and `status_single_source` catches a second wording source the moment it appears.
- **Edits-in-two-places is the recurring cost.** A label change touches the `.uix` key reference (rarely) and *both* prefix blocks (always). Forgetting the second language leaves a key unresolved — `$t()` falls back to the key itself, so the UI shows the raw key string. The fix is mechanical (edit zh and en), but the discipline is "always both."
- **Keeping it in-source keeps the build simple.** No resource embedding step, no install rule for language files, no missing-file failure mode. The strings compile into the binary.

## When to Apply

- **Adding any user-visible string** — add the key to the `.uix` (`$t('key')`), then add `key=zh` to `s_prefix_zh` and `key=en` to `s_prefix_en` in `src/i18n.c`. Both languages, every time.
- **Adding a status string** (referenced from both template and C++) — add one row to `g_status_i18n`; do not hand-edit the zh/en prefix literals for it.
- **Considering external `.lang` / `.json` language files** — don't; the loader takes in-source strings. If the string count ever makes in-source unwieldy, that's a deliberate refactor decision, not a default.
- **Seeing `$t('some_key')` render as the literal `some_key`** — the key is missing from the active Language Table; add it on both language sides in `src/i18n.c` (prefix literal or `g_status_i18n`, whichever owns that key). `tests/test_i18n.c` fails on zh/en key drift, so run CTest before committing string changes.
- **Reviewing an i18n PR** — confirm: every new key exists in zh *and* en; keys pushed from C++ went into the table, not the prefix literals; no `.lang` file was added.

## Examples

### Add a non-status label (both prefix blocks)

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
Now both `$t('status_restarting')` and `i18n_status("status_restarting", zh)` work, from the same string.

### Wrong: inventing a .lang file

```c
/* DO NOT DO THIS — the loader never reads files */
ui_page_load_language_file(g_page, "assets/en.lang");  /* no such API in this project */
```

## Related

- `docs/solutions/reference/core-ui-overview.md` — core-ui's i18n API surface, including the absence of `ui_text_input_on_changed` and the `ui_page_load_language_string` contract.
- `docs/solutions/design-patterns/core-ui-input-read-pattern.md` — references the embedded-literal convention for its settings-section i18n; this doc is the canonical statement.
- `nanobot-manager/src/i18n.c` / `src/i18n.h` — the zh/en prefix blocks, the `g_status_i18n` table, `i18n_table` / `i18n_status` / `i18n_tables_complete`, and the thread-safe (InitOnceExecuteOnce) assembly with overflow fallback.
- `nanobot-manager/src/main.cpp` — the `ui_page_load_language_string` calls, the `t_status` wrapper, and the `is_zh()` dialog strings (which are intentionally NOT part of the language-table system).
- `nanobot-manager/tests/test_i18n.c` — CTest enforcement of zh/en key parity, status single-source, duplicate-key rejection, and table-capacity completeness.
- `docs/solutions/architecture-patterns/config-system.md` — the same pattern family: carve a pure, testable module out of a fat source file and pin it with a unit test.
- `docs/solutions/infrastructure/testing-and-ci-setup.md` — how CTest targets are wired (`nm_add_test`), including `test_i18n`.
- `nanobot-manager/ui/dashboard.uix` — the `$t('key')` references that resolve against these tables (and the `$t(chatStatusKey)` chat status bar).
- Commit `96d8b1b` — `feat(ui): add i18n with Chinese/English language switching` (the origin of this convention).
