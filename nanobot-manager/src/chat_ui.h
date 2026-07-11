#ifndef CHAT_UI_H
#define CHAT_UI_H

#include "ui_core.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void chat_ui_bind(UiPage page, int (*is_zh_fn)(void));

/* health_fn returns 1 if gateway is running; used by send button. */
void chat_ui_set_gateway(int (*health_fn)(void), int webui_port, const char* webui_auth);

void chat_ui_reset_idle(void);
void chat_ui_shutdown(void);

void chat_ui_status(const char* utf8);
void chat_ui_status_i18n(const char* zh, const char* en);

void chat_ui_on_event(WPARAM kind, char* detail);
void chat_ui_on_send(int health_running, int webui_port, const char* webui_auth);
void chat_ui_mount_send(UiPage p, UiWidget w, void* ud);
void chat_ui_on_gateway_stopped(void);

#ifdef __cplusplus
}
#endif
#endif
