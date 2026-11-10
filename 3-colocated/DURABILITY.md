# Durability and transient-counter assessment

## Executive assessment

This implementation is a good fit for **live operational counters for roughly 100 worker/server instances**, where instances may start and stop independently or in large groups and where the counters describe only the jobs that are currently present in Redis.

The important distinction is that the design provides **atomicity and recovery for normal lifecycle operations**, not durable historical accounting. Redis is the live-state store. The implementation keeps the aggregate counters consistent as jobs are created, transitioned, completed, or removed during stale-server cleanup, provided Redis retains the relevant keys and all writers use the guarded C++/Lua API.

For around 100 servers, the server-count scale itself is modest. The main scaling constraint is the chosen Redis Cluster partitioning: all data for one `serverType`, including its aggregate counters and every server's jobs, shares one hash slot and is therefore handled by one Redis Cluster primary. This is an intentional trade-off that makes per-type aggregate updates atomic.

## What is handled well

### 1. Concurrent job activity across servers

For a given server type, the job hash, type index, server index, server summary, type summary, and lifecycle state are all in the same Redis Cluster slot.

The create, transition, and complete operations use Lua scripts in that slot. Redis serializes those scripts with each other and with cleanup scripts:

- **Create:** verifies that the server is `ACTIVE`, the job key is absent, neither job index already contains the reference, and all counters can be incremented before writing the job, indexes, and counters.
- **Transition:** verifies ownership, both index memberships, allowed state transition, and the affected counters before changing the job state and counters.
- **Complete/delete:** verifies ownership, status, both indexes, and all counters before decrementing counters and deleting the job and references.
- **Repeated completion:** the first successful call removes the job and decrements counters once. A subsequent call returns `JobNotFound`; it does not decrement again.

This is the central correctness strength for a fleet where many servers are changing jobs concurrently.

### 2. Servers stopping independently or together

A server whose heartbeat expires is fenced by an atomic per-type Lua operation:

1. It verifies that the heartbeat is absent and the state is `ACTIVE`.
2. It changes the state to `DELETING` and records a Redis-time retirement timestamp.
3. Lifecycle mutations then reject that server because it is no longer `ACTIVE`.
4. Cleanup validates the server's indexed jobs, their ownership and statuses, and the type-level aggregate counters before making any changes.
5. It decrements the type counters by the actual validated jobs belonging to that server, removes those jobs and indexes, deletes the per-server summary and heartbeat, and removes live-set membership.
6. Registry removal and eventual tombstone collection happen in later phases.

Because the fence and job mutations use the same per-type slot, a job operation racing with fencing is serialized: it either completes before the fence and is included in cleanup, or runs after the fence and is rejected. The cleanup script also recognizes an already-cleaned server, so retrying after a crash does not decrement its counters a second time.

If a few servers stop, only those servers are cleaned. If all 100 stop, they are processed individually by the elected cleanup scanner; the type aggregate is reduced as each server's cleanup commits. There is no single atomic transaction for all 100 servers, so readers can observe a valid intermediate state while cleanup is progressing.

### 3. Cleanup scanner redundancy

Each process has a cleanup loop, but a Redis lease normally elects one process to perform the registry scan. The lease reduces duplicate scan load; it is not relied on for correctness.

If the lease expires during a slow scan, scans can overlap. Per-server fencing, state checks, and idempotent cleanup are the correctness mechanisms. A crash after Phase A but before registry removal is recoverable: a later scan recognizes the already-cleaned tombstone and removes the registry membership. Phase C later collects the tombstone after the retention period and registry-absence check.

### 4. Startup and first-heartbeat handling

Registration makes the per-type `ACTIVE` state the final registration step and generates `meta:registeredAt` using Redis TIME in that same per-type activation script. A server that registers but never publishes its first heartbeat is eligible for fencing after the configured first-heartbeat grace period (60 seconds by default).

A first heartbeat racing with fencing is serialized in the same slot. Once fenced, heartbeat refresh and job lifecycle writes cannot reactivate the server.

## Timing and expected behavior

Current defaults in `RedisHeartbeatManager::Config`:

| Setting | Default | Operational effect |
|---|---:|---|
| Heartbeat refresh | 10 seconds | Each running server attempts a refresh at this interval |
| Heartbeat TTL | 30 seconds | A stopped/unresponsive server becomes eligible after its last heartbeat expires |
| Cleanup scan interval | 5 seconds | Bounds the normal delay until the scanner next checks an expired heartbeat |
| First-heartbeat grace | 60 seconds | Allows a newly registered server time to publish its first heartbeat |
| Tombstone retention | 24 hours | Keeps retired state discoverable before Phase C removes it |

A server that stops after successfully heartbeating is normally detected after roughly the 30-second heartbeat TTL plus up to one scan interval, with additional Redis/network/scheduling delay. This is not an exact deadline. The code does not currently provide an immediate graceful-stop/unregister path; an orderly process shutdown that simply stops heartbeats is detected through TTL expiry too.

A server that registers but never sends its first heartbeat is handled by the 60-second registration grace period, followed by the next cleanup scan.

## Important durability boundary

The word *durability* needs qualification here:

- **Within a live Redis primary:** Lua scripts provide atomic execution relative to other commands. Preflight validation avoids known counter underflow, overflow, malformed-value, and stale-index failure cases before mutation.
- **Across worker process restarts:** live Redis job state and counters remain available. A restarted worker must use a new globally unique server ID; server IDs are never reused. A retired ID remains fenced and is not a supported identity for a new process.
- **Across Redis restart, data loss, or failover:** this code does not itself guarantee persistence of acknowledged writes. It does not configure Redis RDB/AOF, replication, `WAIT`, or `WAITAOF`. Actual survival depends on the Redis deployment's persistence and replication settings. If Redis loses the live keys, the transient counters and job state are lost too.
- **Historical accounting:** this is not an audit ledger. Completed jobs are deleted, retired server summaries are deleted in Phase A, and old tombstones are eventually removed in Phase C. There is no replay/rebuild mechanism in this implementation that can reconstruct historical totals after Redis data loss.

