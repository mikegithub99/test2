# Redis Job Status API and Colocated Heartbeat Services

This directory is the current colocated implementation of the C++17 Redis-backed job lifecycle, worker status, heartbeat publishing, and expired-worker cleanup. It uses Redis hashes and sets for job records, indexes, counters, metadata, server state, and heartbeats. Lua scripts perform multi-key operations atomically within the Redis hash slot that owns those keys.

## Configuration and object lifetime

The default configuration provider used by `RedisJobStatus::Instance()` reads these host-application registry values through `get_reg_val(const std::string&)`:

| Registry key | Purpose |
|---|---|
| `redisJobStatusEnabled` | Enables or disables status reporting |
| `redisJobStatusServerType` | Worker/server type used in Redis keys and job ownership |
| `redisJobStatusServerId` | Stable identity of this worker/server instance |

`redisJobStatusEnabled` accepts `true`/`false`, `1`/`0`, and `yes`/`no`, case-insensitively. Empty or unset defaults to enabled. The singleton reads configuration on its first construction and retains that identity for the process lifetime; later configuration providers are ignored.

Use the process-wide `RedisJobStatus::Instance()` as the public construction path. It retains the configured server identity and shared Redis client for the process lifetime; use the returned `shared_ptr` for registration, lifecycle calls, reads, and heartbeat publishing. The constructor is private. When disabled, result-returning mutators return `Disabled`, void mutators do nothing, and read methods return empty collections. `RedisHeartbeatManager::Instance()` also avoids creating a Redis client or cleanup thread; `HeartbeatStart()` and `scanOnce()` are no-ops. The normal worker startup calls can therefore be made unconditionally.

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
| First-heartbeat startup grace period | 60 seconds | `RedisHeartbeatManager::Config::firstHeartbeatTimeoutSeconds`. |

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

The type summary initializes `meta:createdAt` once in UTC ISO-8601 form and `meta:createdByServerId` once with the ID of the first server to initialize that type summary, both using `HSETNX`. Caller-supplied server-type metadata named `createdAt` or `createdByServerId` is rejected. Server-instance registration initializes `meta:registeredAt` internally using Redis `TIME` during the final activation operation, atomically with creation of the `ACTIVE` state. Caller-supplied server-instance metadata named `registeredAt` is rejected.

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

Registration checks the current type's state hash in C++ and then updates `servers:{registry}:types` and `servers:{registry}:type:<serverType>` in one Lua script; both registry keys use the literal `{registry}` tag. Server IDs are type-prefixed and globally unique, so no cross-type state or registry scan is required. It then initializes type-summary metadata and server-summary metadata in separate operations, and finally activates the server in a per-type-slot Lua script that generates `meta:registeredAt` and writes `ACTIVE` atomically. Registry keys intentionally occupy a different slot from per-type data, so the complete registration workflow is not one atomic cross-slot operation.

Pass every accessed Redis key explicitly through `KEYS` and preserve the same-slot requirement. Registration receives `servers:{registry}:types` and the current `servers:{registry}:type:<serverType>` set as `KEYS[1]` and `KEYS[2]`. Since `serverId` is guaranteed to be type-prefixed and globally unique, Lua checks only current-type membership; C++ checks only the current type's state hash for an ACTIVE record or DELETING tombstone. No Lua key discovery or cross-type scan is needed. The state hash and registry keys occupy different Cluster slots, so the state check and registry insertion are separate operations rather than one cross-slot atomic operation. Redis Lua scripts execute without interleaving, but they are not rollback transactions: validate key types and state before writing.

## Lua argument conventions

Embedded Lua raw strings live in `RedisJobScripts.inc`, included by `RedisJobStatus.cc` and `RedisHeartbeatManager.cc`; it is not a standalone Lua file.

`appendMetadataArgs()` appends a metadata pair count followed by accepted field/value pairs. Keep this encoding aligned with the corresponding Lua scripts.

For job creation, the six keys are the job hash, type job index, server job index, type summary, server summary, and per-type lifecycle state hash. `ARGV[1..4]` contain original job ID, server type, server ID, and composite reference. `ARGV[5]` is the accepted metadata pair count; pairs begin at `ARGV[6]`.

