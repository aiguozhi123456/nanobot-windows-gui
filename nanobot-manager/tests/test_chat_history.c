#include "nm_test.h"
#include "chat_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_temp_path(wchar_t* path, size_t count, const wchar_t* name) {
    wchar_t directory[MAX_PATH];
    GetTempPathW((DWORD)_countof(directory), directory);
    swprintf_s(path, count, L"%snanobot-manager-%lu-%s", directory,
        (unsigned long)GetCurrentProcessId(), name);
}

static void append_ui_message(cJSON* messages, const char* role, const char* text) {
    cJSON* message = cJSON_CreateObject();
    TEST_CHECK(message != NULL);
    if (!message) return;
    TEST_CHECK(cJSON_AddStringToObject(message, "role", role) != NULL);
    TEST_CHECK(cJSON_AddStringToObject(message, "text", text) != NULL);
    TEST_CHECK(cJSON_AddItemToArray(messages, message));
}

static void test_parse_serialize_and_ids(void) {
    const char* source = "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"hello \\u4e16\\u754c\"},{\"role\":\"assistant\",\"text\":\"ok\"}]}";
    cJSON* messages = NULL;
    cJSON* ui = cJSON_CreateArray();
    cJSON* ui_message = cJSON_CreateObject();
    char* serialized;
    TEST_CHECK(chat_history_parse_json(source, &messages) == 0);
    TEST_CHECK(messages && cJSON_GetArraySize(messages) == 2);
    TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "text")->valuestring, "hello 世界") == 0);
    cJSON_AddNumberToObject(ui_message, "id", 42);
    cJSON_AddStringToObject(ui_message, "role", "user");
    cJSON_AddStringToObject(ui_message, "text", "unicode 世界");
    cJSON_AddItemToArray(ui, ui_message);
    serialized = chat_history_serialize(ui);
    TEST_CHECK(serialized && strstr(serialized, "\"id\"") == NULL);
    cJSON_Delete(messages);
    messages = NULL;
    TEST_CHECK(chat_history_parse_json(serialized, &messages) == 0);
    TEST_CHECK(messages && strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "text")->valuestring, "unicode 世界") == 0);
    free(serialized);
    cJSON_Delete(messages);
    cJSON_Delete(ui);
}

static void test_empty_and_commands(void) {
    cJSON* messages = NULL;
    char* long_command;
    TEST_CHECK(chat_history_parse_json("{\"version\":1,\"messages\":[]}", &messages) == 0);
    TEST_CHECK(messages && cJSON_GetArraySize(messages) == 0);
    cJSON_Delete(messages);
    TEST_CHECK(chat_history_is_new_command("/new"));
    TEST_CHECK(chat_history_is_new_command(" \t/new\r\n"));
    TEST_CHECK(chat_history_is_new_command("/NEW"));
    TEST_CHECK(chat_history_is_new_command("/NeW"));
    TEST_CHECK(chat_history_is_new_command("\xC2\xA0/new\xC2\xA0"));
    TEST_CHECK(chat_history_is_new_command("\xE2\x80\x83/new\xE2\x80\x83"));
    TEST_CHECK(chat_history_is_new_command("\xE3\x80\x80/new\xE3\x80\x80"));
    TEST_CHECK(chat_history_is_new_command("\x1C/new\x1F"));
    TEST_CHECK(chat_history_is_new_command("/new@bot"));
    TEST_CHECK(chat_history_is_new_command("/NEW@Bot_123"));
    long_command = (char*)malloc(5000 + strlen("/NEW@Bot_1") + strlen("\xE3\x80\x80") + 1);
    TEST_CHECK(long_command != NULL);
    if (long_command) {
        memset(long_command, ' ', 5000);
        strcpy(long_command + 5000, "/NEW@Bot_1\xE3\x80\x80");
        TEST_CHECK(chat_history_is_new_command(long_command));
        free(long_command);
    }
    TEST_CHECK(!chat_history_is_new_command("/new now"));
    TEST_CHECK(!chat_history_is_new_command("/new@bot args"));
    TEST_CHECK(!chat_history_is_new_command("/new\targ"));
    TEST_CHECK(!chat_history_is_new_command("/new@"));
    TEST_CHECK(!chat_history_is_new_command("/new@bot-name"));
    TEST_CHECK(!chat_history_is_new_command("/new@bot@other"));
    TEST_CHECK(!chat_history_is_new_command("x/new"));
    TEST_CHECK(!chat_history_is_new_command("\xEF\xBC\x8Fnew"));
    TEST_CHECK(!chat_history_is_new_command("/\xEF\xBD\x8E\xEF\xBD\x85\xEF\xBD\x97"));
    TEST_CHECK(!chat_history_is_new_command("\xC0\xAFnew"));
    TEST_CHECK(!chat_history_is_new_command("\xC2/new"));
}

