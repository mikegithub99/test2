# Redis Job Status Architecture (.7)



## 1. Executive summary

The `.7` job-status subsystem is a shared Redis-backed status model for a fleet of server replicas. Each replica reports lifecycle changes to Redis; the UI reads materialized summaries for the overview and retrieves individual job records only when a user drills down. The design avoids polling every replica and recomputing all counts on every dashboard refresh.

The implementation uses short, embedded Lua scripts for job creation, state transitions, and completion. Each lifecycle script updates the job hash, the relevant job-ID indexes, and both type-level and server-level counters in one atomic script execution. This is a good baseline for horizontal scaling because dashboard reads are bounded by the number of server types/replicas and summary dimensions, rather than by the total number of jobs.

Redis Cluster hash tags are deliberate:
- Job lifecycle keys use `{serverType}`, so the job hash, indexes, and summaries touched by one lifecycle script share a cluster slot.
- Server discovery keys use the shared literal `{registry}`, so registration can atomically update the global type set and a type-specific server set.
- The braces are literal key characters; Redis hashes the non-empty text inside the first valid `{...}` pair, not the braces themselves.

**Collision handling in .7:** job hashes use a length-prefixed composite reference derived from `serverId` and the original `jobId`. Both indexes store that reference, while the hash field `jobId` remains unchanged for UI display. This lets replicas of the same type use the same application-generated ID without colliding.

**Scale trade-off:** all keys tagged with the same `{serverType}` occupy one Redis Cluster slot. This enables multi-key Lua atomicity, but a very busy server type can concentrate load on one slot/primary. Measure actual per-node/slot traffic before changing the partitioning strategy.

## 2. Detailed architecture

### Components and data flow



1. **Server replicas** register their `serverType` and `serverId`, refresh a TTL-based heartbeat, and report job lifecycle changes.
2. **RedisJobStatus (C++17)** validates identifiers, constructs keys, calls the configured RedisClient, and maps Lua result codes to the public result enum. Status reporting can be disabled through `redisJobStatusEnabled`.
3. **Embedded Lua scripts** execute on Redis. They validate key types, job ownership, indexes, lifecycle state, and counters before writing. Redis does not interleave other commands during a script, so the related writes are atomic as a unit; this is not a general rollback transaction if a script encounters a runtime error after writing.
4. **Materialized summaries** hold counts for each server type and each server instance. The UI can read the type summary for an overview, then the server summaries and job details as needed.
5. **Discovery and health** use a registry of server types/instances and a heartbeat string with TTL. Registration membership is separate from heartbeat freshness; callers should treat the heartbeat as the liveness signal.

### Lua/C++ result-code contract

The lifecycle Lua scripts return application-defined integer codes. These values
are independent of Redis key names, hash tags, KEYS positions, and ARGV
positions. Keep the values stable because the C++ RedisJobStatus::Result enum
uses the same numeric values.

| Code | C++ result | Meaning |
|---:|---|---|
| 0 | `Success` | Operation succeeded |
| 1 | `NoOp` | Requested state is already current (transition only) |
| 2 | `JobNotFound` | Job does not exist (transition/completion) |
| 3 | `InvalidTransition` | Requested lifecycle transition is not allowed |
| 4 | `InconsistentData` | Redis key types, ownership, indexes, state, or counters are inconsistent |
| 5 | `JobAlreadyExists` | Job key already exists (creation only) |
| 6 | `Disabled` | Feature is disabled; returned by C++ without invoking Lua |

Lua scripts use named local constants for the codes they return. The server
registration script is different: it returns the Redis SADD result and uses
a Redis error reply for wrong key types, rather than the lifecycle result-code
contract.

Do not renumber existing values. Add new meanings only with an explicit
C++/Lua contract update and corresponding documentation.

### Lifecycle operations

- **Create:** create a `PENDING` hash, add its ID to type and server sets, and increment `pending` and `total` counters in both summaries.
- **Transition:** allowed transitions are `PENDING → ACTIVE`, `ACTIVE → SUSPENDED`, and `SUSPENDED → ACTIVE`. The script changes the status and transfers one count between the old and new state in both summaries. An unchanged state is a no-op.
- **Complete:** decrement the current-state and total counters, remove the ID from both indexes, and delete the job hash.
- **Register:** add the type to the global discovery set and the instance to its type-specific set in one script.
- **Heartbeat:** write a per-instance key with a positive TTL. A missing/expired heartbeat indicates that the instance has not refreshed recently; it does not by itself prove every job it owned failed.

### Redis key/data-structure table

The patterns below reflect the current C++ key builders in `RedisJobStatus.cc` and the Lua key contract. `serverType`, `serverId`, and `jobId` stand for actual values. Curly braces are part of the literal Redis key and explicitly delimit the hash tag.

