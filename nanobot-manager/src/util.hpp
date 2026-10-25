#ifndef UTIL_HPP
#define UTIL_HPP

#include <string>
#include "ui_core.h"

/* Small helpers shared by main.cpp and the chat modules. */

/* MultiByteToWideChar(CP_UTF8) on a NUL-terminated string, capped at
 * max_wchars wide chars (including the terminator). Returns an empty string
 * when utf8 is NULL, conversion fails, or the result would exceed the cap --
 * callers distinguish "empty input" themselves when it matters. */
std::wstring nm_utf8_to_wide(const char* utf8, size_t max_wchars = 1024);

/* Push a UTF-8 string into a page text field. An empty/NULL string pushes
 * L"" (clears the field) rather than being skipped. */
void nm_page_set_utf8(UiPage page, const char* name, const char* utf8);

/* Percent-encode every byte outside the RFC 3986 unreserved set (mirrors
 * Python urllib quote(s, safe='') used by nanobot's own browser URL).
 * Returns an empty string for NULL/empty input or when the input contains
 * CR/LF/control chars (header/URL injection guard); callers that must tell
 * "rejected" apart from "empty" check the input themselves first. */
std::string nm_percent_encode(const char* utf8);

#endif
