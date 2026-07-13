#ifndef PROCESS_H
#define PROCESS_H

#include <windows.h>
#include "config.h"   /* app_config (used by the resolver) */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Resolution: how the command to run is decided ------------------------- */

typedef enum {
    NM_SRC_NONE = 0,
    NM_SRC_CONFIG,        /* cfg->nanobot_path, existence-validated               */
    NM_SRC_PATH_NANOBOT,  /* SearchPathA(NULL,"nanobot.exe",...)                  */
    NM_SRC_PYTHON,        /* py.exe / python{,3,w}.exe -m nanobot (probe-passed)  */
} nm_source;

typedef struct {
    int         found;      /* == (exe != NULL)                                   */
    char*       exe;        /* malloc'd; NULL if !found                           */
    int         is_python;  /* 1 => "<exe> -m nanobot gateway", else "<exe> gateway" */
    nm_source   source;     /* diagnostic/UI hint only                            */
} nm_resolution;

void nm_resolution_free(nm_resolution* r);   /* frees exe, zeroes struct        */

/* Pure: build ordered candidate list. NO spawning. Uses GetFileAttributesW only.
 * Returns count; caller frees via nm_candidates_free. Testable in isolation. */
int  nanobot_resolve_candidates(const app_config* cfg,
                                char*** out_exes, int** out_is_python, int* out_count);
void nm_candidates_free(char** exes, int* is_python, int count);

/* Thin wrapper: iterates candidates; for the python tier runs a validation probe
 * ("<exe>" -m nanobot --version, ~10s, exit 0 + output check). cfg may be NULL
 * (skips NM_SRC_CONFIG).
 * `quick`: when non-zero, the python tier is accepted on SearchPathA existence
 * alone (no ~10s probe). Use when any nanobot exe suffices to invoke a CLI
 * subcommand (e.g. stop) and the probe cost is not justified. The tradeoff: a
 * python without nanobot installed may be accepted -> the CLI then fails fast
 * and the caller escalates (acceptable for the stop path). */
int  nanobot_resolve(const app_config* cfg, nm_resolution* out, int quick);

/* --- Process lifecycle ----------------------------------------------------- */

int   nanobot_check_running(int port);
int   nanobot_start(const char* exe, int is_python, int* out_pid);

/* nanobot_start distinct failure codes (caller may surface these verbatim).
 * Success == 0; positive values are GetLastError() codes from CreateProcess. */
#define NANOBOT_START_NO_STATEFILE  (-1)   /* launcher ran but gateway.json missing/no pid */
#define NANOBOT_START_OPEN_FAILED   (-2)   /* OpenProcess on state-file pid failed          */
#define NANOBOT_START_STALE_PID     (-3)   /* state-file pid was already dead (stale file)  */

/* Snapshot of ~/.nanobot/run/gateway.json -- the authoritative source for the
 * REAL foreground gateway pid after `nanobot gateway --background`. The stub
 * launcher writes this file then exits; we read it to find the live pid. */
typedef struct {
    int   found;        /* file present + parseable; 0 if missing/unreadable        */
    DWORD pid;          /* real foreground gateway pid; 0 if !found or no pid field */
    int   port;         /* from state file; informational only (manager uses cfg)   */
    /* Windows: decimal FILETIME ull as str(GetProcessTimes create)
     * (upstream `_windows_process_identity`). Prefer over started_at. */
    char  identity[64];
    char  started_at[40]; /* ISO 8601 boot timestamp; fallback PID-reuse check      */
    char  command[512];   /* launch command; M8 derives stop-CLI exe when uncached   */
} nm_gateway_state;

/* Resolve %USERPROFILE%\.nanobot\run\gateway.json and parse it. Returns 0 on
 * success (out->found=0 when the file is missing OR present-but-pid-less).
 * Non-zero only on I/O/allocation error. Never crashes on a malformed file. */
int   nanobot_read_gateway_state(nm_gateway_state* out);

int   nanobot_stop(void);
int   nanobot_get_process_uptime_seconds(int pid, unsigned long long* out_seconds);