static void test_invalid_documents_and_limits(void) {
    const char* invalid[] = {
        "[]", "{\"version\":2,\"messages\":[]}",
        "{\"version\":1.5,\"messages\":[]}",
        "{\"version\":1,\"messages\":[{\"role\":\"system\",\"text\":\"x\"}]}",
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":3}]}",
        "{\"version\":1,\"messages\":[]} trailing", NULL
    };
    cJSON* messages = NULL;
    char* large = (char*)malloc(CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES + 2);
    for (int index = 0; invalid[index]; index++)
        TEST_CHECK(chat_history_parse_json(invalid[index], &messages) != 0);
    memset(large, 'x', CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES + 1);
    large[CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES + 1] = '\0';
    {
        size_t json_length = strlen(large) + 80;
        char* json = (char*)malloc(json_length);
        snprintf(json, json_length, "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"%s\"}]}", large);
        TEST_CHECK(chat_history_parse_json(json, &messages) != 0);
        free(json);
    }
    free(large);
}

static void test_decoded_nul_escapes_are_invalid(void) {
    const char* decoded_nul =
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"a\\u0000b\"}]}";
    const char* literal_backslash_u0000 =
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"a\\\\u0000b\"}]}";
    const char* backslash_then_decoded_nul =
        "{\"version\":1,\"messages\":[{\"role\":\"user\",\"text\":\"a" "\\\\" "\\u0000" "b\"}]}";
    cJSON* messages = NULL;
    cJSON* text;
    char* serialized;
    TEST_CHECK(chat_history_parse_json(decoded_nul, &messages) != 0);
    TEST_CHECK(messages == NULL);
    TEST_CHECK(chat_history_parse_json(backslash_then_decoded_nul, &messages) != 0);
    TEST_CHECK(messages == NULL);
    TEST_CHECK(chat_history_parse_json(literal_backslash_u0000, &messages) == 0);
    text = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "text");
    TEST_CHECK(text && strcmp(text->valuestring, "a\\u0000b") == 0);
    serialized = chat_history_serialize(messages);
    TEST_CHECK(serialized != NULL);
    cJSON_Delete(messages);
    messages = NULL;
    TEST_CHECK(chat_history_parse_json(serialized, &messages) == 0);
    text = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "text");
    TEST_CHECK(text && strcmp(text->valuestring, "a\\u0000b") == 0);
    free(serialized);
    cJSON_Delete(messages);
}

static void test_file_roundtrip_and_empty_overwrite(void) {
    wchar_t path[MAX_PATH];
    cJSON* ui = cJSON_CreateArray();
    cJSON* message = cJSON_CreateObject();
    cJSON* loaded = NULL;
    DWORD error = ERROR_SUCCESS;
    make_temp_path(path, _countof(path), L"history.json");
    DeleteFileW(path);
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_ABSENT);
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "text", "saved");
    cJSON_AddItemToArray(ui, message);
    TEST_CHECK(chat_history_save(path, ui, &error) == 0);
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_OK);
    TEST_CHECK(loaded && cJSON_GetArraySize(loaded) == 1);
    cJSON_Delete(loaded);
    loaded = NULL;
    cJSON_Delete(ui);
    ui = cJSON_CreateArray();
    TEST_CHECK(chat_history_save(path, ui, &error) == 0);
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_OK);
    TEST_CHECK(loaded && cJSON_GetArraySize(loaded) == 0);
    cJSON_Delete(loaded);
    cJSON_Delete(ui);
    DeleteFileW(path);
}

static void test_embedded_nul_file_is_invalid(void) {
    wchar_t path[MAX_PATH];
    HANDLE file;
    DWORD written;
    DWORD error = ERROR_SUCCESS;
    cJSON* loaded = NULL;
    static const char content[] = "{\"version\":1,\"messages\":[]}" "\0" "junk";
    make_temp_path(path, _countof(path), L"nul-history.json");
    DeleteFileW(path);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    TEST_CHECK(WriteFile(file, content, (DWORD)sizeof(content) - 1, &written, NULL));
    TEST_CHECK(written == sizeof(content) - 1);
    TEST_CHECK(CloseHandle(file));
    TEST_CHECK(chat_history_load(path, &loaded, &error) == CHAT_HISTORY_LOAD_INVALID);
    TEST_CHECK(loaded == NULL);
    TEST_CHECK(error == ERROR_INVALID_DATA);
    DeleteFileW(path);
}

