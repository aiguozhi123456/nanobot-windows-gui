# Concepts

Shared domain vocabulary for this project — entities, named processes, and status concepts with project-specific meaning. Seeded with core domain vocabulary, then accretes as ce-compound and ce-compound-refresh process learnings; direct edits are fine. Glossary only, not a spec or catch-all.

## Gateway Management

### Nanobot Gateway
The independently managed nanobot service that exposes health and WebUI/chat interfaces and can outlive the native manager.

The manager launches it through nanobot's background lifecycle, observes the real gateway instance recorded by the upstream runtime, and requests graceful stop through the gateway control plane rather than treating a launcher PID as the service.

### Health Check
A readiness observation that reports whether the Nanobot Gateway is serving its health endpoint; it is distinct from process identity and from the WebUI interface.

Health is sampled asynchronously and may tolerate transient failures, while process-handle signaling provides immediate death detection.

### Gateway State Record
The upstream-owned rendezvous record that identifies one background Nanobot Gateway instance and preserves the information required to rediscover, validate, observe, and stop it across manager launches.

### Gateway Process Identity
The process-creation identity paired with a Gateway State Record's PID so stale state cannot target a different process after Windows reuses that PID.

### Gateway Lifecycle State
The manager's coordinated status for resolving, idle, startup, running, stopping, and terminal unavailability, used to assign ownership of asynchronous transitions and distinguish an invited stop from an unsolicited failure.

Terminal unavailability can be reached by a process-exit event or sustained failed readiness; it does not by itself prove that the process has exited.

### WebUI Port
The port the nanobot gateway serves its web interface on (default 8765). Independent of the health check port — not `health_check_port + 1`. The native chat client connects to this port for bootstrap and WebSocket upgrade.

### Bootstrap
An HTTP GET to `/webui/bootstrap` on the WebUI port that returns a JSON object with a WebSocket token and ws_path. The chat client uses this to negotiate the WebSocket upgrade. Optional `webui_auth` config (gateway `tokenIssueSecret`) is sent as `X-Nanobot-Auth` header. If bootstrap fails or the gateway doesn't require tokens, the client falls back to a direct WebSocket connect.

### Chat Session
A WebSocket connection to the gateway's chat endpoint, established after bootstrap. Streams JSON text frames: `ready` (chat_id assigned), `delta` (streaming text chunks), `message` (complete assistant text), `stream_end`, and `error`. The native client manages this with a connect thread (bootstrap + WS upgrade) and a receive thread (frame parsing), both joinable with generation-safe close.

### Message Bubble
A single entry in the chat UI's message array, rendered as a bubble via core-ui's `v-for` in a ScrollView. Each has a role (`user` or `assistant`), text content, and a unique id. Assistant bubbles are built incrementally — `delta` events append to the last assistant bubble's text, `message` replaces it with the complete text.
