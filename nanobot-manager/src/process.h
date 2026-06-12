#ifndef PROCESS_H
#define PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

char* find_nanobot_cmd(void);
int   nanobot_detect_install(void);
int   nanobot_check_running(int port);
int   nanobot_start(const char* cmd, int* out_pid);
int   nanobot_stop(int pid, int port);
int   find_nanobot_python_pid(void);
int   nanobot_get_process_uptime_seconds(int pid, unsigned long long* out_seconds);
void  nanobot_set_handle(void* handle);
void* nanobot_get_handle(void);
void  nanobot_clear_handle(void);

#ifdef __cplusplus
}
#endif

#endif
