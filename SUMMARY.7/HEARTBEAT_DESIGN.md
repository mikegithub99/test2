# Heartbeat Failure and Server Artifact Cleanup Design

## Purpose and required behavior

A server instance is considered failed when its heartbeat key expires. Failure triggers cleanup scoped strictly to that one `(serverType, serverId)` instance. It must not remove artifacts belonging to other server IDs of the same server type.

### Strict cleanup scope

The cleanup target is one server ID only. The server-type keys are shared aggregates and must remain in place:

- Delete only this server ID's job hashes, server job index, server summary, heartbeat, live-set membership, and matching registration entry.
- In the shared type job index, remove only the references for jobs owned by this server ID. Do not delete the type job index itself.
- In the shared type summary, remove only this server ID's membership field and decrement only the counters contributed by this server's deleted jobs. Do not delete the type summary, reset its counters, or remove its metadata.
- Do not remove the server type from global type discovery, and do not touch any other server ID's jobs, indexes, summary, heartbeat, live membership, or registration.

Cleanup must:

- Delete every job hash owned by the failed server.
- Remove each job reference from the server-level and server-type job indexes.
- Delete the server summary hash, including its counters and metadata.
- Delete the heartbeat key and remove the server from the live-server index.
- Remove the server ID from the server-type summary's membership fields.
- Remove the server ID from the registered-server directory once cleanup is safely committed.
- Decrement the server-type summary counters for every deleted job: the matching state counter (`pending`, `active`, or `suspended`) and `total`.
- Preserve the server-type summary hash and its metadata. Its membership fields and counters must represent the remaining servers and jobs of that type. Do not delete the type or its descriptive metadata merely because one instance failed.

This is destructive cleanup. A server that resumes after its heartbeat expires is treated as a new registration/incarnation; its old jobs and server summary are not restored.

## Existing key layout

The current implementation uses these relevant keys:

| Key | Purpose |
|---|---|
| `servers:{registry}:types` | Global set of known server types |
| `servers:{registry}:type:<serverType>` | Set of registered server IDs |
| `summary:type:{<serverType>}` | Type counters and metadata |
| `summary:server:{<serverType>}:<serverId>` | Per-server counters and metadata |
| `job:{<serverType>}:<serverId-length>:<serverId>:<jobId>` | Per-job hash |
| `jobs:type:{<serverType>}` | Type-wide set of composite job references |
| `jobs:server:{<serverType>}:<serverId>` | Per-server set of composite job references |
| `heartbeat:server:{<serverType>}:<serverId>` | Heartbeat string with TTL |

The job, index, summary, and heartbeat keys use the `{serverType}` hash tag. The registration directory uses `{registry}`. These are different Redis Cluster slots.

The type summary is a shared HASH containing type-level counters, type metadata, and (under this proposed design) one membership field per registered server. It does not contain job references. Server membership is represented within that same hash using fields named `server:<serverId>`; each field stores the current incarnation token for that server ID. On heartbeat failure, cleanup removes only the failed server's membership field with `HDEL`, while preserving the type summary, its counters, metadata, and all other servers' membership fields. This is a new design convention; the current implementation does not yet maintain these membership fields. Job references are stored only in `jobs:type:{<serverType>}` and `jobs:server:{<serverType>}:<serverId>`.

## Redis Cluster constraint

A single Lua script cannot atomically access both the type-partitioned lifecycle keys and the global `{registry}` keys in Redis Cluster. A cleanup script that tries to delete jobs and also `SREM` the current registered-server set will fail with a cross-slot error.

Therefore cleanup is a coordinated, idempotent protocol with two same-slot phases. Do not attempt to combine the global registry keys and per-type keys in one Lua script.

## API fencing and deletion races

Heartbeat expiry does not stop the server process from sending API requests. Cleanup must therefore fence the failed process before deleting its data. Lua atomicity prevents commands from interleaving during a script, but by itself does not prevent requests immediately before or after cleanup.

Add a per-type lifecycle-state HASH:

- `servers:state:{<serverType>}` — field `<serverId>` stores the current incarnation token and lifecycle state (`ACTIVE` or `DELETING`). The representation may be encoded as a single value or separate fields, but comparisons and state changes must be atomic.

Every lifecycle API script (`createPendingJob`, `transitionJob`, and `completeJob`) must receive the caller's incarnation token and, before changing any job/index/summary data, verify that the server is `ACTIVE` and the stored token matches. Otherwise it returns a dedicated stale-incarnation/server-not-active result without modifying data. Heartbeat refresh must perform the same check, so a deleting incarnation cannot renew itself.

