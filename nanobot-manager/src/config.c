#include "config.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static wchar_t g_active_path[MAX_PATH];
static int g_path_selected = 0;
static DWORD g_last_error = ERROR_SUCCESS;
static volatile LONG g_temp_counter = 0;

typedef enum {
    CONFIG_FILE_ABSENT,
    CONFIG_FILE_VALID,
    CONFIG_FILE_INVALID,
    CONFIG_FILE_IO_ERROR
} config_file_status;

static int path_is_under(const wchar_t* path, const wchar_t* root) {
    size_t len;
    if (!path || !root || !root[0]) return 0;
    len = wcslen(root);
    return _wcsnicmp(path, root, len) == 0
        && (path[len] == L'\0' || path[len] == L'\\' || root[len - 1] == L'\\');
}

static int registry_install_matches(const wchar_t* exe_dir) {
    static const wchar_t* const keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\nanobot-manager",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\nanobot-manager_is1"
    };
    HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    REGSAM views[] = { 0, KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (int r = 0; r < (int)_countof(roots); r++) {
        for (int v = 0; v < (int)_countof(views); v++) {
            for (int k = 0; k < (int)_countof(keys); k++) {
                HKEY key = NULL;
                wchar_t location[MAX_PATH];
                DWORD type = 0, bytes = sizeof(location);
                if (RegOpenKeyExW(roots[r], keys[k], 0, KEY_READ | views[v], &key) != ERROR_SUCCESS)
                    continue;
                LONG rc = RegQueryValueExW(key, L"InstallLocation", NULL, &type,
                                            (BYTE*)location, &bytes);
                RegCloseKey(key);
                if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    location[_countof(location) - 1] = L'\0';
                    if (type == REG_EXPAND_SZ) {
                        wchar_t expanded[MAX_PATH];
                        DWORD expanded_length = ExpandEnvironmentStringsW(
                            location, expanded, _countof(expanded));
                        if (expanded_length > 0 && expanded_length <= _countof(expanded))
                            wcscpy_s(location, _countof(location), expanded);
                    }
                    while (wcslen(location) > 0 && location[wcslen(location) - 1] == L'\\')
                        location[wcslen(location) - 1] = L'\0';
                    if (_wcsicmp(exe_dir, location) == 0) return 1;
                }
            }
        }
    }
    return 0;
}

static int exe_is_in_program_files(const wchar_t* exe_dir) {
    const KNOWNFOLDERID ids[] = { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 };
    for (int i = 0; i < (int)_countof(ids); i++) {
        PWSTR folder = NULL;
        HRESULT hr = SHGetKnownFolderPath(&ids[i], 0, NULL, &folder);
        if (SUCCEEDED(hr) && folder) {
            int result = path_is_under(exe_dir, folder);
            CoTaskMemFree(folder);
            if (result) return 1;
        } else if (folder) {
            CoTaskMemFree(folder);
        }
    }
    return 0;
}