Type-summary metadata initialization receives the creation timestamp, metadata pair count, then pairs. Server-summary metadata receives the pair count followed by pairs.

## Colocated cleanup architecture

Every Redis-enabled worker process starts its own `RedisHeartbeatManager` cleanup thread. Each manager attempts the shared `servers:{registry}:cleanup-lease` lease (30-second TTL, unique token). The holder normally scans all registered server types and IDs, so it can clean an expired worker other than itself. Lease renewal and release compare the token, preventing a stale owner from changing a successor's lease.

The lease is a scan-load optimization, not a correctness lock. A scan may outlive the lease and overlap another scan. Per-server Lua fencing and idempotent cleanup remain authoritative.

Heartbeat refresh and fencing serialize in the per-type slot. A successful first heartbeat adds the server ID to the live set; that membership is the durable marker that the server has heartbeated at least once (the heartbeat key itself has a TTL). Fencing checks heartbeat absence and ACTIVE state. If the live marker exists, normal heartbeat-expiry fencing applies. If it does not exist, the scanner uses the per-server registration timestamp and the configurable first-heartbeat startup grace period before fencing. The check and ACTIVE-to-DELETING transition are atomic with heartbeat refresh because both scripts run in the same per-type slot. Lifecycle mutators check ACTIVE in Lua: if the lifecycle script runs first, cleanup accounts for it; if fencing runs first, later lifecycle calls return ServerNotActive.

**TTL caveat:** heartbeat-key expiry alone does not change the server's ACTIVE state. Until a cleanup manager runs the fence script, a lifecycle mutator may still succeed after TTL expiry. Immediate rejection at TTL expiry would require lifecycle scripts to also test heartbeat-key existence; that stricter policy is not implemented.

Cleanup is a three-phase protocol across the Redis Cluster slot boundary. The phases deliberately use separate scripts/operations because Redis Cluster cannot run one Lua script across the `{registry}` and `{serverType}` slots.

### Phase A — fence and clean (one server-type slot)

Phase A runs entirely in the `{serverType}` slot. It has two atomic per-type Lua operations:

1. **Fence:** `kFenceExpiredServerScript` checks that the heartbeat key is absent, the lifecycle state is `ACTIVE`, and the server is eligible for expiry. A server that has heartbeated before is identified by membership in `servers:live:{<serverType>}`; it is fenced when its heartbeat TTL has expired. A server that has never heartbeated is not immediately considered dead: the script reads `meta:registeredAt` from that server's summary hash and waits until `firstHeartbeatTimeoutSeconds` has elapsed. For rolling upgrades it falls back to the legacy `registeredAt:<serverId>` field in the per-type state hash. The script then atomically changes the state from `ACTIVE` to `DELETING` and inserts the ID into `servers:retired:{<serverType>}` with a retirement timestamp from Redis `TIME`. Heartbeat refresh checks the same state hash in the same slot, so a first heartbeat racing the timeout either wins before fencing or is rejected after fencing.
2. **Clean:** `kCleanupExpiredServerScript` rechecks `DELETING` and heartbeat absence, validates key types, job ownership, index membership, statuses, and counters before writing. It removes this server's job hashes and server-job index, updates type-level counters, removes the per-server summary and heartbeat, and removes live-set membership. It deliberately retains the `DELETING` state tombstone and retired-index entry. A repeat after successful cleanup returns the already-cleaned result rather than decrementing counters twice.

The final activation Lua script generates `meta:registeredAt` internally using Redis `TIME` and `HSETNX` in the same per-type-slot operation that creates `ACTIVE`. The timestamp is not accepted from caller metadata and is committed atomically with activation. The default first-heartbeat grace period is 60 seconds. Configure it through `RedisHeartbeatManager::Config::firstHeartbeatTimeoutSeconds`. Choose a value longer than normal worker startup and initialization. This timeout does not count as a heartbeat: a server is live only after its first successful heartbeat.

### Phase B — remove registry membership (registry slot)

After Phase A has completed (newly or on a retry), the manager issues `SREM servers:{registry}:type:<serverType> <serverId>`. This is deliberately a separate registry-slot operation; Phase A must not try to access registry keys. Removing one member is idempotent. If the process crashes after Phase A but before Phase B, the retained tombstone and retired index allow a later scan to retry registry removal. The global `servers:{registry}:types` set is retained; it is the discovery list of server types, not an individual server registration.

