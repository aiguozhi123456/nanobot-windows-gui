#include "nm_test.h"
#include "chat_history.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

static void make_temp_path(wchar_t* path, size_t count, const wchar_t* name) {
    wchar_t directory[MAX_PATH];
    GetTempPathW((DWORD)_countof(directory), directory);
    swprintf_s(path, count, L"%snanobot-manager-%lu-%s", directory,
        (unsigned long)GetCurrentProcessId(), name);
}

static void make_v2(chat_history_document* d, cJSON* m) {
    chat_history_document_init(d);
    d->version = 2; d->webui_port = 8765; d->resume_safe = 1;
    strcpy_s(d->chat_id, sizeof(d->chat_id), "test-session"); d->messages = m;
}

static void append_ui_message(cJSON* a, const char* role, const char* text) {
    cJSON* m = cJSON_CreateObject();
    TEST_CHECK(m != NULL);
    if (!m) return;
    TEST_CHECK(cJSON_AddStringToObject(m, "role", role) != NULL);
    TEST_CHECK(cJSON_AddStringToObject(m, "text", text) != NULL);
    TEST_CHECK(cJSON_AddItemToArray(a, m));
}

static cJSON* messages(void) {
    cJSON* a = cJSON_CreateArray(); cJSON* m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", 9);
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "text", "hello");
    cJSON_AddItemToArray(a, m); return a;
}

static void test_v1_is_legacy(void) {
    chat_history_document d; chat_history_document_init(&d);
    TEST_CHECK(chat_history_parse_json("{\"version\":1,\"messages\":[]}", &d) == 0);
    TEST_CHECK(d.version == 1 && !chat_history_document_can_resume(&d, 8765));
    chat_history_document_free(&d);
}

static void test_v2_round_trip(void) {
    chat_history_document d, loaded; char* json;
    chat_history_document_init(&d); d.version = 2; d.webui_port = 8765;
    strcpy_s(d.chat_id, sizeof(d.chat_id), "session_1:ok-"); d.resume_safe = 1; d.messages = messages();
    json = chat_history_serialize(&d);
    TEST_CHECK(json && strstr(json, "\"id\"") == NULL);
    chat_history_document_init(&loaded);
    TEST_CHECK(chat_history_parse_json(json, &loaded) == 0);
    TEST_CHECK(loaded.version == 2 && loaded.webui_port == 8765 && loaded.resume_safe);
    TEST_CHECK(chat_history_document_can_resume(&loaded, 8765));
    TEST_CHECK(!chat_history_document_can_resume(&loaded, 8766));
    free(json); chat_history_document_free(&d); chat_history_document_free(&loaded);

    make_v2(&d, cJSON_CreateArray());
    append_ui_message(d.messages, "user", "unicode \xE4\xB8\x96\xE7\x95\x8C");
    json = chat_history_serialize(&d);
    chat_history_document_init(&loaded);
    TEST_CHECK(chat_history_parse_json(json, &loaded) == 0);
    TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded.messages, 0), "text")->valuestring,
        "unicode \xE4\xB8\x96\xE7\x95\x8C") == 0);
    free(json); chat_history_document_free(&d); chat_history_document_free(&loaded);
}

static void test_strict_v2(void) {
    const char* bad[] = {
        "{\"version\":2,\"webui_port\":0,\"chat_id\":\"a\",\"resume_safe\":true,\"messages\":[]}",
        "{\"version\":2,\"webui_port\":8765,\"chat_id\":\"bad space\",\"resume_safe\":true,\"messages\":[]}",
        "{\"version\":2,\"webui_port\":8765,\"chat_id\":\"a\",\"resume_safe\":1,\"messages\":[]}",
        "{\"version\":2,\"webui_port\":8765,\"chat_id\":\"a\",\"resume_safe\":false,\"messages\":[],\"extra\":1}", NULL };
    for (int i = 0; bad[i]; i++) { chat_history_document d; chat_history_document_init(&d); TEST_CHECK(chat_history_parse_json(bad[i], &d) != 0); chat_history_document_free(&d); }
    TEST_CHECK(chat_history_is_valid_chat_id("A_z:1-2"));
    TEST_CHECK(!chat_history_is_valid_chat_id("")); TEST_CHECK(!chat_history_is_valid_chat_id("a/b"));
}

