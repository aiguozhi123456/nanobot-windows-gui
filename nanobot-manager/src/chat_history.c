#include "chat_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int is_json_trim_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
        value == '\v' || value == '\f';
}

static int json_contains_decoded_nul_escape(const char* json, size_t length) {
    int in_string = 0;
    size_t offset;
    if (!json) return 0;
    for (offset = 0; offset < length; offset++) {
        unsigned char value = (unsigned char)json[offset];
        if (!in_string) {
            if (value == '"') in_string = 1;
            continue;
        }
        if (value == '"') {
            in_string = 0;
            continue;
        }
        if (value != '\\') continue;
        if (length - offset >= 6 && json[offset + 1] == 'u' && json[offset + 2] == '0' &&
            json[offset + 3] == '0' && json[offset + 4] == '0' && json[offset + 5] == '0')
            return 1;
        if (offset + 1 < length) offset++;
    }
    return 0;
}

static int utf8_decode(const unsigned char* text, size_t length, size_t* consumed, unsigned int* codepoint) {
    unsigned char first;
    if (!text || !length || !consumed || !codepoint) return 0;
    first = text[0];
    if (first <= 0x7F) {
        *consumed = 1;
        *codepoint = first;
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF && length >= 2 &&
        text[1] >= 0x80 && text[1] <= 0xBF) {
        *consumed = 2;
        *codepoint = ((unsigned int)(first & 0x1F) << 6) | (text[1] & 0x3F);
        return 1;
    }
    if (first >= 0xE0 && first <= 0xEF && length >= 3 && text[2] >= 0x80 && text[2] <= 0xBF &&
        ((first == 0xE0 && text[1] >= 0xA0 && text[1] <= 0xBF) ||
         (first == 0xED && text[1] >= 0x80 && text[1] <= 0x9F) ||
         (first != 0xE0 && first != 0xED && text[1] >= 0x80 && text[1] <= 0xBF))) {
        *consumed = 3;
        *codepoint = ((unsigned int)(first & 0x0F) << 12) |
            ((unsigned int)(text[1] & 0x3F) << 6) | (text[2] & 0x3F);
        return 1;
    }
    if (first >= 0xF0 && first <= 0xF4 && length >= 4 &&
        text[2] >= 0x80 && text[2] <= 0xBF && text[3] >= 0x80 && text[3] <= 0xBF &&
        ((first == 0xF0 && text[1] >= 0x90 && text[1] <= 0xBF) ||
         (first == 0xF4 && text[1] >= 0x80 && text[1] <= 0x8F) ||
         (first >= 0xF1 && first <= 0xF3 && text[1] >= 0x80 && text[1] <= 0xBF))) {
        *consumed = 4;
        *codepoint = ((unsigned int)(first & 0x07) << 18) |
            ((unsigned int)(text[1] & 0x3F) << 12) |
            ((unsigned int)(text[2] & 0x3F) << 6) | (text[3] & 0x3F);
        return 1;
    }
    return 0;
}

