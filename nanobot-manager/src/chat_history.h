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

typedef struct {
    cJSON* messages;
    int version; /* 1 is a valid, unbound legacy document. */
    int webui_port;
    char chat_id[65];
    int resume_safe;
} chat_history_document;

int chat_history_path_from_config(const wchar_t* config_path, wchar_t* out_path, size_t out_count);
void chat_history_document_init(chat_history_document* document);
void chat_history_document_free(chat_history_document* document);
int chat_history_is_valid_chat_id(const char* chat_id);
int chat_history_document_can_resume(const chat_history_document* document, int webui_port);
chat_history_load_result chat_history_load(const wchar_t* path, chat_history_document* out_document, DWORD* out_error);
int chat_history_quarantine_invalid(const wchar_t* path, DWORD* out_error);
int chat_history_archive(const wchar_t* path, DWORD* out_error);
int chat_history_copy_to_archive(const wchar_t* path, wchar_t* out_archive_path, size_t out_count, DWORD* out_error);
int chat_history_parse_json(const char* json, chat_history_document* out_document);
char* chat_history_serialize(const chat_history_document* document);
int chat_history_save(const wchar_t* path, const chat_history_document* document, DWORD* out_error);
int chat_history_is_new_command(const char* text);
int chat_history_is_blank_message(const char* text);

#ifdef __cplusplus
}
#endif
#endif
