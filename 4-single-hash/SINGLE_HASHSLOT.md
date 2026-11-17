# Single-Hash-Slot Server-Type Architecture

## Status and scope

This document describes a proposed Redis Cluster architecture. It is a design target, not a claim that the current implementation already uses these keys or scripts. The current implementation remains documented under 3-colocated/ and uses a separate {registry} slot for global type discovery and server registration.

The proposal removes the global registry slot. A server type is the consistency boundary: every Redis key belonging to one type uses the same Redis Cluster hash tag, {serverType}. Mutations affecting that type's servers, jobs, indexes, heartbeats, lifecycle state, and counters execute in Lua in that slot.

The UI retains the same hierarchy:
1. One row per server type, with aggregate counters.
2. Drill down to all server IDs and per-server counters/metadata.
3. Drill down from a server to its jobs.
4. A type-wide jobs view across every server ID of that type.

This does not provide atomic updates across different server types or a globally consistent snapshot across all types.

## Design principles

- One hash tag per type: all orders keys use {orders}; all billing keys use {billing}.
- No global mutable registry: type existence is represented by a durable per-type manifest.
- Counters are maintained incrementally. Every job lifecycle mutation updates both the owning server counters and the type aggregate counters in the same script.
- Both a type-wide job index and a per-server job index are maintained.
- Fence before cleanup. Expired workers are fenced before potentially batched reconciliation/removal.
- Validate key types, ownership, memberships, state, arguments, and counter bounds before the first write.
- UI reads have an explicit consistency scope. A per-type Lua getter can return a coherent snapshot of that type; reads across types are best effort.

## Redis Cluster key structure

Canonical proposed layout for type orders:

| Redis type | Key | Contents / purpose |
|---|---|---|
| HASH | servers:{orders}:manifest | Durable type manifest: type name, created timestamp, lifecycle, schema version, optional type metadata. |
| HASH | servers:{orders}:summary | Aggregate counters across all servers of this type, plus type metadata. |
| HASH | servers:{orders}:state | Server ID to lifecycle state, such as ACTIVE, DELETING, RETIRED. |
| SET | servers:{orders}:ids | Server IDs known to this type. Retain IDs if needed for tombstone/audit purposes. |
| HASH | servers:{orders}:server:<serverId> | Per-server metadata and that server's counters. |
| STRING with TTL | servers:{orders}:heartbeat:<serverId> | Heartbeat/liveness key for one server. |
| SET | servers:{orders}:live | Server IDs that have successfully published at least one heartbeat. |
| ZSET | servers:{orders}:retired | Fenced/retired server IDs with Redis-time retirement timestamps. |
| SET | servers:{orders}:jobs | Composite references for all current jobs of this type, across every server ID. |
| SET | servers:{orders}:server:<serverId>:jobs | Composite references for current jobs owned by this server. |
| HASH | servers:{orders}:job:<jobRef> | Job status, original job ID, server type, owning server ID, and custom metadata. |

The same layout is repeated for each type, changing the hash-tag value and identifiers:

    servers:{orders}:manifest
    servers:{orders}:summary
    servers:{orders}:state
    servers:{orders}:ids
    servers:{orders}:server:server-123
    servers:{orders}:heartbeat:server-123
    servers:{orders}:live
    servers:{orders}:retired
    servers:{orders}:jobs
    servers:{orders}:server:server-123:jobs
    servers:{orders}:job:<jobRef>

    servers:{billing}:manifest
    servers:{billing}:summary
    ...

Redis hashes the text inside braces. All keys for one type therefore share one slot; different types may be distributed across slots/nodes.

### Job references

Do not build job keys using ambiguous concatenation. Use a deterministic unambiguous composite reference, such as a length-prefixed encoding:

    <serverId-length>:<serverId>:<applicationJobId>

Use the same reference in the type-wide job set and the server-specific job set. The job hash retains original jobId, serverType, and serverId as authoritative fields. Construct the reference consistently in C++ for lifecycle and read paths. Server IDs need to be unique within a type, not globally across all types.

## Manifest and server-type discovery

### Manifest lifecycle

The manifest is a durable marker that a type exists. Create it on the first successful registration for that type, in the same Lua execution that initializes the type summary and activates the first server.

Suggested fields:

| Field | Meaning |
|---|---|
| serverType | Canonical type name; must agree with the hash tag. |
| createdAt | Creation timestamp, preferably generated using Redis TIME. |
| lifecycle | ACTIVE, RETIRING, or RETIRED. |
| schemaVersion | Version of the per-type key/data layout. |
| meta:* | Optional type metadata, namespaced to avoid reserved fields. |

Keep the manifest after the last server exits. An empty type is still a known type. Retain a retired manifest as a tombstone if the UI must distinguish “known but retired” from “never existed.” Do not expire it as a side effect of server cleanup.

Registration should be idempotent. The per-type registration Lua script validates manifest, summary, server record, and state/index key types before writing. It creates the manifest if absent, initializes type metadata/counters if absent, adds the server ID, creates server metadata, and sets the server state to ACTIVE in one script. Retries must not reset existing counters or immutable creation metadata.

### Cluster-wide SCAN procedure

There is no global {registry} set. A discovery service enumerates Redis Cluster primary nodes and scans each primary directly for manifest keys using this pattern:

    servers:*:manifest

Conceptual algorithm:

    types = set()

    for each current cluster primary:
        cursor = 0
        repeat:
            cursor, keys = SCAN cursor MATCH "servers:*:manifest" COUNT <hint>
            for each key in keys:
                parse and validate exact form:
                    servers:{<serverType>}:manifest
                add <serverType> to types
        until cursor == 0

    for each discovered serverType:
        read servers:{serverType}:manifest
        include or filter it according to lifecycle state

Operational details:

- Redis Cluster SCAN is node-local. A single SCAN against an arbitrary cluster endpoint is not a cluster-wide enumeration. Obtain current cluster topology and scan every primary.
- Follow each node's cursor to zero. COUNT is a work hint, not a page-size guarantee.
- Deduplicate results. SCAN may return duplicates, and keys may change while scanning.
- Parse the key format strictly; do not accept arbitrary keys matching a loose glob.
- Validate each candidate by reading its manifest. It may have disappeared or changed during discovery.
- Refresh topology and retry nodes that fail or are replaced during a scan.
- Use an ACL identity permitted to run SCAN on every primary and read manifest keys.
- Run discovery periodically and cache the result in the UI/backend. A newly created type may not appear until a later scan.
- Keep retired manifests if retired types must remain discoverable.

### Discovery consistency

Cluster-wide SCAN is eventually consistent, not a point-in-time cluster snapshot. A type created during a scan may be missed until the next scan; a type retired or deleted during a scan may still be returned. Redis SCAN does not promise a stable snapshot while keys are changing.

This is normally acceptable for a UI catalogue. The UI should tolerate a type appearing on a later refresh and treat a manifest read that returns missing as a stale candidate. If an exact instantaneous global type list is required, SCAN alone cannot provide it. A globally coordinated catalogue or another coordination mechanism would be required, reintroducing a global consistency boundary.

## Counters and atomic job lifecycle

Each server hash stores that server's counters, for example pending, active, suspended, and total. The type summary stores aggregate counters across all servers of that type using the same fields.

For every counter-changing operation, one Lua script in the type's slot updates the job record, both indexes, server counters, and type counters together.

### Create job

One script should:
1. Validate that the server is ACTIVE.
2. Validate that the job does not already exist and the composite reference is absent from both indexes.
3. Validate key types and preflight counters for malformed values and signed 64-bit overflow.
4. Create the job hash with authoritative jobId, serverType, serverId, and PENDING status.
5. Add the reference to servers:{type}:jobs.
6. Add the reference to servers:{type}:server:<serverId>:jobs.
7. Increment pending and total on both the server hash and type summary.

### Transition job

One script validates ownership, old status, both index memberships, server state, and counter bounds; then updates job status and decrements/increments the old/new counters in both server and type summaries. A no-op transition does not change counters.

### Complete job

One script validates job ownership/status, both index memberships, and counter bounds; then decrements the matching status and total counters on both summaries, removes the reference from both indexes, and deletes or terminally retains the job record according to retention policy.

### Counter invariant

For each status counter and total:

    typeSummary.counter = sum(server.counter for every server ID of this type)

Every mutation changing a server's counters must change the type summary in the same Lua execution. This includes job creation, transitions, completion, reassignment, and expired-server cleanup. Counter repair/reconciliation must also execute within that type's slot.

Both indexes are intentional: the type-wide job index supports a type-wide jobs view; the per-server job index supports efficient server drill-down. Every job lifecycle script must keep both indexes consistent with the job hash.