static int is_python_space(unsigned int codepoint) {
    return (codepoint >= 0x0009 && codepoint <= 0x000D) ||
        (codepoint >= 0x001C && codepoint <= 0x001F) || codepoint == 0x0020 ||
        codepoint == 0x0085 || codepoint == 0x00A0 || codepoint == 0x1680 ||
        (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 ||
        codepoint == 0x2029 || codepoint == 0x202F || codepoint == 0x205F ||
        codepoint == 0x3000;
}

static int is_ascii_token_char(unsigned char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') || value == '_';
}

int chat_history_is_blank_message(const char* text) {
    const unsigned char* bytes = (const unsigned char*)text;
    size_t length;
    size_t offset = 0;
    if (!bytes) return 0;
    length = strlen(text);
    while (offset < length) {
        size_t consumed;
        unsigned int codepoint;
        if (!utf8_decode(bytes + offset, length - offset, &consumed, &codepoint) ||
            !is_python_space(codepoint))
            return 0;
        offset += consumed;
    }
    return 1;
}

int chat_history_is_new_command(const char* text) {
    const unsigned char* bytes = (const unsigned char*)text;
    size_t length;
    size_t offset = 0;
    size_t start = 0;
    size_t end = 0;
    int leading_space = 1;
    if (!bytes) return 0;
    length = strlen(text);
    while (offset < length) {
        size_t consumed;
        unsigned int codepoint;
        if (!utf8_decode(bytes + offset, length - offset, &consumed, &codepoint)) return 0;
        offset += consumed;
        if (leading_space && is_python_space(codepoint)) {
            start = offset;
        } else {
            leading_space = 0;
            if (!is_python_space(codepoint)) end = offset;
        }
    }
    if (end <= start) return 0;
    if (end - start == 4 && bytes[start] == '/' &&
        (bytes[start + 1] == 'n' || bytes[start + 1] == 'N') &&
        (bytes[start + 2] == 'e' || bytes[start + 2] == 'E') &&
        (bytes[start + 3] == 'w' || bytes[start + 3] == 'W'))
        return 1;
    if (end - start <= 5 || bytes[start] != '/' ||
        (bytes[start + 1] != 'n' && bytes[start + 1] != 'N') ||
        (bytes[start + 2] != 'e' && bytes[start + 2] != 'E') ||
        (bytes[start + 3] != 'w' && bytes[start + 3] != 'W') || bytes[start + 4] != '@')
        return 0;
    for (offset = start + 5; offset < end; offset++)
        if (!is_ascii_token_char(bytes[offset])) return 0;
    return 1;
}

int chat_history_path_from_config(const wchar_t* config_path, wchar_t* out_path, size_t out_count) {
    const wchar_t* slash;
    size_t directory_length;
    if (!config_path || !config_path[0] || !out_path || out_count == 0) return -1;
    slash = wcsrchr(config_path, L'\\');
    if (!slash) slash = wcsrchr(config_path, L'/');
    directory_length = slash ? (size_t)(slash - config_path + 1) : 0;
    if (directory_length + wcslen(L"chat-history.json") + 1 > out_count) return -1;
    if (directory_length) memcpy(out_path, config_path, directory_length * sizeof(wchar_t));
    wcscpy_s(out_path + directory_length, out_count - directory_length, L"chat-history.json");
    return 0;
}

/* Exactly the two named keys, each present exactly once. */
static int exact_members(const cJSON* object, const char* first, const char* second) {
    const cJSON* child;
    int has_first = 0;
    int has_second = 0;
    if (!cJSON_IsObject(object)) return 0;
    for (child = object->child; child; child = child->next) {
        if (!child->string) return 0;
        if (strcmp(child->string, first) == 0) {
            if (has_first) return 0;   /* duplicate key */
            has_first = 1;
        } else if (strcmp(child->string, second) == 0) {
            if (has_second) return 0;  /* duplicate key */
            has_second = 1;
        } else {
            return 0;
        }
    }
    return has_first && has_second;
}

/* A single message must always be able to fit inside the total-text budget,
 * or the drop-oldest trim below can never converge. */
typedef char chat_history_static_assert_message_fits_total[
    (CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES <= CHAT_HISTORY_MAX_TOTAL_TEXT_BYTES) ? 1 : -1];

static int copy_normalized_messages(const cJSON* source, cJSON** out_messages, int strict) {
    cJSON* result;
    int count;
    int first;
    size_t total_text = 0;
    if (!out_messages || !cJSON_IsArray(source)) return -1;
    *out_messages = NULL;
    count = cJSON_GetArraySize(source);
    if (count < 0) return -1;
    /* Size caps are a normalization policy, not a validity rule: trim to the
     * newest CHAT_HISTORY_MAX_MESSAGES on load exactly like the serializer
     * does, so an oversized-but-valid history is truncated and reused instead
     * of being quarantined. `strict` stays responsible for structure, roles
     * and ids only. */
    first = count > CHAT_HISTORY_MAX_MESSAGES ? count - CHAT_HISTORY_MAX_MESSAGES : 0;
    for (int index = first; index < count; index++) {
        const cJSON* item = cJSON_GetArrayItem(source, index);
        const cJSON* role;
        const cJSON* text;
        size_t text_length;
        if (!item || (strict && !exact_members(item, "role", "text"))) return -1;
        role = cJSON_GetObjectItemCaseSensitive(item, "role");
        text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (!cJSON_IsString(role) || !role->valuestring || !cJSON_IsString(text) || !text->valuestring ||
            (strcmp(role->valuestring, "user") != 0 && strcmp(role->valuestring, "assistant") != 0))
            return -1;
        text_length = strlen(text->valuestring);
        if (text_length > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) return -1;
        if (text_length > CHAT_HISTORY_MAX_TOTAL_TEXT_BYTES - total_text) {
            while (text_length > CHAT_HISTORY_MAX_TOTAL_TEXT_BYTES - total_text) {
                const cJSON* oldest;
                const cJSON* oldest_text;
                if (first > index) return -1;   /* cannot free enough budget */
                oldest = cJSON_GetArrayItem(source, first);
                oldest_text = oldest ? cJSON_GetObjectItemCaseSensitive(oldest, "text") : NULL;
                if (!oldest_text || !oldest_text->valuestring) return -1;
                total_text -= strlen(oldest_text->valuestring);
                first++;
            }
        }
        total_text += text_length;
    }
    result = cJSON_CreateArray();
    if (!result) return -1;
    for (int index = first; index < count; index++) {
        const cJSON* item = cJSON_GetArrayItem(source, index);
        const cJSON* role;
        const cJSON* text;
        cJSON* copy;
        size_t text_length;
        if (!item || (strict && !exact_members(item, "role", "text"))) goto invalid;
        role = cJSON_GetObjectItemCaseSensitive(item, "role");
        text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (!cJSON_IsString(role) || !role->valuestring || !cJSON_IsString(text) || !text->valuestring)
            goto invalid;
        if ((strcmp(role->valuestring, "user") != 0 && strcmp(role->valuestring, "assistant") != 0))
            goto invalid;
        text_length = strlen(text->valuestring);
        if (text_length > CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES) goto invalid;
        copy = cJSON_CreateObject();
        if (!copy || !cJSON_AddStringToObject(copy, "role", role->valuestring) ||
            !cJSON_AddStringToObject(copy, "text", text->valuestring)) {
            cJSON_Delete(copy);
            goto invalid;
        }
        if (!cJSON_AddItemToArray(result, copy)) {
            cJSON_Delete(copy);
            goto invalid;
        }
    }
    *out_messages = result;
    return 0;
invalid:
    cJSON_Delete(result);
    return -1;
}

void chat_history_document_init(chat_history_document* document) {
    if (document) memset(document, 0, sizeof(*document));
}

void chat_history_document_free(chat_history_document* document) {
    if (!document) return;
    cJSON_Delete(document->messages);
    chat_history_document_init(document);
}

int chat_history_is_valid_chat_id(const char* chat_id) {
    size_t length;
    if (!chat_id) return 0;
    length = strlen(chat_id);
    if (length < 1 || length > 64) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)chat_id[i];
        if (!is_ascii_token_char(c) && c != ':' && c != '-')
            return 0;
    }
    return 1;
}