| Redis type | Key pattern | Hash tag used by Redis Cluster | Purpose / sample content |
|---|---|---|---|
| SET | `servers:{registry}:types` | `registry` | Global server-type discovery. Example members: `query`, `order`. |
| SET | `servers:{registry}:type:serverType` | `registry` | Registered server IDs for one type. For `servers:{registry}:type:query`, example members: `query-01`, `query-02`. |
| HASH | `summary:type:{serverType}` | `serverType` | Type-level counters. Example key `summary:type:{query}`: `pending=3`, `active=5`, `suspended=1`, `total=9`. |
| HASH | `summary:server:{serverType}:serverId` | `serverType` | Per-replica counters. Example key `summary:server:{query}:query-01`: `pending=1`, `active=2`, `suspended=0`, `total=3`. |
| HASH | `job:{serverType}:<serverId-length>:<serverId>:<jobId>` | `serverType` | Job state, ownership, and optional metadata. Example key `job:{query}:8:query-01:job-100`: `jobId=job-100`, `status=PENDING`, `serverType=query`, `serverId=query-01`, plus any metadata fields. |
| SET | `jobs:type:{serverType}` | `serverType` | Composite references for current jobs of a server type. Example key `jobs:type:{query}` contains `8:query-01:job-100`, `8:query-02:job-100`. |
| SET | `jobs:server:{serverType}:serverId` | `serverType` | Composite references for current jobs owned by one replica. Example key `jobs:server:{query}:query-01` contains `8:query-01:job-100`; the corresponding `query-02` set contains `8:query-02:job-100`. |
| STRING + TTL | `heartbeat:server:{serverType}:serverId` | `serverType` | Heartbeat value for a replica. Example key `heartbeat:server:{query}:query-01` has a value such as `1` and an expiry set by the configured TTL. |

Example: `job:{query}:8:query-01:job-100`, `jobs:type:{query}`, `jobs:server:{query}:query-01`, `summary:type:{query}`, and `summary:server:{query}:query-01` all hash using the tag `query` and therefore map to the same cluster slot. The registry keys instead use `{registry}` so the two keys used by server registration share a slot.

### Hash-tag and Cluster constraints

Redis hashes the text inside the first non-empty pair of braces. For example, in `job:{query}:8:query-01:job-100`, the tag is `query`; the braces are delimiters, not part of the tag value. Keys with the same tag map to the same slot. A Lua script in Redis Cluster must receive all keys it accesses explicitly through `KEYS`, and those keys must be in the same slot.

The current partitioning unit is `serverType`, not `serverId`. This is what allows a type summary and each replica's summary to be updated by the same lifecycle script. It also means all jobs of one type share a slot. Changing to per-replica tags would spread traffic more widely, but a single script could no longer atomically update both a per-replica job record and one global per-type summary unless the data model or aggregation path changed.

### UI read pattern

The APIs support a summary-first flow:
1. Discover types using `getServerTypes()`.
2. Read a type aggregate using `getTypeSummary(serverType)`.
3. Read per-instance aggregates using `getServerSummaries(serverType)`.
4. On drill-down, read current jobs for a particular instance using `getServerJobs(serverType, serverId)`.

The summary APIs avoid scanning every job to render the overview. `getServerSummaries()` currently iterates the registered server IDs and reads each server summary, so its cost grows with the number of replicas for that type; it does not grow with the number of jobs. `getServerJobs()` returns all current jobs for an instance and should be paginated if an instance can own many jobs.

### Correctness and operational considerations

- Lua scripts validate expected key types and important state/counter conditions before writing. Keep validation ahead of writes as scripts are atomic but do not offer general rollback.
- The counters are materialized data. Add a reconciliation/repair procedure if operational failures, manual Redis changes, or future code paths could leave counters inconsistent.
- The registry set records registered instances; it does not automatically remove instances when a heartbeat expires. The UI should consult heartbeat freshness if it needs current liveness.
- Completed jobs are deleted by the current completion script. The system therefore tracks current in-flight jobs, not a durable history of completed jobs.
- Metadata is stored in the job hash as additional field/value pairs. Reserved fields (`jobId`, `status`, `serverType`, `serverId`) are supplied by the API/script rather than overridden by caller metadata.
- `RedisJobStatus` reads its enable flag at construction. `RedisClient` is a lazy singleton; its first initialization determines the client configuration for the process.

## 3. Pros and cons

### Advantages

- **Efficient dashboard reads:** the overview reads counters instead of scanning all jobs or polling every replica.
- **Consistent lifecycle accounting:** create/transition/complete scripts update job data, indexes, and summaries atomically with respect to other Redis commands.
- **Useful drill-down:** indexes and per-server summaries support type → replica → job navigation.
- **Cluster-safe multi-key operations:** explicit, consistent hash tags keep each script's keys in one slot.
- **Low operational complexity:** no separate aggregation service or periodic full scan is required for the normal read path.
- **Optional feature:** the status subsystem can be disabled without initializing RedisJobStatus's client path.