static void test_archive(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH], pattern[MAX_PATH + 64];
    HANDLE file; DWORD written; WIN32_FIND_DATAW find;
    GetTempPathW((DWORD)_countof(dir), dir);
    swprintf_s(path, _countof(path), L"%snanobot-history-archive-%lu.json", dir, (unsigned long)GetCurrentProcessId());
    DeleteFileW(path);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        TEST_CHECK(WriteFile(file, "old", 3, &written, NULL) && written == 3);
        CloseHandle(file);
        TEST_CHECK(chat_history_archive(path, NULL) == 0);
        TEST_CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
        swprintf_s(pattern, _countof(pattern), L"%s.archive.*", path);
        HANDLE h = FindFirstFileW(pattern, &find);
        TEST_CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t archived[MAX_PATH + 64];
            const wchar_t* slash = wcsrchr(path, L'\\');
            size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
            memcpy(archived, path, prefix * sizeof(wchar_t));
            wcscpy_s(archived + prefix, _countof(archived) - prefix, find.cFileName);
            DeleteFileW(archived);
            FindClose(h);
        }
    }
}

static void test_commands_and_blank_input(void) {
    static const char invalid_utf8[] = "\xC2";
    TEST_CHECK(chat_history_is_new_command(" /NEW@Bot_123\xE3\x80\x80"));
    TEST_CHECK(chat_history_is_new_command("\xC2\xA0/new\xC2\xA0"));
    TEST_CHECK(chat_history_is_new_command("\xE2\x80\x83/new\xE2\x80\x83"));
    TEST_CHECK(chat_history_is_new_command("\x1C/new\x1F"));
    TEST_CHECK(chat_history_is_new_command("/new"));
    TEST_CHECK(!chat_history_is_new_command("/new now"));
    TEST_CHECK(!chat_history_is_new_command("/new@bot-name"));
    TEST_CHECK(!chat_history_is_new_command("/new@"));
    TEST_CHECK(!chat_history_is_new_command("/new@bot@other"));
    TEST_CHECK(!chat_history_is_new_command("/new\targ"));
    TEST_CHECK(!chat_history_is_new_command("\xEF\xBC\x8Fnew"));
    TEST_CHECK(!chat_history_is_new_command("/\xEF\xBD\x8E\xEF\xBD\x85\xEF\xBD\x97"));
    TEST_CHECK(!chat_history_is_new_command("\xC0\xAFnew"));
    TEST_CHECK(!chat_history_is_new_command("\xC2/new"));
    TEST_CHECK(chat_history_is_blank_message(""));
    TEST_CHECK(chat_history_is_blank_message(" \t\r\n\v\f\x1C"));
    TEST_CHECK(chat_history_is_blank_message(" \t\xC2\xA0\xE3\x80\x80"));
    TEST_CHECK(!chat_history_is_blank_message("\xEF\xBB\xBF"));
    TEST_CHECK(!chat_history_is_blank_message(invalid_utf8));
}

static void test_strict_load_limits(void) {
    size_t length = CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES + 1;
    char* text = (char*)malloc(length + 1);
    char* json = (char*)malloc(length + 160);
    chat_history_document d;
    TEST_CHECK(text != NULL && json != NULL);
    if (!text || !json) { free(text); free(json); return; }
    memset(text, 'x', length); text[length] = '\0';
    snprintf(json, length + 160,
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"%s\"}]}", text);
    chat_history_document_init(&d);
    TEST_CHECK(chat_history_parse_json(json, &d) != 0 && d.messages == NULL);
    free(text); free(json);
}

static void test_archive_copy_preserves_source_and_policy(void) {
    wchar_t path[MAX_PATH], archive[MAX_PATH + 64]; HANDLE file; DWORD written; char source[8] = {0};
    chat_history_document d;
    archive[0] = L'\0';   /* copy failure leaves the buffer untouched: keep cleanup safe */
    make_temp_path(path, _countof(path), L"copy-history.json"); DeleteFileW(path);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    TEST_CHECK(WriteFile(file, "source", 6, &written, NULL) && written == 6); CloseHandle(file);
    int copied = chat_history_copy_to_archive(path, archive, _countof(archive), NULL) == 0;
    TEST_CHECK(copied);
    TEST_CHECK(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) { TEST_CHECK(ReadFile(file, source, 6, &written, NULL) && written == 6); CloseHandle(file); }
    TEST_CHECK(strcmp(source, "source") == 0); DeleteFileW(path);
    if (copied && archive[0] != L'\0') DeleteFileW(archive);
    make_v2(&d, cJSON_CreateArray());
    TEST_CHECK(chat_history_document_can_resume(&d, 8765));
    d.resume_safe = 0; TEST_CHECK(!chat_history_document_can_resume(&d, 8765));
    d.resume_safe = 1; TEST_CHECK(!chat_history_document_can_resume(&d, 8766));
    cJSON_Delete(d.messages);
}

