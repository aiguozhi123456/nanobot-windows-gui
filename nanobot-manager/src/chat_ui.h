#ifndef CHAT_UI_H
#define CHAT_UI_H

#include "ui_core.h"
#include "chat.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void chat_ui_bind(UiPage page, int (*is_zh_fn)(void));

/* health_fn returns 1 if gateway is running; used by send button. */
void chat_ui_set_gateway(int (*health_fn)(void), int webui_port);
void chat_ui_configure_history(const wchar_t* config_path);

void chat_ui_reset_idle(void);
void chat_ui_shutdown(void);

void chat_ui_status(const char* utf8);
void chat_ui_status_i18n(const char* zh, const char* en);

void chat_ui_on_event(WPARAM kind, chat_event* event);
void chat_ui_on_send(int health_running);
void chat_ui_mount_send(UiPage p, UiWidget w, void* ud);
void chat_ui_mount_input(UiPage p, UiWidget w, void* ud);
void chat_ui_mount_scroll(UiPage p, UiWidget w, void* ud);
void chat_ui_unmount_scroll(UiPage p, UiWidget w, void* ud);
void chat_ui_on_gateway_stopped(void);
/* Called on the UI thread to expire a pending session attachment. */
void chat_ui_tick(ULONGLONG now);
/* Publishes coalesced streaming text when a delta made the message UI dirty. */
void chat_ui_publish_tick(void);
/* False after a terminal chat failure until an explicit user retry. */
int chat_ui_can_auto_connect(void);

#ifdef __cplusplus
}
#endif
#endif
