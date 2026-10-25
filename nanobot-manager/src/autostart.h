#ifndef AUTOSTART_H
#define AUTOSTART_H

#include <stddef.h>   /* wchar_t */

#ifdef __cplusplus
extern "C" {
#endif

/* exe_path must be the wide exe path (e.g. from GetModuleFileNameW) so
 * non-ASCII install paths survive the registry round-trip. */
int autostart_enable(const wchar_t* exe_path);
int autostart_disable(void);
int autostart_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