int chat_history_document_can_resume(const chat_history_document* document, int webui_port) {
    return document && document->version == 2 && cJSON_IsArray(document->messages) &&
        document->webui_port >= 1 && document->webui_port <= 65535 &&
        document->webui_port == webui_port &&
        document->resume_safe && chat_history_is_valid_chat_id(document->chat_id);
}

int chat_history_parse_json(const char* json, chat_history_document* out_document) {
    cJSON* root;
    cJSON* version;
    cJSON* messages;
    const char* end = NULL;
    size_t length;
    cJSON* port;
    cJSON* chat_id;
    cJSON* resume_safe;
    if (!json || !out_document) return -1;
    /* Release any previously owned tree so re-parsing into the same document
     * cannot leak it. out_document must be initialized (or zeroed) by the
     * caller — see chat_history.h. */
    chat_history_document_free(out_document);
    length = strlen(json);
    if (json_contains_decoded_nul_escape(json, length)) return -1;
    root = cJSON_ParseWithLengthOpts(json, length, &end, 0);
    if (!root) return -1;
    while (end && is_json_trim_space((unsigned char)*end)) end++;
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!end || *end || !cJSON_IsNumber(version)) {
        cJSON_Delete(root);
        return -1;
    }
    if (version->valuedouble == 1.0 && version->valueint == 1 &&
        exact_members(root, "version", "messages") &&
        copy_normalized_messages(messages, &out_document->messages, 1) == 0) {
        out_document->version = 1;
        cJSON_Delete(root);
        return 0;
    }
    port = cJSON_GetObjectItemCaseSensitive(root, "webui_port");
    chat_id = cJSON_GetObjectItemCaseSensitive(root, "chat_id");
    resume_safe = cJSON_GetObjectItemCaseSensitive(root, "resume_safe");
    if (!(version->valuedouble == 2.0 && version->valueint == 2) ||
        !cJSON_IsObject(root) || !port || !chat_id || !resume_safe ||
        !cJSON_IsNumber(port) || port->valuedouble != (double)port->valueint ||
        port->valueint < 1 || port->valueint > 65535 ||
        !cJSON_IsString(chat_id) || !chat_id->valuestring ||
        !chat_history_is_valid_chat_id(chat_id->valuestring) ||
        !cJSON_IsBool(resume_safe) || !messages) {
        cJSON_Delete(root); return -1;
    }
    {
        const cJSON* child; int count = 0;
        for (child = root->child; child; child = child->next) {
            if (!child->string || (strcmp(child->string, "version") && strcmp(child->string, "webui_port") &&
                strcmp(child->string, "chat_id") && strcmp(child->string, "resume_safe") && strcmp(child->string, "messages"))) {
                cJSON_Delete(root); return -1;
            }
            count++;
        }
        if (count != 5 || copy_normalized_messages(messages, &out_document->messages, 1) != 0) {
            cJSON_Delete(root); return -1;
        }
    }
    out_document->version = 2;
    out_document->webui_port = port->valueint;
    if (strcpy_s(out_document->chat_id, sizeof(out_document->chat_id), chat_id->valuestring) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    out_document->resume_safe = cJSON_IsTrue(resume_safe) ? 1 : 0;
    cJSON_Delete(root);
    return 0;
}

