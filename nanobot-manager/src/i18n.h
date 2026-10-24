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
 * status_* keys:
 *   - i18n_status() resolves keys for the C++ push path (main.cpp t_status)
 *   - i18n_table() generates the status_* portion of the language blob passed
 *     to ui_page_load_language_string (so $t and C++ cannot drift)
 *
 * Pure data + assembly module: no config/global state, the language is always
 * a parameter. Strings stay compiled-in — no external .lang files (see
 * docs/solutions/conventions/i18n-embedded-literals-not-lang-files.md). */

/* Assembled full table ("key=value\n" lines: non-status prefix + status_*)
 * for "zh" or "en". Built lazily on first call into static buffers; on
 * overflow (unreachable with the current key set) it falls back to the
 * non-status prefix alone so the UI still boots — status keys then fall back
 * to their key in $t(). Returns NULL for unknown locale. */
const char* i18n_table(const char* locale);

/* status_* wording lookup: returns the zh (zh != 0) or en value. A missing
 * key returns key itself (same fallback as $t). */
const char* i18n_status(const char* key, int zh);

#ifdef __cplusplus
}
#endif

#endif /* I18N_H */