## Heartbeat expiry, fencing, and cleanup

Heartbeat TTL expiry alone does not change server lifecycle state. A cleanup scanner detects the missing heartbeat and invokes a fencing script. The per-type slot makes server state, heartbeat, job indexes, job records, and counters available to the same Lua execution.

Recommended protocol: two logical phases, with the second phase safely batchable.

### Phase 1 — detect and fence

A Lua script in the type slot:
- Checks that the heartbeat is absent.
- Confirms the server is still ACTIVE.
- Applies the first-heartbeat startup grace period if the server never published a heartbeat.
- Uses a generation/fencing token where applicable so an old process cannot resume as the same logical incarnation.
- Changes server state to DELETING/fenced and records retirement time using Redis TIME.
- Prevents subsequent heartbeat refresh, job creation, transition, and completion from the fenced server identity/generation.

Heartbeat refresh and fencing inspect/update the same state and generation keys in the same slot. Their scripts serialize: whichever runs first determines whether the heartbeat is accepted or the fence wins.

### Phase 2 — reconcile and finalize

For a small bounded number of jobs, one script may reconcile all jobs and finalize cleanup. For potentially large job sets, process bounded batches:

- Read a bounded set of references from the server job index.
- Validate each job's type, owner, status, and membership in both indexes.
- Apply an explicit recovery policy for each job (for example, return it to a retryable state, mark it failed, or remove it). Do not silently assume every in-flight job should be deleted.
- Update type and server counters consistently for each processed job.
- Remove each processed reference from server and type indexes as appropriate.
- Make each batch idempotent so retries cannot decrement counters twice.
- When no server-owned jobs remain, remove active/live/heartbeat/server metadata as appropriate, update server-ID membership according to retention policy, and retain the state/retired tombstone for the required retention period.

A batch is atomic within the type slot, but the entire multi-batch cleanup is not one indivisible operation. Fencing prevents new work from being added while batches run. The UI may observe intermediate cleanup progress; expose DELETING so it is not mistaken for a healthy active server.

A tombstone-collection task may later remove expired tombstones and retired-index entries. Do not delete the type manifest merely because a server was cleaned up.

## UI getters and best-effort snapshots

The UI retains its existing hierarchy:

- Type row: discover/cached type names from manifests; fetch that type's summary counters and lifecycle metadata.
- Type drill-down: enumerate server IDs from the type's server-ID index/state hash and fetch each server's counters/metadata.
- Server drill-down: enumerate jobs from that server's job set and fetch job details.
- Type-wide jobs view: enumerate the type-wide job set, containing jobs across every server ID of that type.

### Preferred getter strategy

Prefer read-only Lua snapshot scripts per type when a screen needs mutually consistent fields from several keys. Explicitly pass every accessed key in KEYS; all keys must share the type hash tag. A script can read the type summary, state, server records, and/or selected job records without another command interleaving during that script.

Suggested getters:

- getTypeSnapshot(type): manifest + type summary + server state/index, with a bounded/paginated server list if needed.
- getServerSnapshot(type, serverId): server metadata/counters + state + heartbeat presence + a page of that server's job references/details.
- getTypeJobsSnapshot(type, cursor, limit): a page from the type-wide job index with corresponding job records.
- getServerJobsSnapshot(type, serverId, cursor, limit): a page from the server-specific job index with corresponding job records.

Do not return an unbounded number of jobs or servers in one Lua call. Use bounded pages and keep scripts short to avoid blocking Redis. For large indexes, use deterministic pagination and document that concurrent changes can shift page boundaries; cursor-based iteration or stable snapshot tokens may be needed for gap-free pagination under heavy mutation.

### Consistency meanings

1. **One per-type Lua snapshot:** coherent with respect to Redis command interleaving at the instant the script executes, for the keys it reads. This is the preferred UI getter.
2. **Several ordinary reads for one type:** best effort. Jobs or servers can change between commands, so counters and lists may represent slightly different instants.
3. **Several pages of one type:** best effort over time unless a snapshot/version protocol is added. Concurrent inserts/removals can affect later pages.
4. **Multiple server types:** no global atomic snapshot. Each type snapshot is internally coherent at its own execution time, but different rows represent different instants.
5. **Manifest discovery plus counter reads:** best effort. A type can be created, retired, or updated between discovery and summary read. Re-read/refresh on missing keys or lifecycle changes.

