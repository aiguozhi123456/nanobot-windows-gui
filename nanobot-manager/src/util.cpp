#include "util.hpp"

#include <windows.h>

std::wstring nm_utf8_to_wide(const char* utf8, size_t max_wchars) {
    if (!utf8 || !utf8[0] || max_wchars == 0) return std::wstring();
    /* cbMultiByte MUST be -1 (a NUL-terminated byte count, not the wchar
     * sizing count) so multibyte sequences decode fully and the API writes
     * the terminator. */
    int written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (written <= 0 || (size_t)written > max_wchars) return std::wstring();
    std::wstring out;
    out.resize((size_t)written - 1);
    written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], written);
    if (written <= 0) return std::wstring();
    return out;
}

void nm_page_set_utf8(UiPage page, const char* name, const char* utf8) {
    if (!page || !name) return;
    const char* src = utf8 ? utf8 : "";
    if (!src[0]) { ui_page_set_text(page, name, L""); return; }
    std::wstring w = nm_utf8_to_wide(src);
    if (w.empty()) return;   /* conversion failure / over cap: leave as-is */
    ui_page_set_text(page, name, w.c_str());
}

std::string nm_percent_encode(const char* utf8) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    if (!utf8) return out;
    out.reserve(strlen(utf8) * 3 + 1);
    for (const unsigned char* p = (const unsigned char*)utf8; *p; p++) {
        unsigned char c = *p;
        if (c == '\r' || c == '\n' || c < 0x20) return std::string();
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
            c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}
