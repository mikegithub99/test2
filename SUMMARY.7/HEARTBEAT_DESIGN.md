# Heartbeat Failure and Server Artifact Cleanup Design

## Purpose

When a server's heartbeat expires, a background worker cleans only that exact `(serverType, serverId)`. It must preserve all other server IDs and shared type-level data.

## Process identity and API

The application guarantees that `serverId` is unique per process by appending a UUID, and that an ID is never reused. This existing unique `serverId` is the process identity; no separate incarnation token is needed.

Both `serverType` and `serverId` continue to come from `ConfigProvider` (or the existing default configuration lookup). Keep the existing API unchanged:

```cpp
registerServer(serverTypeMetadata, serverMetadata);
```

Do not add a UUID/incarnation argument, change lifecycle API signatures, or duplicate `serverId` in metadata or type-summary metadata. Heartbeat value is the configured `serverId`.

`RedisJobStatus` retains its existing configured identity and Redis client; it does not generate a second UUID or own the cleanup thread. A separate `HeartbeatManager` owns the background thread and its Redis access.

## Existing key layout

| Key | Purpose |
|---|---|
| `servers:{registry}:types` | Global set of known server types |
| `servers:{registry}:type:<serverType>` | Set of registered server IDs |
| `summary:type:{<serverType>}` | Shared type counters and metadata |
| `summary:server:{<serverType>}:<serverId>` | Per-server counters and metadata |
| `job:{<serverType>}:<serverId-length>:<serverId>:<jobId>` | Per-job hash |
| `jobs:type:{<serverType>}` | Type-wide set of composite job references |
| `jobs:server:{<serverType>}:<serverId>` | Per-server set of composite job references |
| `heartbeat:server:{<serverType>}:<serverId>` | Heartbeat string with TTL |
| `servers:live:{<serverType>}` | Set of IDs with a maintained heartbeat |
| `servers:state:{<serverType>}` | Hash mapping server ID to `ACTIVE` or `DELETING` |

Job, index, summary, heartbeat, live-set, and state keys use the `{serverType}` hash tag. Registration keys use `{registry}`; these are different Redis Cluster slots. The type summary remains only shared counters and type metadata. It is not a server membership registry, and no duplicate server ID is added to its metadata.

## Redis Cluster constraint

One Lua script cannot atomically access both the per-type keys and global `{registry}` keys in Redis Cluster. Cleanup is therefore two idempotent phases: Phase A operates only in the `{serverType}` slot; Phase B operates only in the `{registry}` slot. Never combine the two slots in one script.

## Heartbeat and lifecycle fencing

Heartbeat refresh is a Lua script in the `{serverType}` slot. It verifies that `servers:state:{<serverType>}` contains `ACTIVE` for this ID, sets the heartbeat value to the exact `serverId` with a positive TTL, and adds the ID to `servers:live:{<serverType>}` atomically.

Every job lifecycle script (`createPendingJob`, `transitionJob`, and `completeJob`) checks that the server ID is `ACTIVE` before changing job, index, or summary data. If not active, it returns a dedicated server-not-active result without modifying data.

Before cleanup, the worker invokes a fencing Lua script in the same `{serverType}` slot. It verifies that the heartbeat is absent and state is `ACTIVE`, then atomically changes state to `DELETING`. If the heartbeat exists or state is inconsistent, it makes no destructive change. Heartbeat refresh and lifecycle scripts use the same slot and state check: an operation ordered before fencing completes before the state changes; an operation ordered after fencing is rejected. A heartbeat refresh before fencing renews the TTL, so fencing then fails.

## Cleanup protocol

The manager periodically discovers types from `servers:{registry}:types`, then candidate IDs from `servers:live:{<serverType>}`. Candidate discovery is advisory; the fencing script decides whether the heartbeat is truly expired.

### Phase A — per-type cleanup

After fencing succeeds, a cleanup Lua script operates only on keys tagged `{serverType}`. It requires state `DELETING` and an absent heartbeat for the exact candidate `serverId`.

Before its first write, the script preflights all data that can cause failure:

1. Validate expected Redis key types (SET for indexes/live set; HASH for summaries, state, and job hashes).
2. Read the candidate's server job references.
3. Resolve each reference to its job hash and verify exact `serverType` and `serverId` ownership.
4. Validate each status is `PENDING`, `ACTIVE`, or `SUSPENDED`.
5. Count jobs by status from job hashes; do not trust the server summary for decrements.
6. Verify every reference is present in the type-wide job index.
7. Verify type-summary counters are valid integers and sufficient for the required decrements.

Only after all checks pass, atomically:

- Remove only this server's references from `jobs:type:{<serverType>}` and delete only its job hashes.
- Delete `jobs:server:{<serverType>}:<serverId>`.
- Decrement type-summary `pending`, `active`, `suspended`, and `total` by counts derived from this server's jobs. Preserve type metadata and all remaining counters.
- Delete `summary:server:{<serverType>}:<serverId>`.
- Delete this server's heartbeat and remove only its ID from `servers:live:{<serverType>}`.
- Remove this ID's field from `servers:state:{<serverType>}` after cleanup commits within the script.

Do not delete the type-wide job index, type summary, global type-discovery set, or any other server's keys/references. With zero jobs, counters are unchanged but per-server artifacts and state can still be removed.

Redis Lua scripts serialize commands but do not roll back earlier writes after a runtime error. Preflight all possible failure conditions before writing. On inconsistent data, return an error without partial cleanup. Phase A must be idempotent: once the server job index and artifacts are removed, a retry must not decrement counters again.

### Phase B — registry cleanup

Only after Phase A is confirmed committed, remove this exact `serverId` from `servers:{registry}:type:<serverType>` in a separate registry-slot operation. Keep `servers:{registry}:types` and every other ID. The operation is idempotent and safe to retry after a worker crash.

Because IDs are guaranteed unique and never reused, a delayed Phase B removal cannot unregister a replacement process: a replacement must register with a different ID. Do not introduce a registry token/hash solely for incarnation comparison.

## Restart and expiry behavior

Expiry is permanent for that process ID. A process that resumes after fencing has its heartbeat refresh and lifecycle operations rejected because its state is no longer `ACTIVE`. A replacement registers under a new unique `serverId` and starts with its own jobs and summary. Never reactivate or reuse an expired ID.

## Operational behavior

- Set heartbeat TTL comfortably longer than the refresh interval to allow for scheduling pauses and transient Redis latency.
- Retry transient Redis failures; both phases must be idempotent.
- If Phase A finds wrong-type keys, missing job hashes, ownership mismatches, invalid statuses, missing type-index references, or invalid/insufficient counters, stop and report inconsistent data. Do not guess or partially clean.
- Monitor cleanup failures and expired-candidate age/count.
- Restrict destructive cleanup permissions to the trusted `HeartbeatManager`.
- Never run Phase B unless Phase A is confirmed committed.

## Implementation impact and status

Implementation requires an atomic heartbeat refresh script, fencing script, `ACTIVE` checks in every lifecycle Lua script, a Phase A cleanup script in `RedisJobScripts.inc`, a registry-slot Phase B operation, and a background `HeartbeatManager` with retries and error reporting.

Tests should cover multiple jobs in each state, zero-job servers, wrong-type keys, missing jobs, ownership mismatch, counter underflow, retry/idempotency, lifecycle calls racing with fencing, heartbeat renewal racing with fencing, rejection after fencing, and Redis Cluster slot separation.

This is a target design, not an implementation. Existing runtime behavior remains unchanged until the manager, scripts, and tests are implemented.
