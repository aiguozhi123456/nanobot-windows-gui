#ifndef CREDENTIAL_H
#define CREDENTIAL_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CREDENTIAL_TARGET "nanobot-manager/bootstrap-secret/v1"

/* Store/delete return 0 on success and -1 on failure. Load returns 1 when a
 * value was found, 0 when absent, and -1 on an API/validation error. */
int credential_store(const char* secret, DWORD* error);
int credential_load(char* out, size_t cap, DWORD* error);
int credential_delete(DWORD* error);

int credential_store_for_target(const char* target, const char* secret, DWORD* error);
int credential_load_for_target(const char* target, char* out, size_t cap, DWORD* error);
int credential_delete_for_target(const char* target, DWORD* error);

#ifdef __cplusplus
}
#endif

#endif