Before Phase A cleanup, run a fencing script in the `{serverType}` slot. It verifies that the heartbeat is absent and that the current incarnation matches the expected token, then atomically changes that incarnation's state from `ACTIVE` to `DELETING`. If the heartbeat has been renewed, the token differs, or the state is already inconsistent, it makes no destructive changes. The fence and every API script use the same slot and are atomic, so a concurrent API call is ordered either before the fence (and its changes are included in subsequent cleanup) or after the fence (and is rejected). Do not begin destructive cleanup until fencing succeeds.

Phase A cleanup must require the matching `DELETING` state and incarnation token. It must not clean a replacement incarnation. After Phase A, Phase B conditionally removes the matching registry entry. A new process reusing the same server ID registers a new token and an `ACTIVE` state; delayed requests and cleanup from the old token must not affect it. Registration of a replacement must be coordinated with cleanup state so it cannot be overwritten by delayed old-incarnation operations.

## Recommended key additions

Add these per-type keys:

- `servers:live:{<serverType>}`
- `servers:state:{<serverType>}`

It shares the `{serverType}` hash tag with the heartbeat, jobs, and summaries. A successful heartbeat refresh should atomically:

1. Validate the live-set key type.
2. Set the heartbeat value with the requested positive TTL.
3. Add the server ID to `servers:live:{<serverType>}`.

Use a Lua script for those operations so the heartbeat and live-set membership cannot diverge because of an interrupted client sequence. Registration should also establish the server's current incarnation in the type summary's `server:<serverId>` field, storing the incarnation token. That summary update must be performed by a script operating only on `{serverType}` keys; it cannot be combined in the same Lua script as updates to the `{registry}` directory.

The existing `servers:{registry}:type:<serverType>` set remains the discovery directory during migration, but cleanup must remove its member in the separate registry phase.

## Cleanup protocol

A cleanup worker periodically obtains candidate IDs from `servers:live:{<serverType>}`. It invokes a cleanup Lua script for each candidate, passing the candidate server ID and its expected incarnation token.

### Phase A — per-type cleanup (one hash slot)

The script receives only keys with the `{serverType}` hash tag, including:

- `servers:live:{<serverType>}`
- `servers:state:{<serverType>}`
- The candidate's heartbeat key.
- The candidate's server job index.
- The type-wide job index.
- The candidate's server summary.
- The type summary.

The script must first check that the candidate's lifecycle state is `DELETING` with the expected token, that the heartbeat is still absent/expired, and that the incarnation still matches the `server:<serverId>` field in the type summary. If the heartbeat exists, the state is not `DELETING`, or any incarnation differs, it must make no changes and report that the candidate is live, not fenced, or has been replaced. The preceding fencing operation must have succeeded before this script is invoked.

If the heartbeat is absent and the incarnation matches, the script must preflight all data before writing:

1. Validate expected Redis key types (SET for indexes/live set, HASH for summaries and job hashes, and the expected heartbeat type).
2. Read the server's job references from its server job index.
3. Resolve each job key from the server type and composite reference, and verify that the job hash belongs to the exact server type and server ID.
4. Validate each job status is one of `PENDING`, `ACTIVE`, or `SUSPENDED`.
5. Count jobs by status. These counts, rather than the server summary, are authoritative for the type-counter decrements.
6. Verify the type summary has valid integer counters with sufficient values for each decrement. Validate the relevant type job index membership for every reference.
7. Verify the type summary's `server:<serverId>` membership field contains the expected incarnation token.

Only after all checks pass, perform the cleanup atomically:

- For each job owned by this server ID, remove only that job's reference from the shared `jobs:type:{<serverType>}` set and delete only that job hash. Leave references belonging to other server IDs untouched.
- Delete `jobs:server:{<serverType>}:<serverId>`.
- Decrement `pending`, `active`, and `suspended` in the shared `summary:type:{<serverType>}` by the counts for this server's deleted jobs. Decrement `total` by only this server's deleted-job count. A zero decrement may be skipped. Preserve all remaining counts and metadata.
- Remove only the `server:<serverId>` field from `summary:type:{<serverType>}` (using `HDEL`). Do not delete or reset the type summary hash, any other `server:<otherServerId>` field, or any type metadata/counter field.
- Delete `summary:server:{<serverType>}:<serverId>`.
- Delete the heartbeat key.
- Remove the server ID from `servers:live:{<serverType>}`.

The script must not decrement counters based on the server summary: that hash is being deleted and could be stale or inconsistent. It must not treat the type summary as a job-reference index, change type-summary metadata, or delete the type summary. On inconsistent data, return an error/result without partial cleanup; validate everything that can fail before the first write because Redis Lua scripts do not provide general rollback after runtime errors.