The UI should treat the type list and cross-type dashboard as a periodically refreshed operational view, not a globally transactional report. Each type row can have a coherent per-type snapshot, but the whole table is not guaranteed to represent one exact cluster-wide instant.

If exact multi-type consistency becomes a requirement, it needs an explicit coordination/snapshot design. Per-type hash tags do not provide cross-type atomicity.

### UI handling recommendations

- Cache discovered type names and periodically rescan manifests.
- Refresh each type summary independently; concurrent application-level reads are fine, but their timestamps differ.
- Include a snapshot timestamp/version in getter results so the UI can show freshness.
- Treat a missing manifest/summary after discovery as a stale candidate and refresh discovery.
- Display RETIRING/RETIRED types distinctly or filter them according to UI needs.
- Display DELETING servers as fenced/cleaning, not active.
- Use bounded pagination for server and job drill-down.
- Do not reconstruct aggregate counters by summing a paginated UI result; the maintained type summary is authoritative.

## Redis Cluster and operational considerations

- Every key accessed by one Lua script must be explicitly supplied in KEYS and map to the same slot.
- Do not access undeclared/implicitly constructed Redis keys from Lua.
- Different types can land on different slots, but one very large/hot type can become a hot slot. This deliberately trades per-type atomicity for the inability to split one type across slots.
- A Redis Cluster primary can own many slots. Discovery scans every current primary, not one node per type.
- Resharding/failover/topology changes can occur during discovery; discovery should retry and remain periodic.
- A per-type script can atomically update that type's counters, but cannot atomically update another type's keys.
- Redis scripts do not roll back earlier writes after an unexpected runtime error. Validate before mutation and test failure cases.
- Keep manifests and tombstones long enough for discovery, stale-worker rejection, and diagnosis.

## Migration from the current {registry} design

This document does not prescribe an in-place cutover sequence, but migration must account for the current layout:

- Current global keys include servers:{registry}:types and servers:{registry}:type:<serverType>.
- Current per-type keys already use {serverType} for summaries, jobs, indexes, heartbeat, state, and retired data.
- Current registration first updates the registry slot and then initializes/activates in the type slot. That cross-slot gap is the consistency issue this proposal removes.

A migration should create/validate per-type manifests and per-type server-ID indexes, verify counters and job indexes, then switch readers/discovery and writers in a coordinated rollout. Do not stop writing the registry while old readers still depend on it. Define rollback and mixed-version behavior before deployment. Existing keys should only be renamed/rebuilt with an explicit migration plan. This design document makes no data changes.

## Testing checklist

- Register the first server for a type; verify manifest, summary, server metadata, state, and server-ID membership are created in one per-type script.
- Retry registration; verify metadata/counters are not reset and duplicate identity behavior is deterministic.
- Register additional servers of the same type and verify the manifest remains stable.
- Create jobs on multiple servers of one type; verify type-wide and per-server indexes and both counter levels.
- Transition and complete jobs; verify the counter invariant after every operation.
- Exercise malformed, negative, zero, and signed-64-bit-boundary counter values.
- Verify every key passed to each Lua script shares the expected hash slot.
- Fence a server racing heartbeat refresh and job lifecycle operations.
- Retry cleanup batches and verify no double-decrement, lost job, or dangling index reference.
- Verify cleanup policy for each job state and type counters after cleanup.
- Discover manifests on every primary; test duplicates, missing keys, concurrent type creation/retirement, failover, and resharding.
- Verify UI snapshots are coherent per type and cross-type rows are not represented as a globally consistent snapshot.
- Test bounded pagination while jobs are concurrently added and removed.

## Summary

The server type becomes the atomic consistency boundary. The type owns its manifest, summary counters, server IDs and states, server metadata/counters, heartbeats, type-wide job index, per-server job indexes, and job records. Every mutation for that type executes in one same-slot Lua script, or a bounded sequence of idempotent same-slot scripts for large cleanup.

Type discovery periodically scans every Redis Cluster primary for durable servers:*:manifest keys and validates each manifest. Discovery is eventually consistent. UI getters should use per-type read-only Lua snapshots where a coherent view is needed, while the multi-type dashboard and paginated listings remain best-effort views across time.

The design removes the separate {registry} mutation dependency without removing the UI's type → server → jobs drill-down or type-wide job listing.
