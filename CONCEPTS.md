# Concepts

Shared domain vocabulary for this project — entities, named processes, and status concepts with project-specific meaning. Seeded with core domain vocabulary, then accretes as ce-compound and ce-compound-refresh process learnings; direct edits are fine. Glossary only, not a spec or catch-all.

## Gateway Management

### Nanobot Gateway
The independently managed nanobot service that exposes health and WebUI/chat interfaces and can outlive the native manager.

The manager launches it through nanobot's background lifecycle, observes the real gateway instance recorded by the upstream runtime, and requests graceful stop through the gateway control plane rather than treating a launcher PID as the service.

### Health Check
A readiness observation that reports whether the Nanobot Gateway is serving its health endpoint; it is distinct from process identity and from the WebUI interface.

Health is sampled asynchronously and may tolerate transient failures, while process-handle signaling provides immediate death detection.

### Gateway Health Port
The Nanobot Gateway endpoint used for readiness checks and selected when the manager starts a new gateway; it is independent of the WebUI Port.

For a running gateway, the manager may observe a different effective value only through a Gateway State Record whose Gateway Process Identity has been verified; that observation does not change the next-launch preference.

### Gateway State Record
The upstream-owned rendezvous record that identifies one background Nanobot Gateway instance and preserves the information required to rediscover, validate, observe, and stop it across manager launches.

### Gateway Process Identity
The process-creation identity paired with a Gateway State Record's PID so stale state cannot target a different process after Windows reuses that PID.

### Gateway Lifecycle State
The manager's coordinated status for resolving, idle, startup, running, stopping, and terminal unavailability, used to assign ownership of asynchronous transitions and distinguish an invited stop from an unsolicited failure.

Terminal unavailability can be reached by a process-exit event or sustained failed readiness; it does not by itself prove that the process has exited.

### WebUI Port
The Nanobot Gateway endpoint used for its web interface, Bootstrap, and Chat Session transport; it is independent of the Gateway Health Port.

The manager may override only its connection target; it does not change the gateway's configuration or listening port.

### Bootstrap
A negotiation step on the WebUI Port that obtains the connection token and route required to establish a Chat Session.

When negotiation is unavailable or does not require a token, the client may attempt a direct chat connection.

### Bootstrap Credential
A user-scoped secret that authorizes Bootstrap, distinct from the short-lived connection token returned by that negotiation.

The manager keeps it outside ordinary configuration and reactive UI state. Migration removes a legacy plaintext source only after the credential can be read back, and conflicting sources require explicit resolution rather than silent overwrite.

### Chat Session
A WebSocket attachment to the gateway's chat interface, established after Bootstrap, that carries one or more Chat Turns and may outlive any individual turn.

Connection lifecycle and Chat Turn lifecycle are separate: a connected session may be idle, processing a turn, or carrying a delayed event from an earlier turn.

### Chat Turn
The client-visible lifecycle of one accepted chat input, from transport acceptance through the evidence that permits the manager to accept another input.

Normal response turns require response activity before a terminal event can complete them, while inline commands may define a distinct acknowledgement. A terminal event without the required current-turn evidence is treated as stale rather than as proof of completion.

### Chat History
The manager-owned local sequence of user and assistant messages restored across manager launches, distinct from the gateway's server-side conversation context.

An unreadable existing history is never treated as permission to overwrite it. A destructive command keeps a process-local compensation copy until acknowledgement, but that copy neither reverses server-side effects nor survives a process crash.

### Message Bubble
A display item representing one user or assistant message in Chat History.

An assistant bubble may be built from partial response events and then normalized by a complete response; transient rendering identity is not part of durable Chat History.