An empty server job index means there are no jobs to delete; the type counters are unchanged, and the server summary and the matching type-summary membership field can still be deleted.

### Phase B — global registry cleanup (registry hash slot)

After Phase A reports that it committed cleanup, remove only this server ID's matching registry entry from `servers:{registry}:type:<serverType>` in a separate registry-slot operation. Keep `servers:{registry}:types` and every other server ID's registry entry. Keep the type summary and its metadata: other servers may still use the type.

This phase must be idempotent and safe to retry after a worker crash. The cleanup coordinator must also guard against a new incarnation registering between Phase A and Phase B. Use a unique incarnation/generation token for each server process lifetime, stored with the registry entry, and remove the registry entry only if it still contains the expected token. A restarted process registers a new token, so delayed cleanup for the old process cannot unregister the new process.

To support this compare-and-remove operation, replace or supplement the per-type registry SET with a per-type HASH mapping `serverId -> incarnationToken`. The global type-discovery SET can remain unchanged. Registry registration and conditional removal scripts operate only on `{registry}` keys and remain cluster-safe.

## Incarnation and restart rules

A server ID alone is not sufficient to distinguish a failed process from a restarted process reusing the same ID. Each process lifetime must have a unique incarnation token (for example, a UUID generated at startup).

- Registration publishes `serverId -> incarnationToken` in the registry and stores the token in the type summary's `server:<serverId>` membership field.
- Heartbeat refresh publishes the same token alongside the heartbeat and live-set membership.
- Cleanup is conditional on the expected token.
- A new process using the same server ID publishes a different token.
- A delayed cleanup for an old token must never delete jobs or summaries created by the new incarnation.

The heartbeat value should carry or be paired with the incarnation token. The cleanup script compares the expected token to the current token before deleting anything. If tokens do not match, cleanup is stale and must stop.

## Operational behavior and failure handling

- Choose a heartbeat TTL comfortably longer than the heartbeat interval, allowing for normal scheduling pauses and transient Redis latency.
- A temporary heartbeat delay shorter than the TTL does not trigger cleanup.
- Once the TTL expires, cleanup is permanent for that incarnation. A returning process must register a fresh incarnation and start with no prior jobs or server summary.
- Retry transient Redis failures. Both phases are idempotent; a retry must not decrement type counters twice. Phase A achieves this by deleting the server's job index and artifacts in the same atomic script. If the artifacts are already absent, it must report already-cleaned and make no further counter changes.
- If Phase A finds wrong-type keys, missing indexed job hashes, ownership mismatches, invalid statuses, or insufficient/non-integer type counters, stop and report inconsistent data. Do not guess counter adjustments or delete only part of the server's data. Alert/repair the inconsistency explicitly.
- Monitor cleanup failures and the age/count of expired candidates so stale entries do not silently accumulate.
- Restrict cleanup permissions to the trusted cleanup worker; it can permanently delete this server ID's job records and server summary metadata, and decrement shared type counters. It must not delete type-level metadata.

## API and implementation impact

A future implementation should add:

- A heartbeat refresh Lua script that validates the active incarnation, sets the TTL-bearing heartbeat, and adds the ID to the per-type live set atomically.
- A fencing Lua script that changes the matching incarnation from `ACTIVE` to `DELETING` only when its heartbeat is absent.
- Incarnation-token validation in every job lifecycle Lua script, rejecting requests from deleting, inactive, or superseded processes.
- A cleanup API/worker operation that accepts `serverType`, `serverId`, and the expected incarnation token, and reports cleaned, still-live/replaced, already-cleaned, or inconsistent outcomes.
- A registry representation and Lua operations that register an incarnation and conditionally remove only the matching incarnation.
- A type-summary membership convention: `server:<serverId>` fields map each registered server ID to its incarnation token. Registration adds/updates only that server's field; cleanup removes only that server's field with `HDEL`. The type summary's counters and metadata remain independent fields in the same hash.
- A cleanup Lua script embedded in `RedisJobScripts.inc`, with all Phase A keys passed explicitly through `KEYS`.
- Tests covering multiple jobs in each state, zero-job servers, wrong-type keys, missing jobs, counter underflow, retry/idempotency, API calls racing with fencing, heartbeat renewal racing with fencing, rejected calls after fencing, and server restart with the same server ID.
- Redis Cluster tests proving each Lua script touches only one hash slot.

The existing public API does not currently implement this cleanup behavior. This document specifies the target design; it does not change runtime behavior by itself.
