# Redis Job Status API and Colocated Heartbeat Services

This directory is the current colocated implementation of the C++17 Redis-backed job lifecycle, worker status, heartbeat publishing, and expired-worker cleanup. It uses Redis hashes and sets for job records, indexes, counters, metadata, server state, and heartbeats. Lua scripts perform multi-key operations atomically within the Redis hash slot that owns those keys.

## Configuration and object lifetime

The default `RedisJobStatus` constructor reads these host-application registry values through `get_reg_val(const std::string&)`:

| Registry key | Purpose |
|---|---|
| `redisJobStatusEnabled` | Enables or disables status reporting |
| `redisJobStatusServerType` | Worker/server type used in Redis keys and job ownership |
| `redisJobStatusServerId` | Stable identity of this worker/server instance |

`redisJobStatusEnabled` accepts `true`/`false`, `1`/`0`, and `yes`/`no`, case-insensitively. Empty or unset defaults to enabled. Configuration is read when each `RedisJobStatus` object is constructed; the object retains its configured identity.

Use the process-wide `RedisJobStatus::Instance()` as the public construction path. It retains the configured server identity and shared Redis client for the process lifetime; use the returned `shared_ptr` for registration, lifecycle calls, reads, and heartbeat publishing. The constructor is private. When disabled, result-returning mutators return `Disabled`, void mutators do nothing, and read methods return empty collections.

Tests can inject a configuration provider on the first singleton call:

```cpp
const auto status = RedisJobStatus::Instance([] {
  RedisJobStatus::Config config;
  config.enabled = false;
  return config;
});
```

The singleton is initialized only once per process; later providers are ignored. Tests needing multiple independent configurations should use separate processes or a dedicated test seam rather than attempting to reconstruct the singleton.

The `RedisClient::Instance()` singleton is initialized on its first call. Its default provider reads `redisUrl` and `isRedisCluster` through the host registry. A custom provider must be supplied on that first call; later providers are ignored. Initialize host configuration before constructing `RedisJobStatus` or making the first client call.

## Worker startup

The worker uses the public singleton entry points:

```cpp
const auto status = RedisJobStatus::Instance();
status->registerServer(typeMetadata, serverMetadata);

const auto manager = RedisHeartbeatManager::Instance();
manager->HeartbeatStart();
```

- `RedisHeartbeatManager::Instance()` creates the process-wide manager and starts its cleanup-scanning thread. The distributed Redis lease elects the normal global registry scan owner across all worker processes.
- `HeartbeatStart()` starts one heartbeat-publishing thread in this process. It obtains `RedisJobStatus::Instance()`, publishes immediately, then calls `refreshHeartbeat(heartbeatTtlSeconds)` every configured refresh interval.
- Both singleton access and `HeartbeatStart()` are idempotent within a process. The first singleton call fixes that singleton's configuration provider; later providers are ignored.

Use the same `RedisJobStatus::Instance()` object for registration and job lifecycle operations. The heartbeat thread catches and logs refresh errors and retries on the next interval. The worker's normal event/work loop runs independently. The manager owns the cleanup thread; its heartbeat runner is stopped when the process-wide manager is destroyed during orderly shutdown.

## TTLs and timing configuration

There are **two Redis keys with expiration (TTL) in this implementation**. They serve different purposes and must be configured independently:

| TTL | Redis key | Current value | What it controls | Where to change it |
|---|---|---:|---|---|
| Worker heartbeat TTL | `heartbeat:server:{<serverType>}:<serverId>` | 30 seconds | How long a worker's last heartbeat remains present after its most recent successful refresh. When it expires, the cleanup scanner may fence the worker. | `RedisHeartbeatManager::Config::heartbeatTtlSeconds`. |
| Distributed cleanup-lease TTL | `servers:{registry}:cleanup-lease` | 30 seconds (30,000 ms) | How long the registry-scan ownership lease remains valid if its owner stops renewing it. This coordinates cleanup scanners; it is **not** a worker heartbeat. | `RedisHeartbeatManager::Config::cleanupLeaseTtlMilliseconds`. |

The current related intervals are **not TTLs**:

