#ifndef I18N_H
#define I18N_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bilingual (zh/en) language tables for core-ui's ui_page_load_language_string.
 *
 * core-ui exposes ui_page_load_language_string / $t('key') for .uix templates
 * but has no C++ key→string lookup API. Status text is pushed from C++ via
 * page_set. The g_status_i18n table (i18n.c) is the ONLY wording source for
 * status_* keys (and non-status keys pushed from C++ instead of $t()):
 *   - i18n_status() resolves keys for the C++ push path (main.cpp t_status)
 *   - i18n_table() generates the status_* portion of the language blob passed
 *     to ui_page_load_language_string (so $t and C++ cannot drift)
 *
 * Pure data + assembly module: no config reads, the language is always a
 * parameter. The assembled tables are cached in static buffers built exactly
 * once (InitOnceExecuteOnce), so every accessor is safe from any thread.
 * Strings stay compiled-in — no external .lang files (see
 * docs/solutions/conventions/i18n-embedded-literals-not-lang-files.md). */

/* Assembled full table ("key=value\n" lines: non-status prefix + status_*)
 * for "zh" or "en". Built lazily on first call into static buffers; on
 * overflow (unreachable with the current key set) it falls back to the
 * complete prefix lines that fit so the UI still boots — status keys then
 * fall back to their key in $t(). Returns NULL for unknown locale. */
const char* i18n_table(const char* locale);

/* 1 when both assembled tables fit in full (no overflow fallback applied).
 * Tests assert this so a key set that outgrows the table capacity fails
 * loudly instead of silently degrading every status label to its key. */
int i18n_tables_complete(void);

/* status_* wording lookup: returns the zh (zh != 0) or en value. A missing
 * key returns key itself (same fallback as $t). */
const char* i18n_status(const char* key, int zh);

#ifdef __cplusplus
}
#endif

#endif /* I18N_H */