char* chat_history_serialize(const chat_history_document* document) {
    cJSON* normalized = NULL;
    cJSON* root;
    char* serialized;
    if (!document || document->version != 2 || document->webui_port < 1 ||
        document->webui_port > 65535 || !chat_history_is_valid_chat_id(document->chat_id) ||
        !document->messages || copy_normalized_messages(document->messages, &normalized, 0) != 0) return NULL;
    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(normalized);
        return NULL;
    }
    if (!cJSON_AddNumberToObject(root, "version", 2) ||
        !cJSON_AddNumberToObject(root, "webui_port", document->webui_port) ||
        !cJSON_AddStringToObject(root, "chat_id", document->chat_id) ||
        !cJSON_AddBoolToObject(root, "resume_safe", document->resume_safe ? 1 : 0) ||
        !cJSON_AddItemToObject(root, "messages", normalized)) {
        cJSON_Delete(normalized);
        cJSON_Delete(root);
        return NULL;
    }
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return serialized;
}

chat_history_load_result chat_history_load(const wchar_t* path, chat_history_document* out_document, DWORD* out_error) {
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read_total = 0;
    char* content;
    if (out_document) chat_history_document_init(out_document);
    if (out_error) *out_error = ERROR_SUCCESS;
    if (!path || !out_document) {
        if (out_error) *out_error = ERROR_INVALID_PARAMETER;
        return CHAT_HISTORY_LOAD_IO_ERROR;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (out_error) *out_error = error;
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? CHAT_HISTORY_LOAD_ABSENT : CHAT_HISTORY_LOAD_IO_ERROR;
    }
    if (!GetFileSizeEx(file, &size)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        if (out_error) *out_error = error;
        return CHAT_HISTORY_LOAD_IO_ERROR;
    }
    if (size.QuadPart < 0 || (unsigned long long)size.QuadPart > CHAT_HISTORY_MAX_FILE_BYTES) {
        CloseHandle(file);
        if (out_error) *out_error = ERROR_FILE_TOO_LARGE;
        return CHAT_HISTORY_LOAD_INVALID;
    }
    content = (char*)malloc((size_t)size.QuadPart + 1);
    if (!content) {
        CloseHandle(file);
        if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY;
        return CHAT_HISTORY_LOAD_IO_ERROR;
    }
    while (read_total < (DWORD)size.QuadPart) {
        DWORD read_now = 0;
        if (!ReadFile(file, content + read_total, (DWORD)size.QuadPart - read_total, &read_now, NULL)) {
            DWORD error = GetLastError();
            free(content);
            CloseHandle(file);
            if (out_error) *out_error = error;
            return CHAT_HISTORY_LOAD_IO_ERROR;
        }
        if (!read_now) break;
        read_total += read_now;
    }
    if (!CloseHandle(file) || read_total != (DWORD)size.QuadPart) {
        free(content);
        if (out_error) *out_error = ERROR_READ_FAULT;
        return CHAT_HISTORY_LOAD_IO_ERROR;
    }
    if (memchr(content, '\0', read_total) != NULL) {
        free(content);
        if (out_error) *out_error = ERROR_INVALID_DATA;
        return CHAT_HISTORY_LOAD_INVALID;
    }
    content[read_total] = '\0';
    if (chat_history_parse_json(content, out_document) != 0) {
        free(content);
        if (out_error) *out_error = ERROR_INVALID_DATA;
        return CHAT_HISTORY_LOAD_INVALID;
    }
    free(content);
    return CHAT_HISTORY_LOAD_OK;
}