| Interval | Current value | Where to change it |
|---|---:|---|
| Worker heartbeat refresh interval | 10 seconds | `RedisHeartbeatManager::Config::heartbeatRefreshIntervalSeconds`. |
| Cleanup scan interval | 5 seconds | `RedisHeartbeatManager::Config::cleanupScanIntervalSeconds`. |
| Cleanup-lease renewal interval | 5 seconds | `RedisHeartbeatManager::Config::cleanupLeaseRenewIntervalSeconds`. |

**How to change the timing:** supply a `RedisHeartbeatManager::Config` provider on the first `RedisHeartbeatManager::Instance(provider)` call. For example, set `heartbeatTtlSeconds = 60` and `heartbeatRefreshIntervalSeconds = 15` for a 60-second heartbeat refreshed every 15 seconds. Keep the refresh interval comfortably shorter than the TTL. The same configuration controls the cleanup lease TTL, lease renewal interval, scan interval, and tombstone retention.

These values default in `RedisHeartbeatManager.h` and can be overridden through the singleton's first-call configuration provider. They are not read from the host registry. After changing them, rebuild and deploy the worker. The two TTLs do not need to be equal. A heartbeat TTL controls worker-liveness detection; the cleanup-lease TTL controls scanner coordination.

**Important expiry behavior:** expiration of the worker heartbeat key does not itself change the worker's state. The cleanup scanner must run and execute the fencing script before lifecycle operations are rejected. Detection therefore also depends on the cleanup scan interval and Redis/network/scheduling delays.

## Public job-status API

```cpp
const auto status = RedisJobStatus::Instance();

status->registerServer(typeMetadata, serverMetadata);
status->refreshHeartbeat(30); // optional explicit/manual refresh

status->createPendingJob("job-100", jobMetadata);
status->transitionJob("job-100", RedisJobStatus::State::Active);
status->transitionJob("job-100", RedisJobStatus::State::Suspended);
status->transitionJob("job-100", RedisJobStatus::State::Active);
status->completeJob("job-100");

auto types = status->getServerTypes();
auto typeCounters = status->getTypeSummary("query");
auto servers = status->getServerSummaries("query");
auto jobs = status->getServerJobs("query", "query-01");
```

The configured identity is used by registration, lifecycle operations, and heartbeats. Read APIs accept the server type and, where needed, server ID explicitly. The constructor is private; acquire the object through `RedisJobStatus::Instance()`.

Supported transitions:

- `PENDING → ACTIVE`
- `ACTIVE → SUSPENDED`
- `SUSPENDED → ACTIVE`

Repeating the current state returns `NoOp`. Transitioning to `PENDING` is unsupported. `completeJob()` removes the job and decrements its state and total counters.

C++ result values are part of the Lua/C++ interface and must remain aligned:

| Result | Numeric value |
|---|---:|
| `Success` | 0 |
| `NoOp` | 1 |
| `JobNotFound` | 2 |
| `InvalidTransition` | 3 |
| `InconsistentData` | 4 |
| `JobAlreadyExists` | 5 |
| `Disabled` | 6 |
| `ServerNotActive` | 7 |

## Metadata and reserved fields

Metadata values are stored as strings in Redis hashes.

- Job metadata is supplied to `createPendingJob(jobId, metadata)`.
- Server-type metadata is supplied as the first `registerServer()` argument and merged into the type summary.
- Server-instance metadata is supplied as the second argument and merged into that instance's summary.
- Updates replace supplied matching fields and leave unspecified fields unchanged.
- Summary metadata fields are prefixed with `meta:`; counter APIs omit these fields.

Job hash fields owned by the API are `jobId`, `status`, `serverType`, and `serverId`. Caller metadata using these names is ignored; the API writes authoritative values. Metadata field names must not be empty.

The type summary initializes `meta:createdAt` once in UTC ISO-8601 form using `HSETNX`. Caller-supplied server-type metadata named `createdAt` is rejected. Server-instance metadata has no additional reserved names.

## Redis key layout

