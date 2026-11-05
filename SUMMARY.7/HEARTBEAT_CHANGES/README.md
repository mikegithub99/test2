# Heartbeat cleanup changes

This folder documents the proposed heartbeat-expiry cleanup design for Redis job status.

## Agreed API and identity

- Keep the existing `RedisJobStatus::registerServer(serverTypeMetadata, serverMetadata)` signature unchanged.
- `serverType` and `serverId` come from `ConfigProvider` (or the existing default configuration lookup).
- The application guarantees that `serverId` is unique per process by appending a UUID, and IDs are not reused.
- Do not add a separate incarnation token or copy `serverId` into metadata/type-summary metadata.
- Keep the existing lifecycle API signatures unchanged.

## Proposed components

- `RedisJobStatus` retains its existing configured identity and Redis client. It does not generate a second UUID or own a cleanup thread.
- Server processes own heartbeat refresh. One separately deployed `RedisHeartbeatManager` owns the cleanup scan across all server types. It starts after successful `registerServer()`.
- Heartbeat refresh writes the configured `serverId` as the heartbeat value with a 30-second TTL every 10 seconds and updates the per-type live-server set atomically. Cleanup scans run every 5 seconds.
- The manager refreshes its own heartbeat, discovers candidates, fences expired IDs, performs per-type cleanup, then removes the registration in a separate registry-slot operation.

## Cleanup safety

Cleanup is limited to one exact `(serverType, serverId)`. It removes only that server's job hashes, server job index, server summary, heartbeat, live-set membership, and registration entry. It removes only that server's job references from the shared type index and decrements shared type counters based on the statuses read from that server's job hashes. It preserves the type summary, type metadata, global type discovery, and all other servers' data.

A per-type state hash records `ACTIVE` or `DELETING`. Heartbeat refresh and all job lifecycle Lua scripts require `ACTIVE`. A fencing Lua script atomically changes `ACTIVE` to `DELETING` only when the heartbeat is absent. This prevents later lifecycle calls or heartbeat refreshes from racing with cleanup.

Redis Cluster requires two phases because per-type keys use the `{serverType}` hash tag while registration keys use `{registry}`:

1. Phase A fences and cleans this server using only per-type keys, retaining a terminal `DELETING` tombstone.
2. Phase B removes this unique server ID from the registry using only registry-slot keys, after Phase A is confirmed committed.

Lua scripts must preflight key types, job ownership/status, type-index membership, and counter validity before writing. Lua execution is atomic against interleaving but does not roll back earlier writes after a runtime error. Both phases must be idempotent and safe to retry. If the manager crashes between phases, a later pass recognizes a terminal `DELETING` state with per-server artifacts already absent and retries Phase B.

## Files

- [`../HEARTBEAT_DESIGN.md`](../HEARTBEAT_DESIGN.md) — detailed protocol, key layout, fencing, cleanup phases, failure handling, and implementation impact.

## Status

This is a design proposal, not an implementation. The existing runtime behavior is unchanged until the manager, scripts, and tests are implemented.


## Source implementation

- `../HeartbeatManager.h/.cc` — background scanner, atomic expiry fence, per-type cleanup Lua execution, and separate registry removal.
- `../RedisJobStatus.cc` and `../RedisJobStatus.h` — initialize per-type `ACTIVE` state, include the state key in lifecycle scripts, reject lifecycle calls after fencing, and return a server-not-active result.
- `../RedisJobScripts.inc` — atomic heartbeat/live-set refresh and lifecycle state checks.

The terminal `DELETING` marker is intentionally retained: process IDs are never reused, and retaining the tombstone prevents a delayed registration from reactivating a cleaned identity. A cleanup pass can recognize that Phase A already committed and retry Phase B.

## Deployment topology

Run the worker/server application normally on every server process. Each process calls `registerServer()` and publishes only its own heartbeat every 10 seconds with a 30-second TTL. Separately build and run exactly one `RedisHeartbeatManagerMain.cc` service process. It scans all registered server types and IDs every 5 seconds and performs expiry fencing and cleanup. Do not start the cleanup manager inside worker processes.
