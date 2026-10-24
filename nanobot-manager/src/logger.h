#ifndef LOGGER_H
#define LOGGER_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NM_LOG_DEBUG = 0,
    NM_LOG_INFO  = 1,
    NM_LOG_WARN  = 2,
    NM_LOG_ERROR = 3
} nm_log_level;

/* Initialize the logger writing <dir>\nanobot-manager.log. dir == NULL means
 * beside the current exe. On any failure the logger degrades to a no-op:
 * logging must never take the app down. Re-init (after shutdown) is allowed.
 * Returns 0 on success, -1 on failure (logger stays usable as a no-op). */
int  logger_init(const wchar_t* dir, nm_log_level level);
void logger_shutdown(void);

/* Runtime level control; takes effect on the next nm_log call. */
void logger_set_level(nm_log_level level);

/* "debug"/"info"/"warn"/"error" (case-insensitive); anything else -> info. */
nm_log_level logger_level_from_string(const char* s);
const char*  logger_level_to_string(nm_log_level level);

/* Rotation threshold override, mainly for tests. Default 512 KiB. */
void logger_set_max_bytes(unsigned long bytes);

/* Line format: [YYYY-MM-DD HH:MM:SS] [LEVEL] [module] message\n (UTF-8).
 * Thread-safe; messages below the active level are dropped. fmt is printf
 * style with UTF-8 text; message must be a single line (no embedded \n). */
void nm_log(nm_log_level level, const char* module, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