### Phase C — collect old tombstones (one server-type slot, after a registry-slot check)

Phase C candidates are discovered from `servers:retired:{<serverType>}`, not from the registry, because Phase B has already removed their IDs. For each candidate, the manager first checks registry membership separately in the `{registry}` slot. If the ID is still registered, collection is skipped. It then runs `kCollectRetiredServerTombstoneScript` entirely in the per-type slot. That script verifies the state is still `DELETING`, the Redis-time retention period has elapsed, and the heartbeat, server-job index, server summary, and live-set membership are absent. It removes only that server ID's state-hash fields and retired sorted-set member; it never deletes the shared state hash. Default retention is 24 hours, configurable through `RedisHeartbeatManager::Config::retiredTombstoneRetentionSeconds`.

The registry check and Phase C script cannot be one atomic operation because they use different Cluster slots. Registration checks only the current type's state hash before attempting its atomic registry-slot insertion. The ID's type prefix guarantees it cannot collide with an ID from another type. An ID is rejected while it remains in the current type's registry or state hash, including a `DELETING` tombstone. Once Phase C removes the tombstone, the ID may be registered again. This is a deliberately coarse duplicate-registration guard, not proof of process identity: a delayed or restarted old process could register again after Phase C if it reuses the same ID. Workers must therefore generate a fresh ID for each process incarnation and must not replay registration after retirement. The per-type script independently verifies all per-type safety conditions.

### Upgrade and failure recovery notes

Older deployments may have `DELETING` fields without retired-index entries. Each scan incrementally HSCANs the state hash and backfills missing entries with Redis `TIME` using `ZADD NX`; legacy tombstones receive a fresh full retention period, while existing retired scores are preserved.

A crash after Phase A is recovered by retrying Phase B. A crash after Phase B leaves the retired-index entry for Phase C. The cleanup lease coordinates scan load but is not a correctness lock; overlapping scans are safe because per-server fencing and cleanup are state-checked and idempotent.

**First-heartbeat behavior:** if registration completes and the worker never sends its first heartbeat, the scanner fences it after the 60-second default startup grace period (or the configured value), then runs the same Phase A/B/C cleanup. If a first heartbeat arrives before the deadline, normal heartbeat TTL monitoring applies thereafter.

**Existing ACTIVE records during upgrade:** records created before registration timestamps were added may lack both `meta:registeredAt` in the server summary and the legacy `registeredAt:<serverId>` state-hash field. The fence script intentionally does not guess their age, so a never-heartbeated legacy ACTIVE record without either timestamp is not automatically expired by the startup timeout. Existing servers with a live-set marker continue to use normal heartbeat TTL expiry. Audit/reconcile such legacy ACTIVE records during rollout.

**Server ID uniqueness and reuse policy:** `serverId` is type-prefixed and globally unique by construction, so it cannot collide across server types. `registerServer()` checks only that type's state hash (rejecting an `ACTIVE` record or `DELETING` tombstone) and current-type registry set before publishing membership. Phase B removes the old registry membership; Phase C removes the tombstone after the configured retention period (24 hours by default). After Phase C has completed, same-type reuse is technically permitted, although workers should generate a fresh ID for every process incarnation. There is no permanent `servers:{registry}:ids` reservation set.

This is intentionally a coarse safeguard: Redis records whether the ID is still registered or retained as a tombstone, but does not identify the operating-system process using it. A delayed or restarted old process could therefore register again after Phase C if it reuses the same ID. The worker startup contract must generate a fresh type-prefixed globally unique ID for every process incarnation and must not replay registration after retirement. This policy prevents duplicate registration while the prior identity is still represented in Redis; it does not provide permanent uniqueness or process-level fencing.

**Upgrade and partial-registration behavior:** deployments that previously created `servers:{registry}:ids` may leave that set in Redis, but the new registration code ignores it; it can be deleted during a deliberate cleanup if desired. Registration checks only the current type's state hash for IDs removed from the active registry but still retained as tombstones, then performs the explicit-key registry check/insertion atomically in the `{registry}` slot. Because state hashes and registry keys occupy different Cluster slots, the overall check-and-register workflow is not atomic across slots. A crash after registry insertion can leave a stale registry member without an ACTIVE state; reconcile such records before retrying with the same ID. A crash before registry insertion does not reserve the ID.


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