/* --- P5a: thread-pool wait on the gateway handle (instant crash detection) -- */

/* Arm a one-shot thread-pool wait on g_state.handle. hwnd receives `msg` when
 * the handle becomes signaled (process exited). No-op (returns 0) if no handle
 * is tracked (HTTP-only liveness fallback). Returns 0 on success, non-zero on
 * RegisterWaitForSingleObject failure. */
int  nanobot_watch_handle(HWND hwnd, UINT msg);

/* Block until any in-flight wait callback completes, then unregister. MUST be
 * called before CloseHandle on g_state.handle on every path that closes it.
 * No-op if no wait is currently registered. */
void nanobot_unwatch_handle(void);

/* Shutdown: unwatch + close handle + zero pid/start_tick. Idempotent. Closing
 * an OpenProcess handle does NOT kill the process -- safe when stop_on_exit is
 * off (the gateway keeps running). */
void nm_state_clear(void);

/* Atomically steal the gateway handle + drain its wait + close. Serialize closes
 * so two threads can never close the same handle. No-op if already cleared. */
void nm_state_close_handle(void);

/* --- P5b: attach to an already-running gateway we didn't start -------------- */

/* Discover an already-running gateway the manager didn't start (state file +
 * OpenProcess), populate g_state.{pid,handle,create_ft,start_tick}, and let the
 * caller arm full liveness. Returns 0 on success (attached), non-zero if no
 * running gateway / open failed (caller falls back to HTTP-only liveness).
 * Idempotent: no-op (returns 0) if handle already set. Does NOT arm
 * nanobot_watch_handle -- the caller does that after success (needs hwnd +
 * WM_CHILD_EXITED which live in main.cpp). */
int  nanobot_attach_running(void);

/* Lifecycle state enum (forward use; P1 does not drive transitions, just declares it). */
typedef enum {
    NM_STATE_UNRESOLVED = 0,
    NM_STATE_RESOLVING,
    NM_STATE_IDLE,
    NM_STATE_STARTING,
    NM_STATE_RUNNING,
    NM_STATE_STOPPING,
    NM_STATE_DEAD,
} nm_state;

/* Read-only snapshot of the guarded process state (caller-owned copy). */
typedef struct {
    nm_state    state;
    int         installed;
    DWORD       pid;
    HANDLE      handle;
    ULONGLONG   start_tick;
    FILETIME    create_ft;          /* PID-reuse guard (P4); zeroed when handle==NULL */
} nm_process_view;

/* Call once at WinMain start, before any worker thread exists. */
void nm_state_init(void);

/* Snapshot under CS (copies all fields). */
void nm_state_snapshot(nm_process_view* out);

/* Quick peek (no full snapshot). */
nm_state nm_state_get(void);
void     nm_state_set_state(nm_state v);   /* transitions begin at P3b */

/* Field accessors -- each takes the CS internally. */
int       nm_state_is_installed(void);
void      nm_state_set_installed(int v);
DWORD     nm_state_get_pid(void);
void      nm_state_set_pid(DWORD v);
HANDLE    nm_state_get_handle(void);
void      nm_state_set_handle(HANDLE v);
ULONGLONG nm_state_get_start_tick(void);
void      nm_state_set_start_tick(ULONGLONG v);

/* P3a additions backing the OpenProcess'd real gateway handle. */
void nm_state_get_create_ft(FILETIME* out);                 /* zeroes *out if NULL     */
void nm_state_set_create_ft(const FILETIME* v);              /* no-op if v==NULL        */
void nm_state_get_resolved_exe(char* buf, size_t buf_size);  /* fills caller's buffer   */
void nm_state_set_resolved_exe(const char* exe);
int  nm_state_get_resolved_is_python(void);
void nm_state_set_resolved_is_python(int v);

/* P5a: thread-pool wait handle (NULL when no wait is armed). */
HANDLE nm_state_get_wait_handle(void);
void   nm_state_set_wait_handle(HANDLE v);

#ifdef __cplusplus
}
#endif

#endif