static int file_exists(const wchar_t* path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int path_is_legacy_adjacent(const wchar_t* path) {
    wchar_t exe[MAX_PATH];
    wchar_t* slash;
    if (!path || !GetModuleFileNameW(NULL, exe, _countof(exe))) return 0;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return 0;
    *slash = L'\0';
    wchar_t legacy[MAX_PATH];
    swprintf_s(legacy, _countof(legacy), L"%s\\config.json", exe);
    return _wcsicmp(path, legacy) == 0;
}

static void get_legacy_adjacent_path(wchar_t path[MAX_PATH]) {
    wchar_t exe[MAX_PATH];
    path[0] = L'\0';
    if (!GetModuleFileNameW(NULL, exe, _countof(exe))) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *slash = L'\0';
    swprintf_s(path, MAX_PATH, L"%s\\config.json", exe);
}

static void select_config_path(void) {
    wchar_t exe_dir[MAX_PATH], portable_flag[MAX_PATH], legacy[MAX_PATH];
    if (g_path_selected) return;
    g_path_selected = 1;
    g_active_path[0] = L'\0';
    if (!GetModuleFileNameW(NULL, exe_dir, _countof(exe_dir))) {
        g_last_error = GetLastError();
        return;
    }
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (!slash) { g_last_error = ERROR_BAD_PATHNAME; return; }
    *slash = L'\0';
    swprintf_s(portable_flag, _countof(portable_flag), L"%s\\portable.flag", exe_dir);
    swprintf_s(legacy, _countof(legacy), L"%s\\config.json", exe_dir);

    if (file_exists(portable_flag) ||
        (!exe_is_in_program_files(exe_dir) && !registry_install_matches(exe_dir))) {
        wcscpy_s(g_active_path, _countof(g_active_path), legacy);
        return;
    }

    PWSTR local_app_data = NULL;
    HRESULT app_data_hr = SHGetKnownFolderPath(
        &FOLDERID_LocalAppData, 0, NULL, &local_app_data);
    if (FAILED(app_data_hr) || !local_app_data) {
        if (local_app_data) CoTaskMemFree(local_app_data);
        g_last_error = ERROR_PATH_NOT_FOUND;
        return;
    }
    wchar_t directory[MAX_PATH];
    swprintf_s(directory, _countof(directory), L"%s\\nanobot-manager", local_app_data);
    CoTaskMemFree(local_app_data);
    if (!CreateDirectoryW(directory, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) { g_last_error = err; return; }
    }
    swprintf_s(g_active_path, _countof(g_active_path), L"%s\\config.json", directory);
}

DWORD config_get_last_error(void) { return g_last_error; }
const wchar_t* config_get_active_path(void) { select_config_path(); return g_active_path; }

void config_apply_defaults(app_config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->health_check_port = 18790;
    cfg->theme_mode = 0;
    cfg->hotkey_enabled = 1;
    cfg->webui_port = 0;    /* 0 = auto-detect from nanobot's config */
    strcpy(cfg->language, "zh");
}

int config_parse_port(const char* text, int* out_port) {
    unsigned int value = 0;
    if (!text || !text[0] || !out_port) return 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        if (value > 65535U / 10U) return 0;
        value = value * 10U + (unsigned int)(*p - '0');
        if (value > 65535U) return 0;
    }
    if (value == 0) return 0;
    *out_port = (int)value;
    return 1;
}

int config_parse_json(app_config* cfg, const char* json) {
    if (!cfg || !json) return -1;
    cJSON* root = cJSON_Parse(json);
    if (!root) return -1;
    cJSON* item = cJSON_GetObjectItem(root, "nanobot_path");
    if (item && cJSON_IsString(item) && item->valuestring[0]) {
        free(cfg->nanobot_path); cfg->nanobot_path = _strdup(item->valuestring);
    }
    item = cJSON_GetObjectItem(root, "autostart_nanobot");
    if (item && cJSON_IsBool(item)) cfg->autostart_nanobot = item->valueint;
    item = cJSON_GetObjectItem(root, "stay_in_tray");
    if (item && cJSON_IsBool(item)) cfg->stay_in_tray = item->valueint;
    item = cJSON_GetObjectItem(root, "hotkey_enabled");
    if (item && cJSON_IsBool(item)) cfg->hotkey_enabled = item->valueint;
    item = cJSON_GetObjectItem(root, "health_check_port");
    if (item && cJSON_IsNumber(item) && item->valuedouble == item->valueint && item->valueint > 0 && item->valueint <= 65535)
        cfg->health_check_port = item->valueint;
    item = cJSON_GetObjectItem(root, "theme_mode");
    if (item && cJSON_IsNumber(item) && item->valuedouble == item->valueint && item->valueint >= 0 && item->valueint <= 2)
        cfg->theme_mode = item->valueint;
    item = cJSON_GetObjectItem(root, "language");
    if (item && cJSON_IsString(item) && item->valuestring[0] &&
        (strcmp(item->valuestring, "zh") == 0 || strcmp(item->valuestring, "en") == 0)) {
        strcpy(cfg->language, item->valuestring);
    }
    item = cJSON_GetObjectItem(root, "webui_port");
    if (item && cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
        item->valueint >= 0 && item->valueint <= 65535) {
        /* 0 = auto-detect. Managers before auto-detection always serialized
         * the old default 8765, which equals nanobot's own default -- normalize
         * it to auto so pre-existing configs keep following nanobot's config. */
        cfg->webui_port = (item->valueint == 8765) ? 0 : item->valueint;
    }
    item = cJSON_GetObjectItem(root, "webui_auth");
    if (item && cJSON_IsString(item) && item->valuestring) {
        size_t length = strlen(item->valuestring);
        if (length >= sizeof(cfg->legacy_webui_auth)) {
            SecureZeroMemory(cfg->legacy_webui_auth, sizeof(cfg->legacy_webui_auth));
            cfg->legacy_migration_pending = 2;
        } else {
            memcpy(cfg->legacy_webui_auth, item->valuestring, length + 1);
            cfg->legacy_migration_pending = length ? 1 : 0;
        }
    }
    if (item && cJSON_IsString(item) && item->valuestring) {
        SecureZeroMemory(item->valuestring, strlen(item->valuestring));
    }
    cJSON_Delete(root);
    return 0;
}