| Redis type | Key pattern | Purpose |
|---|---|---|
| SET | `servers:{registry}:types` | Global server-type discovery |
| SET | `servers:{registry}:type:<serverType>` | Registered server IDs for one type |
| HASH | `summary:type:{<serverType>}` | Type counters and metadata |
| HASH | `summary:server:{<serverType>}:<serverId>` | Server counters and metadata |
| HASH | `job:{<serverType>}:<serverId-length>:<serverId>:<jobId>` | Job state, ownership, and custom fields |
| SET | `jobs:type:{<serverType>}` | Composite references for current jobs of a type |
| SET | `jobs:server:{<serverType>}:<serverId>` | Composite references for jobs owned by a server |
| STRING with TTL | `heartbeat:server:{<serverType>}:<serverId>` | Last heartbeat value |
| SET | `servers:live:{<serverType>}` | IDs that successfully published a heartbeat |
| HASH | `servers:state:{<serverType>}` | Per-server state: `ACTIVE` or terminal `DELETING` tombstone |
| ZSET | `servers:retired:{<serverType>}` | Server IDs awaiting Phase C; score is Redis retirement time in Unix milliseconds |

Job keys and indexes use a length-prefixed composite reference derived from server ID and original application job ID. This avoids separator ambiguity. The job hash retains the original application job ID in its `jobId` field.

## Redis Cluster hash-tag design

Redis Cluster uses the text inside the first non-empty pair of braces as a hash tag. All keys accessed by one multi-key Lua script must resolve to the same hash slot.

Job lifecycle keys use `{serverType}`; job hashes, type/server indexes, summaries, heartbeat, live membership, and state for that type share a slot. Different server types can be distributed across slots.

Registration updates `servers:{registry}:types` and `servers:{registry}:type:<serverType>` in one script; both use the literal `{registry}` tag. Registry keys intentionally occupy a different slot from per-type data. Type-summary initialization and server-summary metadata updates therefore run as separate operations.

Pass every accessed key explicitly through Lua `KEYS` and preserve the same-slot requirement. Redis Lua scripts execute without interleaving, but they are not rollback transactions: validate key types and state before writing.

## Lua argument conventions

Embedded Lua raw strings live in `RedisJobScripts.inc`, included by `RedisJobStatus.cc` and `RedisHeartbeatManager.cc`; it is not a standalone Lua file.

`appendMetadataArgs()` appends a metadata pair count followed by accepted field/value pairs. Keep this encoding aligned with the corresponding Lua scripts.

For job creation, the six keys are the job hash, type job index, server job index, type summary, server summary, and per-type lifecycle state hash. `ARGV[1..4]` contain original job ID, server type, server ID, and composite reference. `ARGV[5]` is the accepted metadata pair count; pairs begin at `ARGV[6]`.

Type-summary metadata initialization receives the creation timestamp, metadata pair count, then pairs. Server-summary metadata receives the pair count followed by pairs.

## Colocated cleanup architecture

Every worker process starts its own `RedisHeartbeatManager` cleanup thread. Each manager attempts the shared `servers:{registry}:cleanup-lease` lease (30-second TTL, unique token). The holder normally scans all registered server types and IDs, so it can clean an expired worker other than itself. Lease renewal and release compare the token, preventing a stale owner from changing a successor's lease.

The lease is a scan-load optimization, not a correctness lock. A scan may outlive the lease and overlap another scan. Per-server Lua fencing and idempotent cleanup remain authoritative.

Heartbeat refresh and fencing serialize in the per-type slot. Fencing verifies heartbeat absence, ACTIVE state, and live-set membership, then marks the server terminal `DELETING`. Lifecycle mutators check `ACTIVE` in Lua: if the lifecycle script runs first, cleanup accounts for it; if fencing runs first, later lifecycle calls return `ServerNotActive`.

**TTL caveat:** heartbeat-key expiry alone does not change the server's ACTIVE state. Until a cleanup manager runs the fence script, a lifecycle mutator may still succeed after TTL expiry. Immediate rejection at TTL expiry would require lifecycle scripts to also test heartbeat-key existence; that stricter policy is not implemented.

Cleanup is a three-phase protocol across the Redis Cluster slot boundary:

