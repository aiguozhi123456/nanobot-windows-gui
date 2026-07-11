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

void config_apply_defaults(app_config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->health_check_port = 18790;
    cfg->theme_mode = 0;
    cfg->webui_port = 8765;
    cfg->webui_auth[0] = '\0';
    strcpy(cfg->language, "zh");
}

int config_parse_json(app_config* cfg, const char* json) {
    if (!cfg || !json) return -1;
    cJSON* root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON* jp = cJSON_GetObjectItem(root, "nanobot_path");
    if (jp && cJSON_IsString(jp) && jp->valuestring[0]) {
        free(cfg->nanobot_path);            /* avoid leak if parsed twice */
        cfg->nanobot_path = _strdup(jp->valuestring);
    }

    cJSON* ja = cJSON_GetObjectItem(root, "autostart_nanobot");
    if (ja && cJSON_IsBool(ja))
        cfg->autostart_nanobot = ja->valueint;

    cJSON* jt = cJSON_GetObjectItem(root, "stay_in_tray");
    if (jt && cJSON_IsBool(jt))
        cfg->stay_in_tray = jt->valueint;

    cJSON* jport = cJSON_GetObjectItem(root, "health_check_port");
    if (jport && cJSON_IsNumber(jport) && jport->valueint > 0 && jport->valueint <= 65535)
        cfg->health_check_port = jport->valueint;

    cJSON* jtm = cJSON_GetObjectItem(root, "theme_mode");
    if (jtm && cJSON_IsNumber(jtm) && jtm->valueint >= 0 && jtm->valueint <= 2)
        cfg->theme_mode = jtm->valueint;

    cJSON* jlang = cJSON_GetObjectItem(root, "language");
    if (jlang && cJSON_IsString(jlang) && jlang->valuestring[0]) {
        strncpy(cfg->language, jlang->valuestring, sizeof(cfg->language) - 1);
        cfg->language[sizeof(cfg->language) - 1] = '\0';
    }

    cJSON* jwp = cJSON_GetObjectItem(root, "webui_port");
    if (jwp && cJSON_IsNumber(jwp) && jwp->valueint > 0 && jwp->valueint <= 65535)
        cfg->webui_port = jwp->valueint;

    cJSON* jwa = cJSON_GetObjectItem(root, "webui_auth");
    if (jwa && cJSON_IsString(jwa) && jwa->valuestring) {
        strncpy(cfg->webui_auth, jwa->valuestring, sizeof(cfg->webui_auth) - 1);
        cfg->webui_auth[sizeof(cfg->webui_auth) - 1] = '\0';
    }

    cJSON_Delete(root);
    return 0;
}

char* config_serialize_json(const app_config* cfg) {
    if (!cfg) return NULL;
    cJSON* root = cJSON_CreateObject();
    if (cfg->nanobot_path)
        cJSON_AddStringToObject(root, "nanobot_path", cfg->nanobot_path);
    else
        cJSON_AddNullToObject(root, "nanobot_path");
    cJSON_AddBoolToObject(root, "autostart_nanobot", cfg->autostart_nanobot);
    cJSON_AddBoolToObject(root, "stay_in_tray", cfg->stay_in_tray);
    cJSON_AddNumberToObject(root, "health_check_port", cfg->health_check_port);
    cJSON_AddNumberToObject(root, "theme_mode", cfg->theme_mode);
    cJSON_AddStringToObject(root, "language", cfg->language);
    cJSON_AddNumberToObject(root, "webui_port", cfg->webui_port);
    cJSON_AddStringToObject(root, "webui_auth", cfg->webui_auth);

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

int config_load(app_config* cfg) {
    config_apply_defaults(cfg);

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

    config_parse_json(cfg, buf);   /* invalid JSON tolerated: defaults preserved */
    free(buf);
    return 0;
}

int config_save(const app_config* cfg) {
    char* path = get_config_path();
    if (!path) return -1;

    char* json = config_serialize_json(cfg);
    if (!json) { free(path); return -1; }

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
