#ifndef AUTOSTART_H
#define AUTOSTART_H

#ifdef __cplusplus
extern "C" {
#endif

int autostart_enable(const char* exe_path);
int autostart_disable(void);
int autostart_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
