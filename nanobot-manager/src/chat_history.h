#ifndef CHAT_HISTORY_H
#define CHAT_HISTORY_H

#include <stddef.h>
#include <windows.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAT_HISTORY_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define CHAT_HISTORY_MAX_MESSAGES 500
#define CHAT_HISTORY_MAX_MESSAGE_TEXT_BYTES (256U * 1024U)
#define CHAT_HISTORY_MAX_TOTAL_TEXT_BYTES (512U * 1024U)

typedef enum {
    CHAT_HISTORY_LOAD_OK,
    CHAT_HISTORY_LOAD_ABSENT,
    CHAT_HISTORY_LOAD_INVALID,
    CHAT_HISTORY_LOAD_IO_ERROR
} chat_history_load_result;

int chat_history_path_from_config(const wchar_t* config_path, wchar_t* out_path, size_t out_count);
chat_history_load_result chat_history_load(const wchar_t* path, cJSON** out_messages, DWORD* out_error);
int chat_history_quarantine_invalid(const wchar_t* path, DWORD* out_error);
int chat_history_parse_json(const char* json, cJSON** out_messages);
char* chat_history_serialize(const cJSON* ui_messages);
int chat_history_save(const wchar_t* path, const cJSON* ui_messages, DWORD* out_error);
int chat_history_is_new_command(const char* text);
int chat_history_is_blank_message(const char* text);

#ifdef __cplusplus
}
#endif
#endif