static void test_quarantine_invalid_file(void) {
    wchar_t path[MAX_PATH];
    wchar_t pattern[MAX_PATH + 64];
    WIN32_FIND_DATAW find_data;
    HANDLE find;
    HANDLE file;
    DWORD written;
    DWORD error = ERROR_SUCCESS;
    cJSON* messages = cJSON_CreateArray();
    static const char invalid_content[] = "not valid history";
    make_temp_path(path, _countof(path), L"quarantine-history.json");
    DeleteFileW(path);
    swprintf_s(pattern, _countof(pattern), L"%s.invalid.*", path);
    find = FindFirstFileW(pattern, &find_data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t backup[MAX_PATH + 64];
            const wchar_t* slash = wcsrchr(path, L'\\');
            size_t directory_length = slash ? (size_t)(slash - path + 1) : 0;
            memcpy(backup, path, directory_length * sizeof(wchar_t));
            wcscpy_s(backup + directory_length, _countof(backup) - directory_length, find_data.cFileName);
            DeleteFileW(backup);
        } while (FindNextFileW(find, &find_data));
        FindClose(find);
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) {
        cJSON_Delete(messages);
        return;
    }
    TEST_CHECK(WriteFile(file, invalid_content, (DWORD)sizeof(invalid_content) - 1, &written, NULL));
    TEST_CHECK(written == sizeof(invalid_content) - 1);
    TEST_CHECK(CloseHandle(file));
    TEST_CHECK(chat_history_quarantine_invalid(path, &error) == 0);
    TEST_CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
    find = FindFirstFileW(pattern, &find_data);
    TEST_CHECK(find != INVALID_HANDLE_VALUE);
    if (find != INVALID_HANDLE_VALUE) {
        wchar_t backup[MAX_PATH + 64];
        char saved_content[sizeof(invalid_content)] = {0};
        const wchar_t* slash = wcsrchr(path, L'\\');
        size_t directory_length = slash ? (size_t)(slash - path + 1) : 0;
        memcpy(backup, path, directory_length * sizeof(wchar_t));
        wcscpy_s(backup + directory_length, _countof(backup) - directory_length, find_data.cFileName);
        TEST_CHECK(GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES);
        file = CreateFileW(backup, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        TEST_CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            TEST_CHECK(ReadFile(file, saved_content, (DWORD)sizeof(invalid_content) - 1, &written, NULL));
            TEST_CHECK(written == sizeof(invalid_content) - 1);
            TEST_CHECK(memcmp(saved_content, invalid_content, sizeof(invalid_content) - 1) == 0);
            TEST_CHECK(CloseHandle(file));
        }
        DeleteFileW(backup);
        FindClose(find);
    }
    TEST_CHECK(chat_history_save(path, messages, &error) == 0);
    DeleteFileW(path);
    cJSON_Delete(messages);
}

static void test_serialize_keeps_latest_500_messages(void) {
    cJSON* ui = cJSON_CreateArray();
    cJSON* loaded = NULL;
    char* serialized;
    char text[32];
    for (int index = 0; index < 501; index++) {
        snprintf(text, sizeof(text), "message-%d", index);
        append_ui_message(ui, "user", text);
    }
    serialized = chat_history_serialize(ui);
    TEST_CHECK(serialized != NULL);
    TEST_CHECK(chat_history_parse_json(serialized, &loaded) == 0);
    TEST_CHECK(cJSON_GetArraySize(loaded) == CHAT_HISTORY_MAX_MESSAGES);
    TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded, 0), "text")->valuestring, "message-1") == 0);
    TEST_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded, 499), "text")->valuestring, "message-500") == 0);
    free(serialized);
    cJSON_Delete(loaded);
    cJSON_Delete(ui);
}