int config_remove_legacy_auth_json(const char* json, char** out_json) {
    if (!json || !out_json) return -1;
    *out_json = NULL;
    cJSON* root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON* secret = cJSON_DetachItemFromObject(root, "webui_auth");
    if (secret) {
        if (cJSON_IsString(secret) && secret->valuestring)
            SecureZeroMemory(secret->valuestring, strlen(secret->valuestring));
        cJSON_Delete(secret);
    }
    *out_json = cJSON_Print(root);
    cJSON_Delete(root);
    return *out_json ? 0 : -1;
}

char* config_serialize_json(const app_config* cfg) {
    if (!cfg) return NULL;
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;
    if (cfg->nanobot_path) cJSON_AddStringToObject(root, "nanobot_path", cfg->nanobot_path);
    else cJSON_AddNullToObject(root, "nanobot_path");
    cJSON_AddBoolToObject(root, "autostart_nanobot", cfg->autostart_nanobot);
    cJSON_AddBoolToObject(root, "stay_in_tray", cfg->stay_in_tray);
    cJSON_AddBoolToObject(root, "hotkey_enabled", cfg->hotkey_enabled);
    cJSON_AddNumberToObject(root, "health_check_port", cfg->health_check_port);
    cJSON_AddNumberToObject(root, "theme_mode", cfg->theme_mode);
    cJSON_AddStringToObject(root, "language", cfg->language);
    cJSON_AddNumberToObject(root, "webui_port", cfg->webui_port);
    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

static config_file_status read_config_file(app_config* cfg, const wchar_t* path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return CONFIG_FILE_ABSENT;
        g_last_error = error;
        return CONFIG_FILE_IO_ERROR;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return CONFIG_FILE_INVALID;

    FILE* file = _wfopen(path, L"rb");
    if (!file) {
        g_last_error = GetLastError();
        if (g_last_error == ERROR_SUCCESS) g_last_error = ERROR_OPEN_FAILED;
        return CONFIG_FILE_IO_ERROR;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        g_last_error = ERROR_READ_FAULT;
        return CONFIG_FILE_IO_ERROR;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        g_last_error = ERROR_READ_FAULT;
        return CONFIG_FILE_IO_ERROR;
    }
    if (length == 0) { fclose(file); return CONFIG_FILE_INVALID; }
    char* text = (char*)malloc((size_t)length + 1);
    if (!text) { fclose(file); g_last_error = ERROR_NOT_ENOUGH_MEMORY; return CONFIG_FILE_IO_ERROR; }
    size_t read = fread(text, 1, (size_t)length, file);
    int read_error = ferror(file);
    int close_result = fclose(file);
    if (read != (size_t)length || read_error || close_result != 0) {
        SecureZeroMemory(text, (size_t)length + 1);
        free(text);
        g_last_error = ERROR_READ_FAULT;
        return CONFIG_FILE_IO_ERROR;
    }
    text[length] = '\0';
    int parsed = config_parse_json(cfg, text);
    SecureZeroMemory(text, (size_t)length + 1);
    free(text);
    return parsed == 0 ? CONFIG_FILE_VALID : CONFIG_FILE_INVALID;
}

static config_file_status scan_legacy_secret(app_config* cfg, const wchar_t* path) {
    app_config scanned;
    config_apply_defaults(&scanned);
    config_file_status status = read_config_file(&scanned, path);
    if (status == CONFIG_FILE_VALID && scanned.legacy_migration_pending) {
        SecureZeroMemory(cfg->legacy_webui_auth, sizeof(cfg->legacy_webui_auth));
        memcpy(cfg->legacy_webui_auth, scanned.legacy_webui_auth,
               sizeof(cfg->legacy_webui_auth));
        cfg->legacy_migration_pending = scanned.legacy_migration_pending;
        wcsncpy_s(cfg->legacy_source_path, _countof(cfg->legacy_source_path), path, _TRUNCATE);
    }
    config_free(&scanned);
    return status;
}

int config_should_scan_adjacent_legacy(int active_legacy_migration_pending) {
    return active_legacy_migration_pending == 0;
}

/* Remove orphaned config temp files left by a process that died between
 * CreateFileW(CREATE_NEW) and the MoveFileExW rename. The atomic-save helpers
 * name temps "<active>.<pid>.<tick>.<counter>.tmp"; each save picks a fresh
 * suffix, so a crash strands one forever and they accumulate across runs. We
 * sweep the config directory for files matching "<active_base>.*.tmp" and drop
 * them. Best-effort: any failure is silent (the next save still works). */
static void cleanup_orphaned_temps(void) {
    wchar_t dir[MAX_PATH];
    wchar_t base[MAX_PATH];
    const wchar_t* fname;
    wchar_t pattern[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (!g_active_path[0]) return;
    /* Split the active path into directory + file name. */
    wcsncpy_s(dir, _countof(dir), g_active_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    fname = slash + 1;
    if (!fname[0]) return;
    /* base = active filename with its extension stripped (e.g. "config"). */
    wcsncpy_s(base, _countof(base), fname, _TRUNCATE);
    {
        wchar_t* bdot = wcsrchr(base, L'.');
        if (bdot) *bdot = L'\0';
    }
    *slash = L'\0';   /* dir now holds the directory only */

    swprintf_s(pattern, _countof(pattern), L"%s\\%s.*.tmp", dir, base);
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t full[MAX_PATH * 2];
        swprintf_s(full, _countof(full), L"%s\\%s", dir, fd.cFileName);
        DeleteFileW(full);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

int config_load(app_config* cfg) {
    if (!cfg) return -1;
    config_apply_defaults(cfg);
    g_last_error = ERROR_SUCCESS;
    select_config_path();
    if (!g_active_path[0]) return -1;
    cleanup_orphaned_temps();
    config_file_status active = read_config_file(cfg, g_active_path);
    if (active == CONFIG_FILE_VALID || active == CONFIG_FILE_INVALID) {
        if (cfg->legacy_migration_pending)
            wcsncpy_s(cfg->legacy_source_path, _countof(cfg->legacy_source_path), g_active_path, _TRUNCATE);
        /* Installed mode scans the old adjacent file for a lingering secret
         * even when the LocalAppData config is authoritative. */
        if (!path_is_legacy_adjacent(g_active_path) &&
            config_should_scan_adjacent_legacy(cfg->legacy_migration_pending)) {
            wchar_t legacy[MAX_PATH];
            get_legacy_adjacent_path(legacy);
            if (legacy[0]) scan_legacy_secret(cfg, legacy);
        }
        return 0;                         /* corrupt active config never revives legacy */
    }
    if (active == CONFIG_FILE_IO_ERROR)
        return -1;

    /* Installed mode only: migrate an old adjacent file if the user file is absent. */
    wchar_t legacy[MAX_PATH];
    get_legacy_adjacent_path(legacy);
    if (legacy[0]) {
        if (_wcsicmp(legacy, g_active_path) != 0) {
            config_file_status legacy_status = read_config_file(cfg, legacy);
            if (legacy_status == CONFIG_FILE_VALID) {
                if (cfg->legacy_migration_pending)
                    wcsncpy_s(cfg->legacy_source_path, _countof(cfg->legacy_source_path), legacy, _TRUNCATE);
                /* The old install location can be read-only (notably Program
                 * Files), so retain it as a recovery source rather than trying
                 * to rename it. A failed user-config write must stop startup. */
                return cfg->legacy_migration_pending ? 0 : config_save(cfg);
            }
            if (legacy_status == CONFIG_FILE_IO_ERROR)
                return -1;
        }
    }
    return 0;
}

int config_scrub_legacy_auth(const wchar_t* path) {
    if (!path || !path[0]) { g_last_error = ERROR_INVALID_PARAMETER; return -1; }
    FILE* file = _wfopen(path, L"rb");
    if (!file) { g_last_error = GetLastError(); return -1; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); g_last_error = ERROR_READ_FAULT; return -1; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); g_last_error = ERROR_READ_FAULT; return -1; }
    char* input = (char*)malloc((size_t)length + 1);
    if (!input) { fclose(file); g_last_error = ERROR_NOT_ENOUGH_MEMORY; return -1; }
    size_t read = fread(input, 1, (size_t)length, file);
    int close_result = fclose(file);
    input[length] = '\0';
    char* output = NULL;
    int ok = read == (size_t)length && close_result == 0 &&
        config_remove_legacy_auth_json(input, &output) == 0;
    SecureZeroMemory(input, (size_t)length + 1);
    free(input);
    if (!ok) { free(output); g_last_error = ERROR_INVALID_DATA; return -1; }
    wchar_t temp[MAX_PATH * 2];
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; attempt++) {
        swprintf_s(temp, _countof(temp), L"%s.%lu.%llu.%ld.tmp", path,
            (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(),
            (long)InterlockedIncrement(&g_temp_counter));
        handle = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (handle != INVALID_HANDLE_VALUE) break;
    }
    BOOL written = handle != INVALID_HANDLE_VALUE;
    size_t remaining = strlen(output);
    const char* cursor = output;
    while (written && remaining) {
        DWORD count = 0, requested = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
        if (!WriteFile(handle, cursor, requested, &count, NULL) || count == 0) { written = FALSE; break; }
        cursor += count; remaining -= count;
    }
    if (written && !FlushFileBuffers(handle)) written = FALSE;
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    SecureZeroMemory(output, strlen(output));
    free(output);
    if (!written || !MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        g_last_error = GetLastError(); DeleteFileW(temp); return -1;
    }
    g_last_error = ERROR_SUCCESS;
    return 0;
}

int config_scrub_adjacent_legacy_auth(void) {
    wchar_t adjacent[MAX_PATH];
    get_legacy_adjacent_path(adjacent);
    if (!adjacent[0]) { g_last_error = ERROR_BAD_PATHNAME; return -1; }
    return config_scrub_legacy_auth(adjacent);
}

int config_adjacent_legacy_auth_present(int* out_present) {
    wchar_t adjacent[MAX_PATH];
    app_config scanned;
    config_file_status status;
    if (!out_present) { g_last_error = ERROR_INVALID_PARAMETER; return -1; }
    *out_present = 0;
    get_legacy_adjacent_path(adjacent);
    if (!adjacent[0]) { g_last_error = ERROR_BAD_PATHNAME; return -1; }
    config_apply_defaults(&scanned);
    status = read_config_file(&scanned, adjacent);
    if (status == CONFIG_FILE_ABSENT) {
        config_free(&scanned);
        g_last_error = ERROR_SUCCESS;
        return 0;
    }
    if (status == CONFIG_FILE_IO_ERROR) { config_free(&scanned); return -1; }
    if (status == CONFIG_FILE_INVALID) {
        config_free(&scanned);
        g_last_error = ERROR_INVALID_DATA;
        return -1;
    }
    if (status == CONFIG_FILE_VALID)
        *out_present = scanned.legacy_migration_pending ? 1 : 0;
    config_free(&scanned);
    return 0;
}

int config_save(const app_config* cfg) {
    if (!cfg) { g_last_error = ERROR_INVALID_PARAMETER; return -1; }
    if (cfg->legacy_migration_pending) { g_last_error = ERROR_BUSY; return -1; }
    select_config_path();
    if (!g_active_path[0]) return -1;
    char* json = config_serialize_json(cfg);
    if (!json) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return -1; }
    wchar_t temp[MAX_PATH * 2];
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; attempt++) {
        int length = swprintf_s(temp, _countof(temp), L"%s.%lu.%llu.%ld.tmp",
            g_active_path, (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(), (long)InterlockedIncrement(&g_temp_counter));
        if (length <= 0) {
            g_last_error = ERROR_BUFFER_OVERFLOW;
            free(json);
            return -1;
        }
        file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        g_last_error = GetLastError();
        if (g_last_error != ERROR_FILE_EXISTS && g_last_error != ERROR_ALREADY_EXISTS) {
            free(json);
            return -1;
        }
    }
    if (file == INVALID_HANDLE_VALUE) { free(json); return -1; }
    const char* cursor = json;
    size_t remaining = strlen(json);
    BOOL ok = TRUE;
    while (remaining > 0) {
        DWORD requested = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
        DWORD written = 0;
        if (!WriteFile(file, cursor, requested, &written, NULL)) {
            g_last_error = GetLastError(); ok = FALSE; break;
        }
        if (written == 0) { g_last_error = ERROR_WRITE_FAULT; ok = FALSE; break; }
        cursor += written;
        remaining -= written;
    }
    if (ok && !FlushFileBuffers(file)) { g_last_error = GetLastError(); ok = FALSE; }
    if (!CloseHandle(file)) {
        DWORD close_error = GetLastError();
        if (ok) { g_last_error = close_error; ok = FALSE; }
    }
    free(json);
    if (!ok) {
        DeleteFileW(temp);
        return -1;
    }
    if (!MoveFileExW(temp, g_active_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        g_last_error = GetLastError(); DeleteFileW(temp); return -1;
    }
    g_last_error = ERROR_SUCCESS;
    return 0;
}

void config_free(app_config* cfg) {
    if (!cfg) return;
    if (cfg->nanobot_path) { free(cfg->nanobot_path); cfg->nanobot_path = NULL; }
    SecureZeroMemory(cfg->legacy_webui_auth, sizeof(cfg->legacy_webui_auth));
    SecureZeroMemory(cfg->legacy_source_path, sizeof(cfg->legacy_source_path));
    cfg->legacy_migration_pending = 0;
}
