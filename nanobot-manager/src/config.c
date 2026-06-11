#include "config.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static char* get_config_path(void) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (!slash) return NULL;
    *slash = '\0';
    char* path = (char*)malloc(MAX_PATH);
    if (!path) return NULL;
    snprintf(path, MAX_PATH, "%s\\config.json", exe);
    return path;
}

int config_load(app_config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->health_check_port = 18790;

    char* path = get_config_path();
    if (!path) return -1;

    FILE* f = fopen(path, "rb");
    free(path);
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 0; }

    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return 0;

    cJSON* jp = cJSON_GetObjectItem(root, "nanobot_path");
    if (jp && cJSON_IsString(jp) && jp->valuestring[0])
        cfg->nanobot_path = _strdup(jp->valuestring);

    cJSON* ja = cJSON_GetObjectItem(root, "autostart_nanobot");
    if (ja && cJSON_IsBool(ja))
        cfg->autostart_nanobot = ja->valueint;

    cJSON* jport = cJSON_GetObjectItem(root, "health_check_port");
    if (jport && cJSON_IsNumber(jport) && jport->valueint > 0 && jport->valueint <= 65535)
        cfg->health_check_port = jport->valueint;

    cJSON_Delete(root);
    return 0;
}

int config_save(const app_config* cfg) {
    char* path = get_config_path();
    if (!path) return -1;

    cJSON* root = cJSON_CreateObject();
    if (cfg->nanobot_path)
        cJSON_AddStringToObject(root, "nanobot_path", cfg->nanobot_path);
    else
        cJSON_AddNullToObject(root, "nanobot_path");
    cJSON_AddBoolToObject(root, "autostart_nanobot", cfg->autostart_nanobot);
    cJSON_AddNumberToObject(root, "health_check_port", cfg->health_check_port);

    char* json = cJSON_Print(root);
    cJSON_Delete(root);

    FILE* f = fopen(path, "wb");
    free(path);
    if (!f) { free(json); return -1; }
    fputs(json, f);
    fclose(f);
    free(json);
    return 0;
}

void config_free(app_config* cfg) {
    if (cfg->nanobot_path) {
        free(cfg->nanobot_path);
        cfg->nanobot_path = NULL;
    }
}