The code has not been compiled or tested against live Redis or Redis Cluster. Verify the installed redis-plus-plus version supports `eval<std::vector<std::string>>`. Test lease acquisition/takeover and stale tokens, lease expiry during slow scans, first-heartbeat-before-timeout and never-heartbeated-after-timeout races, heartbeat/fencing and lifecycle races, Phase A/Phase B recovery, Phase C retention and tombstone collection, stale managers resuming before/after Phase C, the one-time registration invariant, the partial-registration-before-registry window, dashboard reads concurrent with cleanup, malformed key types, counter integrity, and actual Cluster slot routing before deployment.

## Cleanup coordination and stale managers

### Key question

What happens if a cleanup manager acquires the Redis cleanup lease, pauses long enough for that lease to expire, and then resumes after another manager has acquired the lease? Could both managers continue cleanup and delete the same server's data?

### Analysis

The cleanup lease coordinates normal scan ownership; it is not the sole correctness mechanism. It is acquired with `SET NX PX`, renewed only when the stored owner token matches, and released only by its current owner. If a manager loses the lease, its next renewal attempt fails. A paused manager can nevertheless resume and execute code before it reaches that renewal check, so lease expiry alone does not stop an old process.

Correctness is protected at the per-server level by the Redis Lua lifecycle scripts:

- **Atomic fencing:** `kFenceExpiredServerScript` checks heartbeat absence and that the server state is `ACTIVE`. If the server has published a heartbeat before, membership in the live set identifies it for normal heartbeat-expiry fencing. If it has never heartbeated, the script checks its per-server registration timestamp against the configured first-heartbeat startup timeout. It then changes the state to `DELETING` atomically. Heartbeat refresh and this transition operate in the same per-type Redis Cluster slot, so Redis serializes the competing scripts. Whichever runs first determines whether the heartbeat is refreshed or the server is fenced.
- **No reactivation or new work:** Once the state is `DELETING`, heartbeat refresh and job creation/transition operations that require an `ACTIVE` server are rejected. Registration also does not reactivate a terminal server ID.
- **Atomic, repeat-safe cleanup:** `kCleanupExpiredServerScript` rechecks the `DELETING` state and absence of the heartbeat before making changes. Its validation and deletions/counter updates execute in one Lua script. If another manager has already completed Phase A, the script recognizes the completed shape and returns the already-cleaned result rather than decrementing counters again.
- **Recoverable registry cleanup:** The manager removes the ID from the registry after Phase A reports completion. The retained `DELETING` tombstone and retired sorted-set entry make cleanup interrupted between phases discoverable.
- **Delayed tombstone collection:** Phase C confirms registry absence, then atomically verifies retention age and absence of per-server data before removing the individual tombstone. A stale manager holding an old registry snapshot cannot recreate state: fencing only transitions `ACTIVE`, and cleanup only operates on `DELETING`.

Therefore, two managers may briefly overlap after a lease expires, but their per-server operations are serialized and guarded by lifecycle state. For the operations currently implemented, a separate leader-election protocol, quorum, or cleanup fencing-token subsystem is not required. The lease reduces unnecessary concurrent scans; the per-server Lua scripts protect data correctness.

### Assumptions and limits

This safety argument depends on all lifecycle mutations and destructive cleanup remaining behind the guarded Lua scripts, on the scripts continuing to operate on the appropriate per-type Redis slot, and on workers generating a fresh ID for every process incarnation. Registration rejects IDs while their prior registry membership or tombstone remains, but after Phase C the ID is reusable and Redis cannot distinguish a delayed old process from a new one. `registerServer()` must not be replayed or resume late after retirement. Any future destructive operation performed outside those checks must be reviewed for stale-manager safety. The lease token is not passed to the per-server scripts, so it should not be treated as a hard execution fence.

This is a source-level analysis, not a demonstrated failure-injection result. Before deployment, test lease expiry while a manager is paused, takeover by a second manager, concurrent fencing/heartbeat refresh, overlapping Phase A cleanup, and recovery between Phase A and Phase B.

