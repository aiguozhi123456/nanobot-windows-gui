# Concepts

Shared domain vocabulary for this project — entities, named processes, and status concepts with project-specific meaning. Seeded with core domain vocabulary, then accretes as ce-compound and ce-compound-refresh process learnings; direct edits are fine. Glossary only, not a spec or catch-all.

## Gateway Management

### Nanobot Gateway
The Python process (`nanobot gateway` or `python -m nanobot gateway`) that the manager starts, stops, and monitors. Exposes a health endpoint and a WebUI with a WebSocket chat interface. The manager's primary purpose is to keep this process running and provide a native UI for it.

### Health Check
A WinHTTP GET to `http://localhost:{health_check_port}/health` (default port 18790) that returns HTTP 200 when the gateway is active. The manager polls this on a timer to detect start/stop transitions and auto-connect chat. Distinct from the WebUI port — the two are independent.

### WebUI Port
The port the nanobot gateway serves its web interface on (default 8765). Independent of the health check port — not `health_check_port + 1`. The native chat client connects to this port for bootstrap and WebSocket upgrade.

### Bootstrap
An HTTP GET to `/webui/bootstrap` on the WebUI port that returns a JSON object with a WebSocket token and ws_path. The chat client uses this to negotiate the WebSocket upgrade. Optional `webui_auth` config (gateway `tokenIssueSecret`) is sent as `X-Nanobot-Auth` header. If bootstrap fails or the gateway doesn't require tokens, the client falls back to a direct WebSocket connect.

### Chat Session
A WebSocket connection to the gateway's chat endpoint, established after bootstrap. Streams JSON text frames: `ready` (chat_id assigned), `delta` (streaming text chunks), `message` (complete assistant text), `stream_end`, and `error`. The native client manages this with a connect thread (bootstrap + WS upgrade) and a receive thread (frame parsing), both joinable with generation-safe close.

### Message Bubble
A single entry in the chat UI's message array, rendered as a bubble via core-ui's `v-for` in a ScrollView. Each has a role (`user` or `assistant`), text content, and a unique id. Assistant bubbles are built incrementally — `delta` events append to the last assistant bubble's text, `message` replaces it with the complete text.