### Costs and risks

- **Hot-slot risk:** every key for one server type shares one slot, so a very busy type may bottleneck on one primary.
- **Duplicate job-ID risk:** the composite reference includes `serverId` and `jobId`, so equal application job IDs on different replicas of the same type remain distinct.
- **Counter drift risk:** any untracked mutation, manual key change, or future lifecycle path can make materialized summaries inconsistent unless repaired.
- **Read fan-out by replica:** `getServerSummaries()` performs one summary read per registered server, which is reasonable for modest fleets but can become noticeable with very large fleets or frequent refreshes.
- **No completed-job history:** completion deletes the hash and indexes, limiting post-completion audit and UI history.
- **Registry growth/staleness:** server membership is not removed automatically when a heartbeat expires, so stale entries can remain in discovery results.
- **Script constraints:** every key touched by a script must be passed explicitly and share one slot; scripts must remain short and validate before writing.

## 4. Alternatives and possible improvements

### A. Keep the current design, add observability and reconciliation

**Best when:** traffic is currently within capacity and type-level atomic summaries are valuable.

Keep `{serverType}` as the tag. Add metrics for script latency, Redis node/slot utilization, summary-read latency, and counter inconsistencies. Provide an administrative reconciliation job that recomputes summaries from current job indexes/hashes and reports discrepancies. This is the lowest-risk path.

### B. Make job storage identity include the replica

**Best when:** job IDs are not guaranteed unique within a server type.

Preserve the caller's original `jobId` field for display, but use a composite storage identity such as `(serverType, serverId, jobId)`. All create, transition, completion, index, and detail-lookup paths must use the same identity. The UI drill-down must supply `serverId` when IDs can collide. A new key could be `job:{serverType}:serverId:jobId`; this fixes key collision while preserving the current slot partition, but the indexes must also distinguish duplicate IDs rather than storing only an ambiguous ID. Plan a migration if existing data is live.

### C. Shard lifecycle keys more finely and aggregate asynchronously

**Best when:** profiling shows a hot `{serverType}` slot is limiting throughput.

Partition job records by a finer key, such as server instance or a deterministic shard. Maintain shard-local counters atomically with job changes, then aggregate those shard summaries for the dashboard. This spreads write load, but sacrifices the ability to atomically update a single global type counter in the same Lua script across multiple slots. The UI may see a small aggregation delay, and the aggregation pipeline needs retry, idempotency, and recovery semantics.

### D. Publish status events and build a separate read model

**Best when:** write throughput and UI query load both grow substantially, or consumers need more than current counts.

Emit lifecycle events to a durable stream or message system, and have a consumer build a read-optimized summary store. This decouples worker writes from dashboard aggregation and supports history, but introduces eventual consistency, duplicate/out-of-order event handling, consumer operations, and a replay/rebuild strategy. Redis Streams may be enough for a Redis-centered deployment; a separate event platform may be justified only by broader needs.

### E. Retain completed-job history separately

**Best when:** support, audit, or UI workflows need to inspect completed jobs.

Keep the current in-flight indexes and counters for live work, but write completed records to a time-bounded history store or append-only stream before removing the active record. Define retention and privacy policies; do not leave completed records in the active indexes if the dashboard expects only in-flight jobs.

## 5. Recommendation

Keep the summary-first, push-to-Redis architecture. It is a good default for the current dashboard and is much more scalable than polling every replica or scanning every job on every refresh.

Prioritize these checks before a larger rollout:

1. Decide whether `jobId` is unique across all replicas of a `serverType`. If not, fix the storage identity and indexes while preserving the original displayed ID.
2. Measure per-type slot load and Lua latency under realistic replica/job volume.
3. Add counter reconciliation and heartbeat-aware handling of stale server registrations.
4. Add pagination to job drill-down if per-instance job counts can become large.
5. Move to finer-grained sharding or an event-driven read model only when measured load or new product requirements justify the extra complexity.

## References

- `.7/RedisJobStatus.h` and `.7/RedisJobStatus.cc`: public API, key builders, read APIs, and client configuration.
- `.7/RedisJobScripts.inc`: embedded create, transition, completion, and server-registration scripts.
- `.7/luareadme.md`: Lua execution model and Redis Cluster hash-tag rationale.


## .7 fixed identity and collision handling

Each worker-side `RedisJobStatus` object has a fixed `serverType` and `serverId` in its configuration. Worker lifecycle calls no longer accept those identifiers on every call. Job keys and both indexes use a length-prefixed composite reference, while the hash's `jobId` remains the unchanged application-generated value used by the UI. The existing `{serverType}` hash tag is preserved, so lifecycle scripts remain single-slot operations.