int chat_history_quarantine_invalid(const wchar_t* path, DWORD* out_error) {
    wchar_t* backup_path;
    size_t path_length;
    int result = -1;
    if (out_error) *out_error = ERROR_SUCCESS;
    if (!path || !path[0]) {
        if (out_error) *out_error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    path_length = wcslen(path);
    backup_path = (wchar_t*)malloc((path_length + 64) * sizeof(wchar_t));
    if (!backup_path) {
        if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY;
        return -1;
    }
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        swprintf_s(backup_path, path_length + 64, L"%s.invalid.%lu.%lu.%u", path,
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), attempt);
        if (MoveFileExW(path, backup_path, MOVEFILE_WRITE_THROUGH)) {
            result = 0;
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (result != 0 && out_error) *out_error = GetLastError();
    free(backup_path);
    return result;
}

int chat_history_archive(const wchar_t* path, DWORD* out_error) {
    wchar_t* archive_path;
    size_t path_length;
    int result = -1;
    if (out_error) *out_error = ERROR_SUCCESS;
    if (!path || !path[0]) { if (out_error) *out_error = ERROR_INVALID_PARAMETER; return -1; }
    path_length = wcslen(path);
    archive_path = (wchar_t*)malloc((path_length + 64) * sizeof(wchar_t));
    if (!archive_path) { if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY; return -1; }
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        swprintf_s(archive_path, path_length + 64, L"%s.archive.%lu.%lu.%u", path,
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), attempt);
        if (MoveFileExW(path, archive_path, MOVEFILE_WRITE_THROUGH)) { result = 0; break; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (result != 0 && out_error) *out_error = GetLastError();
    free(archive_path);
    return result;
}

int chat_history_copy_to_archive(const wchar_t* path, wchar_t* out_archive_path, size_t out_count, DWORD* out_error) {
    wchar_t* archive_path;
    size_t path_length;
    int result = -1;
    if (out_error) *out_error = ERROR_SUCCESS;
    if (!path || !path[0] || !out_archive_path || out_count == 0) {
        if (out_error) *out_error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    path_length = wcslen(path);
    archive_path = (wchar_t*)malloc((path_length + 64) * sizeof(wchar_t));
    if (!archive_path) { if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY; return -1; }
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        swprintf_s(archive_path, path_length + 64, L"%s.archive.%lu.%lu.%u", path,
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), attempt);
        if (wcslen(archive_path) + 1 > out_count) { if (out_error) *out_error = ERROR_INSUFFICIENT_BUFFER; break; }
        if (CopyFileW(path, archive_path, TRUE)) {
            wcscpy_s(out_archive_path, out_count, archive_path);
            result = 0;
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (result != 0 && out_error && *out_error == ERROR_SUCCESS) *out_error = GetLastError();
    free(archive_path);
    return result;
}

int chat_history_save(const wchar_t* path, const chat_history_document* document, DWORD* out_error) {
    char* serialized;
    wchar_t* temporary_path;
    size_t path_length;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD total = 0;
    DWORD length;
    int result = -1;
    if (out_error) *out_error = ERROR_SUCCESS;
    if (!path || !path[0] || !document) {
        if (out_error) *out_error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    serialized = chat_history_serialize(document);
    if (!serialized) {
        if (out_error) *out_error = ERROR_INVALID_DATA;
        return -1;
    }
    if (strlen(serialized) > CHAT_HISTORY_MAX_FILE_BYTES) {
        free(serialized);
        if (out_error) *out_error = ERROR_FILE_TOO_LARGE;
        return -1;
    }
    path_length = wcslen(path);
    temporary_path = (wchar_t*)malloc((path_length + 48) * sizeof(wchar_t));
    if (!temporary_path) {
        free(serialized);
        if (out_error) *out_error = ERROR_NOT_ENOUGH_MEMORY;
        return -1;
    }
    for (unsigned int attempt = 0; attempt < 16; attempt++) {
        swprintf_s(temporary_path, path_length + 48, L"%s.tmp.%lu.%lu.%u", path,
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), attempt);
        file = CreateFileW(temporary_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        if (out_error) *out_error = GetLastError();
        goto done;
    }
    length = (DWORD)strlen(serialized);
    while (total < length) {
        DWORD written = 0;
        if (!WriteFile(file, serialized + total, length - total, &written, NULL) || !written) {
            if (out_error) *out_error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            DeleteFileW(temporary_path);
            goto done;
        }
        total += written;
    }
    {
        BOOL flushed = FlushFileBuffers(file);
        DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
        BOOL closed = CloseHandle(file);
        DWORD close_error = closed ? ERROR_SUCCESS : GetLastError();
        file = INVALID_HANDLE_VALUE;
        if (!flushed || !closed) {
            if (out_error) *out_error = flush_error ? flush_error : (close_error ? close_error : ERROR_WRITE_FAULT);
            DeleteFileW(temporary_path);
            goto done;
        }
    }
    file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (out_error) *out_error = GetLastError();
        DeleteFileW(temporary_path);
        goto done;
    }
    result = 0;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(temporary_path);
    free(serialized);
    return result;
}
