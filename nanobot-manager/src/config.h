#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* nanobot_path;
    int autostart_nanobot;
    int health_check_port;
    int theme_mode;
} app_config;

int  config_load(app_config* cfg);
int  config_save(const app_config* cfg);
void config_free(app_config* cfg);

#ifdef __cplusplus
}
#endif

#endif