static void test_serialize_rolls_total_text_limit(void) {
    cJSON* ui = cJSON_CreateArray();
    cJSON* loaded = NULL;
    char* text = (char*)malloc(200U * 1024U + 1);
    char* serialized;
    TEST_CHECK(text != NULL);
    if (!text) {
        cJSON_Delete(ui);
        return;
    }
    for (int index = 0; index < 3; index++) {
        memset(text, 'A' + index, 200U * 1024U);
        text[0] = '0' + index;
        text[200U * 1024U] = '\0';
        append_ui_message(ui, "assistant", text);
    }
    serialized = chat_history_serialize(ui);
    TEST_CHECK(serialized != NULL);
    TEST_CHECK(chat_history_parse_json(serialized, &loaded) == 0);
    TEST_CHECK(cJSON_GetArraySize(loaded) == 2);
    TEST_CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded, 0), "text")->valuestring[0] == '1');
    TEST_CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(loaded, 1), "text")->valuestring[0] == '2');
    free(serialized);
    free(text);
    cJSON_Delete(loaded);
    cJSON_Delete(ui);
}

static void test_serialize_escaped_text_stays_within_file_limit(void) {
    cJSON* ui = cJSON_CreateArray();
    cJSON* loaded = NULL;
    char* text = (char*)malloc(CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES + 1);
    char* serialized;
    TEST_CHECK(text != NULL);
    if (!text) {
        cJSON_Delete(ui);
        return;
    }
    memset(text, '"', CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES);
    text[CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES] = '\0';
    append_ui_message(ui, "user", text);
    append_ui_message(ui, "assistant", text);
    serialized = chat_history_serialize(ui);
    TEST_CHECK(serialized != NULL);
    TEST_CHECK(strlen(serialized) <= CHAT_HISTORY_MAX_FILE_BYTES);
    TEST_CHECK(chat_history_parse_json(serialized, &loaded) == 0);
    TEST_CHECK(cJSON_GetArraySize(loaded) == 2);
    free(serialized);
    free(text);
    cJSON_Delete(loaded);
    cJSON_Delete(ui);
}

static void test_path_and_invalid_load_contracts(void) {
    wchar_t path[MAX_PATH];
    wchar_t small[4];
    cJSON* loaded = (cJSON*)1;
    DWORD error = ERROR_SUCCESS;
    TEST_CHECK(chat_history_path_from_config(L"C:\\apps\\config.json", path, _countof(path)) == 0);
    TEST_CHECK(wcscmp(path, L"C:\\apps\\chat-history.json") == 0);
    TEST_CHECK(chat_history_path_from_config(L"config.json", path, _countof(path)) == 0);
    TEST_CHECK(wcscmp(path, L"chat-history.json") == 0);
    TEST_CHECK(chat_history_path_from_config(L"C:\\apps\\config.json", small, _countof(small)) != 0);
    TEST_CHECK(chat_history_load(NULL, &loaded, &error) == CHAT_HISTORY_LOAD_IO_ERROR);
    TEST_CHECK(loaded == NULL);
    TEST_CHECK(error == ERROR_INVALID_PARAMETER);
    TEST_CHECK(chat_history_quarantine_invalid(NULL, &error) != 0);
    TEST_CHECK(error == ERROR_INVALID_PARAMETER);
    make_temp_path(path, _countof(path), L"missing-history.json");
    DeleteFileW(path);
    TEST_CHECK(chat_history_quarantine_invalid(path, &error) != 0);
    TEST_CHECK(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    TEST_CHECK(chat_history_parse_json("not json", &loaded) != 0);
    TEST_CHECK(loaded == NULL);
}

TEST_LIST = {
    { "parse_serialize_and_ids", test_parse_serialize_and_ids },
    { "empty_and_commands", test_empty_and_commands },
    { "invalid_documents_and_limits", test_invalid_documents_and_limits },
    { "decoded_nul_escapes_are_invalid", test_decoded_nul_escapes_are_invalid },
    { "file_roundtrip_and_empty_overwrite", test_file_roundtrip_and_empty_overwrite },
    { "embedded_nul_file_is_invalid", test_embedded_nul_file_is_invalid },
    { "quarantine_invalid_file", test_quarantine_invalid_file },
    { "serialize_keeps_latest_500_messages", test_serialize_keeps_latest_500_messages },
    { "serialize_rolls_total_text_limit", test_serialize_rolls_total_text_limit },
    { "serialize_escaped_text_stays_within_file_limit", test_serialize_escaped_text_stays_within_file_limit },
    { "path_and_invalid_load_contracts", test_path_and_invalid_load_contracts },
    { NULL, NULL }
};
