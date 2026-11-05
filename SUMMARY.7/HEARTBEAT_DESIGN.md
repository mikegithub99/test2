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

`RedisJobStatus` retains its existing configured identity and Redis client; it does not generate a second UUID or own the cleanup thread. Each server process owns its heartbeat publisher. A single central `RedisHeartbeatManager` process owns the cleanup loop. Each distributed process runs its own manager; do not run a second heartbeat runner in the same process.

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

## Heartbeat timing and ownership

Each application process publishes its own heartbeat through `RedisJobStatus`. Exactly one separately deployed `RedisHeartbeatManager` process scans all registered server types and IDs for expiry. Worker/server processes do not start cleanup scanners. The manager both publishes its own heartbeat and scans registered IDs for expired peers. It starts after `registerServer()` has completed.

The default timing is a 10-second heartbeat refresh interval, a 30-second Redis TTL, and a 5-second cleanup scan interval. Each server process refreshes immediately after successful registration, then every 10 seconds under normal scheduling. A process is considered expired only after Redis has removed its heartbeat key; cleanup then runs on a later scan. Expected detection latency is therefore about 30 seconds plus up to one scan interval and Redis/network/scheduling delay. The 3:1 TTL-to-refresh ratio tolerates roughly 20 seconds of missed refresh time, but cannot protect against an outage or pause longer than the TTL.

All Lua used by job lifecycle, heartbeat refresh, fencing, and cleanup is centralized in `RedisJobScripts.inc`. Both `RedisJobStatus.cc` and `RedisHeartbeatManager.cc` include this shared C++ raw-string script file. Do not embed separate Lua script bodies in a `.cc` file.

## Heartbeat and lifecycle fencing

Heartbeat refresh is a Lua script in the `{serverType}` slot. It verifies that `servers:state:{<serverType>}` contains `ACTIVE` for this ID, sets the heartbeat value to the exact `serverId` with a positive TTL, and adds the ID to `servers:live:{<serverType>}` atomically.

Every job lifecycle script (`createPendingJob`, `transitionJob`, and `completeJob`) checks that the server ID is `ACTIVE` before changing job, index, or summary data. If not active, it returns a dedicated server-not-active result without modifying data.

Before cleanup, the worker invokes a fencing Lua script in the same `{serverType}` slot. It verifies that the heartbeat is absent and state is `ACTIVE`, then atomically changes state to `DELETING`. If the heartbeat exists or state is inconsistent, it makes no destructive change. Heartbeat refresh and lifecycle scripts use the same slot and state check: an operation ordered before fencing completes before the state changes; an operation ordered after fencing is rejected. A heartbeat refresh before fencing renews the TTL, so fencing then fails.

## Cleanup protocol

The manager periodically discovers types from `servers:{registry}:types` and IDs from each type's registry set. The fencing script checks the heartbeat and requires live-set membership before fencing an `ACTIVE` ID, so a newly registered process is not cleaned before its first heartbeat. A `DELETING` ID is revisited even after Phase A removes its live-set membership, allowing Phase B to be retried after a crash.

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
- Retain this ID's `DELETING` field in `servers:state:{<serverType>}` as a terminal marker. Do not reactivate or reuse the ID.

Do not delete the type-wide job index, type summary, global type-discovery set, or any other server's keys/references. With zero jobs, counters are unchanged but per-server artifacts and the server summary is removed and the terminal `DELETING` marker remains.

Redis Lua scripts serialize commands but do not roll back earlier writes after a runtime error. Preflight all possible failure conditions before writing. On inconsistent data, return an error without partial cleanup. Phase A must be idempotent: once the server job index and artifacts are removed, a retry must not decrement counters again. Retain the terminal `DELETING` state field so the same process ID cannot be reactivated. If a worker crashes after Phase A but before Phase B, a later pass recognizes `DELETING` with the server job index and server summary absent and safely retries Phase B.

### Phase B — registry cleanup

Only after Phase A is confirmed committed, remove this exact `serverId` from `servers:{registry}:type:<serverType>` in a separate registry-slot operation. Retain the terminal `DELETING` state field in the per-type state hash as a no-reuse tombstone. Keep `servers:{registry}:types` and every other ID. The operation is idempotent and safe to retry after a worker crash.

Because IDs are guaranteed unique and never reused, a delayed Phase B removal cannot unregister a replacement process: a replacement must register with a different ID. Do not introduce a registry token/hash solely for incarnation comparison.

## Restart and expiry behavior

Expiry is permanent for that process ID. A process that resumes after fencing has its heartbeat refresh and lifecycle operations rejected because its state is no longer `ACTIVE`. A replacement registers under a new unique `serverId` and starts with its own jobs and summary. Never reactivate or reuse an expired ID.

## Operational behavior

- The default policy is a 10-second refresh interval, 30-second TTL, and five-second cleanup scan. The 3:1 TTL-to-refresh ratio allows about 20 seconds of missed-refresh tolerance; tune it to the application's pause and Redis-latency profile.
- Retry transient Redis failures; both phases must be idempotent.
- If Phase A finds wrong-type keys, missing job hashes, ownership mismatches, invalid statuses, missing type-index references, or invalid/insufficient counters, stop and report inconsistent data. Do not guess or partially clean.
- Monitor cleanup failures and expired-candidate age/count.
- Restrict destructive cleanup permissions to the trusted `RedisHeartbeatManager`.
- Never run Phase B unless Phase A is confirmed committed.

## Implementation impact and status

Implementation uses the shared `RedisJobScripts.inc` for heartbeat refresh, fencing, lifecycle checks, and Phase A cleanup. `RedisHeartbeatManager.cc` supplies the process-local 10-second heartbeat refresh (30-second TTL), five-second cleanup scan, and registry-slot Phase B operation.

Tests should cover multiple jobs in each state, zero-job servers, wrong-type keys, missing jobs, ownership mismatch, counter underflow, retry/idempotency, lifecycle calls racing with fencing, heartbeat renewal racing with fencing, rejection after fencing, and Redis Cluster slot separation.

The implementation is now present in `RedisHeartbeatManager.h/.cc`, `RedisJobStatus.h/.cc`, and `RedisJobScripts.inc`. All Lua scripts, including the manager's fencing and cleanup scripts, are centralized in `RedisJobScripts.inc`. It has been pushed to the repository, but has not yet been compiled or exercised against Redis; validate the C++ build, standalone Redis behavior, and Redis Cluster slot behavior before deployment.