1. **Phase A — fence and clean (per-type slot).** The fencing Lua script verifies heartbeat absence, ACTIVE state, and prior live-set membership. In the same atomic script it changes the state to `DELETING` and adds the ID to `servers:retired:{<serverType>}` with a timestamp from Redis `TIME`. The cleanup Lua script preflights job ownership, indexes, statuses, key types, and counters; deletes the server's jobs and summary; updates type counters; removes heartbeat/live membership; and retains the `DELETING` tombstone. Repeated Phase A execution does not decrement counters twice.
2. **Phase B — remove registry membership (registry slot).** After Phase A reports newly completed or already completed cleanup, the manager removes only that server ID from `servers:{registry}:type:<serverType>`. A crash after Phase A is recovered by a later scan.
3. **Phase C — collect old tombstones (per-type slot).** The manager discovers candidates from the retired sorted set, not the registry (Phase B has removed those IDs). After checking in the registry slot that Phase B is complete, a per-type Lua script verifies that state is still `DELETING`, the Redis-time retention period elapsed, and heartbeat, server-job index, server summary, and live membership are all absent. It then removes only that server ID from the state hash and retired sorted set. Default retention is 24 hours and is configurable through `RedisHeartbeatManager::Config::retiredTombstoneRetentionSeconds`.

The retired timestamp is written atomically with the `ACTIVE → DELETING` transition, so a crash cannot leave a newly fenced tombstone without a Phase C discovery record. Redis hashes have no per-field TTL; Phase C explicitly removes one hash field rather than expiring the whole state hash.

**Upgrade from the two-phase version:** older deployments may already have `DELETING` fields whose IDs were removed from the registry before the retired sorted set existed. Each cleanup scan incrementally HSCANs the per-type state hash and backfills missing retired-index entries with Redis time using `ZADD NX`. Those legacy tombstones therefore receive a fresh full retention window after upgrade; existing timestamps are preserved. This migration is idempotent and runs in the per-type slot.

**Registration invariant required by Phase C:** each server ID is globally unique, never reused, and `registerServer()` is called exactly once for it during startup. No delayed or retried startup registration may resume after retirement. The registration API can initialize a missing state field, so this non-replayable startup contract prevents a Phase C-collected ID from being recreated as `ACTIVE`. Heartbeat and job lifecycle scripts do not initialize missing state. Register before the first heartbeat and do not call registration again for that ID.

The registry membership check and Phase C Lua script cannot be one atomic operation because they use different Redis Cluster slots. The once-only registration invariant makes the check stable: after Phase B removes membership, no code path may add that ID again. Phase C still checks all per-type safety conditions atomically.

**Startup crash limitation:** registration currently activates the per-type state before adding the ID to the registry, then performs metadata initialization in separate operations. If the process crashes in that sequence, an `ACTIVE` state can be left without registry membership; if it crashes after registry insertion but before its first heartbeat, the scanner deliberately refuses to fence it because it has not joined the live set. Such partial-startup records are not automatically recovered by the current scanner. The worker must complete registration and start its heartbeat promptly, and operators must monitor/reconcile incomplete startup records. A fully automatic solution requires a durable startup-grace/registration protocol spanning the registry and per-type slots; simply removing the first-heartbeat guard would introduce a race that can fence a healthy starting worker.

The cleanup lease remains a scan-load optimization, not a correctness lock. A stale manager may resume after lease expiry, but its fence script cannot transition a missing/non-`ACTIVE` state, its cleanup script requires `DELETING`, and Phase C removes a tombstone only after Phase A data is gone and Phase B membership is absent. Redis Lua does not roll back earlier writes after runtime errors; cleanup scripts preflight before writing.

## Dashboard read consistency

`getServerJobs()` first discovers references, then executes one Lua snapshot with the index and all referenced job hashes declared in `KEYS`. The script revalidates index membership and reads the hashes atomically. If cleanup changed the index between discovery and snapshot execution, the read returns an error rather than a partial list; callers may retry.

`getServerSummaries()` discovers IDs in the `{registry}` slot, then reads all extant summary hashes in one Lua execution in the `{serverType}` slot. Summary hashes are mutually consistent at that script's execution point. The two steps cannot form one global point-in-time snapshot across Redis Cluster slots; a summary removed after registry discovery is omitted.

## Source files

- `RedisClient.h/.cc` — process-wide Redis or Redis Cluster client.
- `RedisJobStatus.h/.cc` — public API and C++ implementation.
- `RedisJobScripts.inc` — embedded lifecycle, heartbeat, fencing, three-phase cleanup, lease, tombstone-index, and snapshot Lua.
- `RedisHeartbeatManager.h/.cc` — process-local cleanup and heartbeat thread entry points.
- `heartbeat.cc` — worker startup integration example.

