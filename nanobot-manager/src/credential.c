#include "credential.h"

#include <wincred.h>
#include <stdlib.h>
#include <string.h>

static wchar_t* target_to_wide(const char* target, DWORD* error) {
    if (!target || !target[0]) {
        if (error) *error = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, NULL, 0);
    if (length <= 0) {
        if (error) *error = GetLastError();
        return NULL;
    }
    wchar_t* wide = (wchar_t*)calloc((size_t)length, sizeof(wchar_t));
    if (!wide) {
        if (error) *error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, wide, length)) {
        if (error) *error = GetLastError();
        SecureZeroMemory(wide, (size_t)length * sizeof(wchar_t));
        free(wide);
        return NULL;
    }
    return wide;
}

int credential_store_for_target(const char* target, const char* secret, DWORD* error) {
    DWORD local_error = ERROR_SUCCESS;
    PCREDENTIALW old_credential = NULL;
    BYTE* old_blob = NULL;
    DWORD old_blob_size = 0;
    int had_old = 0;
    size_t length = secret ? strlen(secret) : 0;
    if (!secret || length < 1 || length > CREDENTIAL_SECRET_MAX) {
        if (error) *error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    wchar_t* wide_target = target_to_wide(target, &local_error);
    if (!wide_target) {
        if (error) *error = local_error;
        return -1;
    }
    if (CredReadW(wide_target, CRED_TYPE_GENERIC, 0, &old_credential)) {
        had_old = 1;
        old_blob_size = old_credential->CredentialBlobSize;
        if (old_blob_size) {
            old_blob = (BYTE*)malloc(old_blob_size);
            if (!old_blob) {
                SecureZeroMemory(old_credential->CredentialBlob, old_blob_size);
                CredFree(old_credential);
                old_credential = NULL;
                SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
                free(wide_target);
                if (error) *error = ERROR_NOT_ENOUGH_MEMORY;
                return -1;
            }
            memcpy(old_blob, old_credential->CredentialBlob, old_blob_size);
        }
    } else if (GetLastError() != ERROR_NOT_FOUND) {
        local_error = GetLastError();
        SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
        free(wide_target);
        if (error) *error = local_error;
        return -1;
    }
    if (old_credential) {
        if (old_credential->CredentialBlob && old_credential->CredentialBlobSize)
            SecureZeroMemory(old_credential->CredentialBlob, old_credential->CredentialBlobSize);
        CredFree(old_credential);
    }
    CREDENTIALW credential;
    ZeroMemory(&credential, sizeof(credential));
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = wide_target;
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = (DWORD)length;
    credential.CredentialBlob = (LPBYTE)secret;
    if (!CredWriteW(&credential, 0)) {
        local_error = GetLastError();
        if (old_blob) { SecureZeroMemory(old_blob, old_blob_size); free(old_blob); }
        SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
        free(wide_target);
        if (error) *error = local_error;
        return -1;
    }
    PCREDENTIALW read_back = NULL;
    int result = -1;
    if (CredReadW(wide_target, CRED_TYPE_GENERIC, 0, &read_back) &&
        read_back->CredentialBlobSize == length &&
        read_back->CredentialBlob &&
        memcmp(read_back->CredentialBlob, secret, length) == 0) {
        result = 0;
    } else {
        local_error = read_back ? ERROR_CRC : GetLastError();
        if (local_error == ERROR_SUCCESS) local_error = ERROR_CRC;
    }
    if (read_back) {
        if (read_back->CredentialBlob && read_back->CredentialBlobSize)
            SecureZeroMemory(read_back->CredentialBlob, read_back->CredentialBlobSize);
        CredFree(read_back);
    }
    if (result != 0) {
        int rollback_ok;
        if (had_old) {
            CREDENTIALW rollback;
            ZeroMemory(&rollback, sizeof(rollback));
            rollback.Type = CRED_TYPE_GENERIC;
            rollback.TargetName = wide_target;
            rollback.Persist = CRED_PERSIST_LOCAL_MACHINE;
            rollback.CredentialBlobSize = old_blob_size;
            rollback.CredentialBlob = old_blob;
            rollback_ok = CredWriteW(&rollback, 0) ? 1 : 0;
        } else {
            rollback_ok = CredDeleteW(wide_target, CRED_TYPE_GENERIC, 0) ||
                GetLastError() == ERROR_NOT_FOUND;
        }
        if (!rollback_ok) local_error = ERROR_WRITE_FAULT;
    }
    if (old_blob) { SecureZeroMemory(old_blob, old_blob_size); free(old_blob); }
    SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
    free(wide_target);
    if (error) *error = result == 0 ? ERROR_SUCCESS : local_error;
    return result;
}

int credential_load_for_target(const char* target, char* out, size_t cap, DWORD* error) {
    DWORD local_error = ERROR_SUCCESS;
    if (!out || cap < 2) {
        if (error) *error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    out[0] = '\0';
    wchar_t* wide_target = target_to_wide(target, &local_error);
    if (!wide_target) {
        if (error) *error = local_error;
        return -1;
    }
    PCREDENTIALW credential = NULL;
    if (!CredReadW(wide_target, CRED_TYPE_GENERIC, 0, &credential)) {
        local_error = GetLastError();
        SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
        free(wide_target);
        /* "Absent" is not an error: return 0 with ERROR_SUCCESS, matching
         * credential_delete_for_target's not-found convention. */
        if (error) *error = (local_error == ERROR_NOT_FOUND) ? ERROR_SUCCESS : local_error;
        return local_error == ERROR_NOT_FOUND ? 0 : -1;
    }
    int result = -1;
    if (credential->CredentialBlob && credential->CredentialBlobSize >= 1 &&
        credential->CredentialBlobSize <= CREDENTIAL_SECRET_MAX && credential->CredentialBlobSize < cap) {
        memcpy(out, credential->CredentialBlob, credential->CredentialBlobSize);
        out[credential->CredentialBlobSize] = '\0';
        result = 1;
    } else {
        /* Range first, capacity second: only a blob that is valid data but
         * does not fit the caller's buffer is a capacity problem. A NULL/
         * empty blob or one larger than CREDENTIAL_SECRET_MAX is bad data a
         * bigger buffer can never fix. */
        local_error = (credential->CredentialBlob && credential->CredentialBlobSize >= 1 &&
                       credential->CredentialBlobSize <= CREDENTIAL_SECRET_MAX &&
                       credential->CredentialBlobSize >= cap)
            ? ERROR_INSUFFICIENT_BUFFER : ERROR_INVALID_DATA;
    }
    if (credential->CredentialBlob && credential->CredentialBlobSize)
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
    free(wide_target);
    if (error) *error = result == 1 ? ERROR_SUCCESS : local_error;
    return result;
}

int credential_delete_for_target(const char* target, DWORD* error) {
    DWORD local_error = ERROR_SUCCESS;
    wchar_t* wide_target = target_to_wide(target, &local_error);
    if (!wide_target) {
        if (error) *error = local_error;
        return -1;
    }
    int result = 0;
    if (!CredDeleteW(wide_target, CRED_TYPE_GENERIC, 0)) {
        local_error = GetLastError();
        if (local_error != ERROR_NOT_FOUND) result = -1;
    }
    SecureZeroMemory(wide_target, (wcslen(wide_target) + 1) * sizeof(wchar_t));
    free(wide_target);
    if (error) *error = result == 0 ? ERROR_SUCCESS : local_error;
    return result;
}

int credential_store(const char* secret, DWORD* error) {
    return credential_store_for_target(CREDENTIAL_TARGET, secret, error);
}

int credential_load(char* out, size_t cap, DWORD* error) {
    return credential_load_for_target(CREDENTIAL_TARGET, out, cap, error);
}

int credential_delete(DWORD* error) {
    return credential_delete_for_target(CREDENTIAL_TARGET, error);
}