static void test_v1_invalid_documents_and_nul(void) {
    const char* invalid[] = {
        "[]", "{\"version\":2,\"messages\":[]}", "{\"version\":1.5,\"messages\":[]}",
        "{\"version\":1,\"messages\":[{\"role\":\"system\",\"text\":\"x\"}]}",
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":3}]}",
        "{\"version\":1,\"messages\":[]} trailing",
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"a\\u0000b\"}]}", NULL };
    for (int i = 0; invalid[i]; i++) {
        chat_history_document d; chat_history_document_init(&d);
        TEST_CHECK(chat_history_parse_json(invalid[i], &d) != 0);
        TEST_CHECK(d.messages == NULL); chat_history_document_free(&d);
    }

    {
        chat_history_document d;
        const char* literal_nul = "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"a\\\\u0000b\"}]}";
        chat_history_document_init(&d);
        TEST_CHECK(chat_history_parse_json(literal_nul, &d) == 0);
        TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(d.messages, 0), "text")->valuestring,
            "a\\u0000b") == 0);
        chat_history_document_free(&d);
    }
}

static void test_physical_nul_file_and_quarantine(void) {
    wchar_t path[MAX_PATH], pattern[MAX_PATH + 64]; HANDLE f; DWORD n, error = ERROR_SUCCESS;
    chat_history_document d;
    static const char nul[] = "{\"version\":1,\"messages\":[]}" "\0" "junk";
    make_temp_path(path, _countof(path), L"nul-history.json"); DeleteFileW(path);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_CHECK(f != INVALID_HANDLE_VALUE);
    if (f == INVALID_HANDLE_VALUE) return;
    TEST_CHECK(WriteFile(f, nul, (DWORD)sizeof(nul) - 1, &n, NULL)); CloseHandle(f);
    chat_history_document_init(&d);
    TEST_CHECK(chat_history_load(path, &d, &error) == CHAT_HISTORY_LOAD_INVALID);
    TEST_CHECK(d.messages == NULL && error == ERROR_INVALID_DATA);
    TEST_CHECK(chat_history_quarantine_invalid(path, &error) == 0);
    swprintf_s(pattern, _countof(pattern), L"%s.invalid.*", path);
    WIN32_FIND_DATAW data; HANDLE find = FindFirstFileW(pattern, &data);
    TEST_CHECK(find != INVALID_HANDLE_VALUE);
    if (find != INVALID_HANDLE_VALUE) {
        wchar_t archived[MAX_PATH + 64]; const wchar_t* slash = wcsrchr(path, L'\\'); size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
        memcpy(archived, path, prefix * sizeof(wchar_t)); wcscpy_s(archived + prefix, _countof(archived) - prefix, data.cFileName);
        DeleteFileW(archived); FindClose(find);
    }
}

static void test_file_roundtrip_and_empty_overwrite(void) {
    wchar_t path[MAX_PATH]; DWORD error; chat_history_document d, loaded;
    make_temp_path(path, _countof(path), L"history.json"); DeleteFileW(path);
    chat_history_document_init(&loaded);
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_ABSENT);
    make_v2(&d, cJSON_CreateArray()); append_ui_message(d.messages, "user", "saved");
    TEST_CHECK(chat_history_save(path, &d, &error) == 0);
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_OK);
    TEST_CHECK(cJSON_GetArraySize(loaded.messages) == 1); chat_history_document_free(&loaded);
    cJSON_Delete(d.messages); make_v2(&d, cJSON_CreateArray());
    TEST_CHECK(chat_history_save(path, &d, &error) == 0);
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_OK);
    TEST_CHECK(cJSON_GetArraySize(loaded.messages) == 0);
    chat_history_document_free(&loaded); cJSON_Delete(d.messages); DeleteFileW(path);
}