## Build and validation

Requires C++17, redis-plus-plus, hiredis, and host-provided `get_reg_val(const std::string&)` and `initializeConfiguration()`. Compile `RedisClient.cc`, `RedisJobStatus.cc`, `RedisHeartbeatManager.cc`, and `HeartbeatRunner.cc` into the worker, with the project's Redis include/link settings. The `heartbeat.cc` file is an example main using the actual singleton API; integrate its startup sequence into the host worker rather than linking a second main.

The code has not been compiled or tested against live Redis or Redis Cluster. Verify the installed redis-plus-plus version supports `eval<std::vector<std::string>>`. Test lease acquisition/takeover and stale tokens, lease expiry during slow scans, heartbeat/fencing and lifecycle races, Phase A/Phase B recovery, Phase C retention and tombstone collection, stale managers resuming before/after Phase C, the one-time registration invariant, dashboard reads concurrent with cleanup, malformed key types, counter integrity, and actual Cluster slot routing before deployment.

## Cleanup coordination and stale managers

### Key question

What happens if a cleanup manager acquires the Redis cleanup lease, pauses long enough for that lease to expire, and then resumes after another manager has acquired the lease? Could both managers continue cleanup and delete the same server's data?

### Analysis

The cleanup lease coordinates normal scan ownership; it is not the sole correctness mechanism. It is acquired with `SET NX PX`, renewed only when the stored owner token matches, and released only by its current owner. If a manager loses the lease, its next renewal attempt fails. A paused manager can nevertheless resume and execute code before it reaches that renewal check, so lease expiry alone does not stop an old process.

Correctness is protected at the per-server level by the Redis Lua lifecycle scripts:

- **Atomic fencing:** `kFenceExpiredServerScript` checks that the heartbeat is absent, that the server has published its first heartbeat, and that its state is `ACTIVE`. It then changes the state to `DELETING` atomically. Heartbeat refresh and this transition operate in the same per-type Redis Cluster slot, so Redis serializes the competing scripts. Whichever runs first determines whether the heartbeat is refreshed or the server is fenced.
- **No reactivation or new work:** Once the state is `DELETING`, heartbeat refresh and job creation/transition operations that require an `ACTIVE` server are rejected. Registration also does not reactivate a terminal server ID.
- **Atomic, repeat-safe cleanup:** `kCleanupExpiredServerScript` rechecks the `DELETING` state and absence of the heartbeat before making changes. Its validation and deletions/counter updates execute in one Lua script. If another manager has already completed Phase A, the script recognizes the completed shape and returns the already-cleaned result rather than decrementing counters again.
- **Recoverable registry cleanup:** The manager removes the ID from the registry after Phase A reports completion. The retained `DELETING` tombstone and retired sorted-set entry make cleanup interrupted between phases discoverable.
- **Delayed tombstone collection:** Phase C confirms registry absence, then atomically verifies retention age and absence of per-server data before removing the individual tombstone. A stale manager holding an old registry snapshot cannot recreate state: fencing only transitions `ACTIVE`, and cleanup only operates on `DELETING`.

Therefore, two managers may briefly overlap after a lease expires, but their per-server operations are serialized and guarded by lifecycle state. For the operations currently implemented, a separate leader-election protocol, quorum, or cleanup fencing-token subsystem is not required. The lease reduces unnecessary concurrent scans; the per-server Lua scripts protect data correctness.

### Assumptions and limits

This safety argument depends on all lifecycle mutations and destructive cleanup remaining behind the guarded Lua scripts, on the scripts continuing to operate on the appropriate per-type Redis slot, and on server IDs being unique and never reused. Phase C additionally requires the enforced startup contract that `registerServer()` runs exactly once per ID and cannot be replayed or resume late after retirement. Any future destructive operation performed outside those checks must be reviewed for stale-manager safety. The lease token is not passed to the per-server scripts, so it should not be treated as a hard execution fence.

This is a source-level analysis, not a demonstrated failure-injection result. Before deployment, test lease expiry while a manager is paused, takeover by a second manager, concurrent fencing/heartbeat refresh, overlapping Phase A cleanup, and recovery between Phase A and Phase B.