That behavior is consistent with the stated goal of transient current-state counters. If the requirement changes to billing, compliance, or historical reporting, those records need a separate durable source of truth.

## Scale assessment for approximately 100 servers

### Server count

Around 100 server instances is a reasonable scale for the current design. The registry contains a modest number of IDs, each heartbeat is a small TTL refresh, and the cleanup scanner processes expired servers one at a time. Simultaneous stops increase cleanup work but do not create a correctness race or cause all server counters to be cleared in one unvalidated operation.

### Redis Cluster placement and throughput

All keys for one `serverType` use the same hash tag, `{serverType}`. This is necessary for atomic type-level counters and same-type cleanup, but it means:

- 100 servers of the **same type** share one Redis Cluster slot/primary for their job data and aggregate counters.
- Adding Redis Cluster shards does not spread the load for that one type.
- Different server types can be distributed across different slots/primaries.
- The type summary hash is a shared write target for every create, transition, completion, and server cleanup of that type.

Therefore, 100 servers alone is not a concern; the more important sizing input is the **job event rate per server type** (creates + transitions + completions), plus the number of jobs accumulated on a single server before cleanup. Load-test the busiest type rather than assuming cluster shard count will distribute one type's traffic.

### Cleanup work and large job sets

The cleanup scan reads the registry and checks servers sequentially. The tombstone migration uses HSCAN in bounded batches, but the current scan loop completes the HSCAN cursor traversal for each type during that scan.

Phase A cleanup currently passes all of one server's job references and job keys to a single Lua script. That is attractive for correctness because counter validation and deletion are one atomic per-type operation. However, a server with a very large job set can produce a large EVAL request and a long-running script that blocks other commands on that Redis primary while it runs. For the expected fleet size this may be acceptable, but the maximum jobs per server should be measured and tested. If that cardinality can become very large, cleanup needs a carefully designed bounded/chunked protocol that preserves fencing and exact counter accounting; simply splitting the current script into arbitrary chunks would weaken its atomicity.

## Constraints and failure cases to operate explicitly

1. **Unique, never-reused process IDs are mandatory.** A process restart must receive a new server ID. Reusing an ID after it has been fenced is unsupported.
2. **Registration is not globally atomic across Cluster slots.** Registry insertion is in the `{registry}` slot while activation is in the `{serverType}` slot. A crash between them can leave a stale registry member without per-type state. The scanner skips it; reconciliation is required. The final activation ordering prevents an ACTIVE server from being invisible to registry-driven scanning, but it cannot eliminate this partial-registration case.
3. **Redis loss is not repaired from an external source.** If Redis is flushed or loses data, the implementation does not rebuild in-flight jobs/counters from the workers.
4. **Cleanup is eventual, not immediate.** Heartbeat expiry plus scan cadence governs detection. A graceful stop still waits for expiry under the current API.
5. **Read consistency is scoped.** Job snapshots are validated atomically within a type slot. Registry discovery and per-type summary retrieval cross slots, so a fleet-wide summary view is not one global point-in-time snapshot.
6. **All writers must use the API/scripts.** Direct Redis edits, the Python browser mock's direct deletion operations, or other writers that bypass the C++ lifecycle scripts can invalidate indexes and counters. The production UI design should send a delete request to the owning server and have that server call `completeJob()`; it should not mutate Redis counters itself.
7. **Redis configuration is external.** Eviction policy, persistence, replication/failover, memory sizing, and backup policy are not configured by these source files. Since these keys represent current state, avoid an eviction policy that can independently evict individual job/index/summary keys and leave partial logical state.

## Recommended validation before production

The design is coherent for the stated transient-counter use case, but the source review is not a load or failure test. Before relying on it for a 100-server fleet, test at least:

- Start 100 servers of one type concurrently; verify registry membership, ACTIVE state, heartbeats, and zero initial counters.
- Start a mixed subset, then the remaining servers; create and transition jobs concurrently across all servers and compare the aggregate counters with a job-hash/index-derived expected count.
- Complete the same job concurrently from duplicate requests; verify exactly one success and no duplicate decrement.
- Stop a few, many, and all servers abruptly; verify each server is fenced once, its jobs are removed, and type counters converge to the remaining live jobs.
- Repeat with orderly shutdown, delayed first heartbeat, heartbeat refresh racing with fencing, and server restarts using new IDs.
- Inject process/connection failure after Phase A and before Phase B, and after Phase B but before Phase C; verify later scans recover.
- Corrupt counters and indexes deliberately; verify preflight rejects cleanup without partial counter changes.
- Load-test the busiest server type at expected peak job event rate and maximum per-server job cardinality, including the longest cleanup script duration.
- Test the actual Redis deployment's restart and failover behavior separately; do not infer persistence guarantees from Lua atomicity.

## Overall conclusion

For roughly 100 servers that start and stop in changing subsets, the implementation is **well-shaped for transient live counters**: lifecycle changes are serialized per server type, aggregate counters are updated atomically with job state, stale workers are fenced before cleanup, and cleanup retries are designed not to double-decrement.

The main qualifications are not a basic flaw in the counter algorithm. They are the intentional single-primary-per-type throughput boundary, eventual heartbeat-based stop detection, the requirement for unique non-reused server IDs, the non-atomic cross-slot registration workflow, and the absence of a Redis data-loss rebuild mechanism. With those constraints accepted and the fleet/load/failure tests above passed, the design is appropriate for the stated purpose.