static void test_limits_and_escaped_size(void) {
    chat_history_document d, loaded; cJSON* a = cJSON_CreateArray(); char text[32]; char* json;
    for (int i = 0; i < 501; i++) { snprintf(text, sizeof(text), "message-%d", i); append_ui_message(a, "user", text); }
    make_v2(&d, a); json = chat_history_serialize(&d);
    TEST_CHECK(json != NULL); chat_history_document_init(&loaded);
    TEST_CHECK(chat_history_parse_json(json, &loaded) == 0);
    TEST_CHECK(cJSON_GetArraySize(loaded.messages) == CHAT_HISTORY_MAX_MESSAGES);
    TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded.messages, 0), "text")->valuestring, "message-1") == 0);
    TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded.messages, 499), "text")->valuestring, "message-500") == 0);
    free(json); chat_history_document_free(&loaded); cJSON_Delete(a);
    a = cJSON_CreateArray(); char* big = (char*)malloc(CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES + 1);
    TEST_CHECK(big != NULL); if (!big) { cJSON_Delete(a); return; }
    memset(big, '"', CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES); big[CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES] = '\0';
    append_ui_message(a, "user", big); append_ui_message(a, "assistant", big); make_v2(&d, a); json = chat_history_serialize(&d);
    TEST_CHECK(json != NULL && strlen(json) <= CHAT_HISTORY_MAX_FILE_BYTES);
    free(json); free(big); cJSON_Delete(a);
}

static void test_aggregate_and_path_contracts(void) {
    cJSON* a = cJSON_CreateArray(); chat_history_document d, loaded; char* text = (char*)malloc(200U * 1024U + 1); char* json;
    wchar_t path[MAX_PATH], small[4]; DWORD error; chat_history_document_init(&loaded);
    TEST_CHECK(text != NULL); if (!text) { cJSON_Delete(a); return; }
    for (int i = 0; i < 3; i++) { memset(text, 'A' + i, 200U * 1024U); text[0] = '0' + i; text[200U * 1024U] = '\0'; append_ui_message(a, "assistant", text); }
    make_v2(&d, a); json = chat_history_serialize(&d); TEST_CHECK(json != NULL);
    TEST_CHECK(chat_history_parse_json(json, &loaded) == 0 && cJSON_GetArraySize(loaded.messages) == 2);
    TEST_CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded.messages, 0), "text")->valuestring[0] == '1');
    TEST_CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded.messages, 1), "text")->valuestring[0] == '2');
    free(json); free(text); cJSON_Delete(a); chat_history_document_free(&loaded);
    TEST_CHECK(chat_history_path_from_config(L"C:\\apps\\config.json", path, _countof(path)) == 0);
    TEST_CHECK(wcscmp(path, L"C:\\apps\\chat-history.json") == 0);
    TEST_CHECK(chat_history_path_from_config(L"config.json", path, _countof(path)) == 0);
    TEST_CHECK(wcscmp(path, L"chat-history.json") == 0);
    TEST_CHECK(chat_history_path_from_config(L"config.json", small, _countof(small)) != 0);
    TEST_CHECK(chat_history_load(NULL, &loaded, &error) == CHAT_HISTORY_LOAD_IO_ERROR && error == ERROR_INVALID_PARAMETER);
    TEST_CHECK(chat_history_quarantine_invalid(NULL, &error) != 0 && error == ERROR_INVALID_PARAMETER);
    make_temp_path(path, _countof(path), L"missing-history.json");
    DeleteFileW(path);
    TEST_CHECK(chat_history_quarantine_invalid(path, &error) != 0);
    TEST_CHECK(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
}

static void test_can_resume_requires_messages(void) {
    chat_history_document d;
    /* Valid v2 header but no messages tree: an incomplete document must not
     * look resumable. */
    make_v2(&d, NULL);
    TEST_CHECK(!chat_history_document_can_resume(&d, 8765));
    /* Port range must match the serializer's 1..65535 contract. */
    d.messages = cJSON_CreateArray();
    d.webui_port = 0;
    TEST_CHECK(!chat_history_document_can_resume(&d, 0));
    d.webui_port = 65536;
    TEST_CHECK(!chat_history_document_can_resume(&d, 65536));
    cJSON_Delete(d.messages);
}

TEST_LIST = {
    { "v1_is_legacy", test_v1_is_legacy },
    { "v2_round_trip", test_v2_round_trip },
    { "strict_v2", test_strict_v2 },
    { "archive", test_archive },
    { "commands_and_blank_input", test_commands_and_blank_input },
    { "strict_load_limits", test_strict_load_limits },
    { "archive_copy_preserves_source_and_policy", test_archive_copy_preserves_source_and_policy },
    { "v1_invalid_documents_and_nul", test_v1_invalid_documents_and_nul },
    { "physical_nul_file_and_quarantine", test_physical_nul_file_and_quarantine },
    { "file_roundtrip_and_empty_overwrite", test_file_roundtrip_and_empty_overwrite },
    { "limits_and_escaped_size", test_limits_and_escaped_size },
    { "aggregate_and_path_contracts", test_aggregate_and_path_contracts },
    { "can_resume_requires_messages", test_can_resume_requires_messages },
    { NULL, NULL }
};
