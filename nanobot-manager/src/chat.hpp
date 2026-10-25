#ifndef CHAT_HPP
#define CHAT_HPP

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

/* Event kinds posted to the UI thread: WPARAM = kind, LPARAM = heap
 * chat_event* whose ownership transfers with the PostMessage. Values are
 * wire-stable (WM_CHAT_EVENT dispatch in main.cpp). */
enum ChatEventKind {
    CHAT_EVT_CONNECTED = 1,       /* socket up; chat_id may still be empty */
    CHAT_EVT_DISCONNECTED = 2,
    CHAT_EVT_DELTA = 3,           /* detail: UTF-8 text chunk */
    CHAT_EVT_MESSAGE = 4,         /* detail: full assistant text */
    CHAT_EVT_STREAM_END = 5,
    CHAT_EVT_ERROR = 6,           /* detail: error text */
    CHAT_EVT_READY = 7,           /* detail: offered default chat_id; not attached */
    CHAT_EVT_DISCONNECT_DONE = 8, /* async disconnect finished */
    CHAT_EVT_TURN_END = 9,
    CHAT_EVT_ATTACHED = 10        /* detail: matching attached chat_id */
};

struct chat_event {
    UINT64 generation;
    char* detail;
};

inline void chat_event_free(chat_event* event) {
    if (!event) return;
    free(event->detail);
    free(event);
}

/* Explicit connection state machine replacing the old g_ready/g_attached/
 * g_connecting volatile flags. Transitions are CAS races by design: a lost
 * CAS (e.g. a connect completing while a disconnect lands) is one of the
 * benign races the old flag set tolerated, never an assertion failure. */
enum class ChatConnState {
    Idle = 0,       /* no socket, not connecting */
    Connecting = 1, /* connect worker running bootstrap + WS upgrade */
    Connected = 2,  /* socket up, chat_id not yet attached */
    Attached = 3    /* chat_id attached, ready to send */
};

const char* chat_conn_state_name(ChatConnState state);

/* WebUI bootstrap + WinHTTP WebSocket chat client.
 *
 * Three workers: connect (one-shot bootstrap + WS upgrade), recv (frame
 * pump), lifecycle (serializes disconnect/reconnect/shutdown requests).
 * Stale events are guarded by a generation counter invalidated on every
 * teardown. Thread ownership mirrors the original C module exactly:
 *   - disconnect joins connect+recv after cancelling the socket receive;
 *   - connect only harvests *finished* workers (finished_* flags are set
 *     as each thread's last statement, giving std::thread a try-join).
 * All public methods are UI-thread entry points. Every state transition
 * is logged through nm_log (module "chat"). */
class ChatClient {
public:
    /* Once, at window creation. hwnd receives WM_CHAT_EVENT. */
    void init(HWND hwnd, UINT msg_id);

    /* WebUI/WS host port and an optional short-lived bootstrap secret. */
    void setEndpoint(int webui_port, const char* bootstrap_secret);

    int  connect();         /* async; idempotent while connected/connecting */
    void disconnect();      /* synchronous: cancels and joins worker threads */
    void disconnectAsync(); /* lifecycle worker; posts DISCONNECT_DONE */
    void reconnectAsync();  /* lifecycle worker: disconnect, then reconnect */

    bool isConnected() const;  /* socket up (Connected or Attached) */
    bool isReady() const;      /* Attached */
    bool needsConnect() const; /* lifecycle operational and Idle */

    /* Select a server-offered or persisted chat id. Usable once the socket
     * is up; CHAT_EVT_ATTACHED confirms it server-side. */
    int attach(const char* chat_id);

    /* Send user text (UTF-8). 0 on success, -1 if not ready/empty. */
    int send(const char* utf8_text);

    uint64_t currentGeneration() const { return generation_.load(); }

    /* Null the UI target, tear down workers and the lifecycle thread. */
    void shutdown();

    ~ChatClient();
    ChatClient() = default;
    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

private:
    enum LifecycleRequest {
        LIFECYCLE_NONE,
        LIFECYCLE_DISCONNECT,
        LIFECYCLE_RECONNECT,
        LIFECYCLE_SHUTDOWN
    };

    struct ConnectContext {
        UINT64 generation;
    };
    struct RecvContext {
        HINTERNET websocket;
        UINT64 generation;
    };

    /* --- synchronization ------------------------------------------------
     * cs_ guards every non-atomic field below. disconnectMutex_ serializes
     * whole disconnect/reconnect/shutdown operations; lock order is always
     * disconnectMutex_ -> cs_. (mutable: const observers like needsConnect()
     * take it briefly.) */
    mutable std::mutex cs_;
    std::mutex disconnectMutex_;
    std::condition_variable lifecycleCv_;
    bool inited_ = false; /* init() ran; gates every public entry point */

    HWND hwnd_ = NULL;
    UINT msg_id_ = 0;

    /* Endpoint snapshot; secret is zeroed on teardown. */
    int webui_port_ = 8765;
    char bootstrap_secret_[128] = {0};

    /* Transport handles, owned by the connection lifecycle (never closed by
     * the recv worker, which only observes websocket_.get()). */
    HINTERNET session_ = NULL;
    HINTERNET connect_ = NULL;
    HINTERNET websocket_ = NULL;

    /* Workers. A joinable thread with its finished flag set is dead and may
     * be joined instantly; joinable without the flag means still running. */
    std::thread recv_thread_;
    std::thread connect_thread_;
    std::thread lifecycle_thread_;
    std::atomic<bool> recv_finished_{false};
    std::atomic<bool> connect_finished_{false};

    std::atomic<bool> stop_{false};          /* worker cancellation signal */
    std::atomic<uint64_t> generation_{1};    /* stale-event guard */
    std::atomic<ChatConnState> state_{ChatConnState::Idle};

    /* Lifecycle request plumbing (under cs_). */
    LifecycleRequest lifecycle_request_ = LIFECYCLE_NONE;
    bool lifecycle_busy_ = false;
    bool lifecycle_running_ = false;
    bool shutting_down_ = false;

    char chat_id_[128] = {0};
    char attach_id_[128] = {0};

    /* "*Locked" methods require cs_ held, mirroring the old *_unlocked. */
    bool lifecycleOperationalLocked() const;
    bool transitionState(ChatConnState expected, ChatConnState desired);
    void forceStateIdle();
    void cancelWebsocketLocked(HINTERNET websocket);
    void destroyConnectionHandlesLocked();
    void reapFinishedThreadsLocked();
    int connectLocked(uint64_t generation);
    void disconnectInternal(int invalidate_events);
    void requestLifecycle(LifecycleRequest request);

    uint64_t invalidateGeneration();
    void postEvent(WPARAM kind, uint64_t generation, char* detail_or_null);

    int httpGetBootstrap(int port, const char* secret,
                         char* token_out, size_t token_size,
                         char* ws_path_out, size_t ws_path_size);
    int wsConnect(int port, const char* path, const char* token,
                  HINTERNET* session_out, HINTERNET* connect_out,
                  HINTERNET* websocket_out);
    void handleJsonFrame(uint64_t generation, const char* json, size_t len);

    void recvThreadMain(RecvContext context);
    void connectThreadMain(ConnectContext context);
    void lifecycleThreadMain();
};

/* Process-wide instance. Constructed on first use (before any window), so
 * there are no static-init-order hazards; teardown happens in shutdown(). */
ChatClient& chat_client();

#endif
