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

Keep one long-lived `RedisJobStatus` object per server process and use it for registration, lifecycle calls, reads, and heartbeat publishing. The default constructor obtains the shared `RedisClient` only when enabled. When disabled, result-returning mutators return `Disabled`, void mutators do nothing, and read methods return empty collections.

Tests can inject a configuration provider:

```cpp
RedisJobStatus status([] {
  RedisJobStatus::Config config;
  config.enabled = false;
  return config;
});
```

The `RedisClient::Instance()` singleton is initialized on its first call. Its default provider reads `redisUrl` and `isRedisCluster` through the host registry. A custom provider must be supplied on that first call; later providers are ignored. Initialize host configuration before constructing `RedisJobStatus` or making the first client call.

## Worker startup

The worker uses two process-level start methods:

```cpp
auto status = std::make_shared<RedisJobStatus>();
status->registerServer(typeMetadata, serverMetadata);

RedisHeartbeatManager::Start();
RedisHeartbeatManager::HeartbeatStart(status);
```

- `RedisHeartbeatManager::Start()` starts one cleanup-scanning thread in this process. The distributed Redis lease elects the normal global registry scan owner across all worker processes.
- `RedisHeartbeatManager::HeartbeatStart(status)` starts one heartbeat-publishing thread in this process. It retains the supplied `shared_ptr<RedisJobStatus>`, publishes immediately, then calls `refreshHeartbeat(30)` every 10 seconds.
- Both calls are idempotent within a process: repeated calls do not create duplicate threads. The first `HeartbeatStart()` call supplies the status object used by that process's heartbeat thread.

The `RedisJobStatus` object must be shared with `HeartbeatStart()` and remain available to application code. Use the same object for job lifecycle operations. The background heartbeat thread catches and logs refresh errors and continues retrying on the next interval. The worker's normal event/work loop runs independently. At process shutdown, the static runner stops and joins its thread.

## TTLs and timing configuration

There are **two Redis keys with expiration (TTL) in this implementation**. They serve different purposes and must be configured independently:

| TTL | Redis key | Current value | What it controls | Where to change it |
|---|---|---:|---|---|
| Worker heartbeat TTL | `heartbeat:server:{<serverType>}:<serverId>` | 30 seconds | How long a worker's last heartbeat remains present after its most recent successful refresh. When it expires, the cleanup scanner may fence the worker. | The `refreshHeartbeat(30)` call in `HeartbeatRunner::run()` in `RedisHeartbeatManager.cc`. |
| Distributed cleanup-lease TTL | `servers:{registry}:cleanup-lease` | 30 seconds (30,000 ms) | How long the registry-scan ownership lease remains valid if its owner stops renewing it. This coordinates cleanup scanners; it is **not** a worker heartbeat. | `kLeaseTtlMs = 30000` near the top of `RedisHeartbeatManager.cc`. |

The current related intervals are **not TTLs**:

| Interval | Current value | Where to change it |
|---|---:|---|
| Worker heartbeat refresh interval | 10 seconds | The `std::chrono::seconds(10)` wait in `HeartbeatRunner::run()` in `RedisHeartbeatManager.cc`. |
| Cleanup scan interval | 5 seconds | `kCleanupScanIntervalSeconds = 5` in `RedisHeartbeatManager.h`. |
| Cleanup-lease renewal interval | 5 seconds | `kLeaseRenewInterval = std::chrono::seconds(5)` in `RedisHeartbeatManager.cc`. |

**How to change the worker heartbeat:** change both its TTL argument and its refresh wait. For example, to use a 60-second heartbeat TTL refreshed every 15 seconds, change the call to `m_status->refreshHeartbeat(60)` and the wait to `std::chrono::seconds(15)`. Keep the refresh interval comfortably shorter than the TTL; 15/60 seconds gives a 4:1 TTL-to-refresh ratio. Manual calls to `status.refreshHeartbeat(ttlSeconds)` use the TTL passed to that call.

**How to change the cleanup lease:** change `kLeaseTtlMs` in milliseconds (for example, `60000` for 60 seconds). Keep `kLeaseRenewInterval` comfortably shorter than the lease TTL; the current 5-second renewal interval is suitable for the current 30-second lease. The cleanup scan cadence is controlled separately by `kCleanupScanIntervalSeconds`.

These values are currently **compile-time settings in the C++ source**, not host registry settings or runtime configuration. After changing them, rebuild and deploy the worker. The two TTLs do not need to be equal. A heartbeat TTL controls worker-liveness detection; the cleanup-lease TTL controls scanner coordination.

**Important expiry behavior:** expiration of the worker heartbeat key does not itself change the worker's state. The cleanup scanner must run and execute the fencing script before lifecycle operations are rejected. Detection therefore also depends on the cleanup scan interval and Redis/network/scheduling delays.

## Public job-status API

```cpp
RedisJobStatus status;

status.registerServer(typeMetadata, serverMetadata);
status.refreshHeartbeat(30); // also available for explicit/manual refresh

status.createPendingJob("job-100", jobMetadata);
status.transitionJob("job-100", RedisJobStatus::State::Active);
status.transitionJob("job-100", RedisJobStatus::State::Suspended);
status.transitionJob("job-100", RedisJobStatus::State::Active);
status.completeJob("job-100");

auto types = status.getServerTypes();
auto typeCounters = status.getTypeSummary("query");
auto servers = status.getServerSummaries("query");
auto jobs = status.getServerJobs("query", "query-01");
```

The configured identity is used by registration, lifecycle operations, and heartbeats. Read APIs accept the server type and, where needed, server ID explicitly.

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
| HASH | `servers:state:{<serverType>}` | Per-server state: `ACTIVE` or terminal `DELETING` |

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

Cleanup is split across Redis Cluster slots:

1. **Phase A** runs in the per-type slot. It preflights and fences/deletes per-type job data, updates counters and live membership, and retains the DELETING tombstone.
2. **Phase B** removes the exact server ID from the registry slot.

A crash between phases is recoverable: a later scan retries Phase B. Redis Lua is atomic relative to other commands but does not roll back earlier writes after runtime errors; cleanup scripts preflight before writing.

## Dashboard read consistency

`getServerJobs()` first discovers references, then executes one Lua snapshot with the index and all referenced job hashes declared in `KEYS`. The script revalidates index membership and reads the hashes atomically. If cleanup changed the index between discovery and snapshot execution, the read returns an error rather than a partial list; callers may retry.

`getServerSummaries()` discovers IDs in the `{registry}` slot, then reads all extant summary hashes in one Lua execution in the `{serverType}` slot. Summary hashes are mutually consistent at that script's execution point. The two steps cannot form one global point-in-time snapshot across Redis Cluster slots; a summary removed after registry discovery is omitted.

## Source files

- `RedisClient.h/.cc` — process-wide Redis or Redis Cluster client.
- `RedisJobStatus.h/.cc` — public API and C++ implementation.
- `RedisJobScripts.inc` — embedded lifecycle, heartbeat, fencing, cleanup, lease, and snapshot Lua.
- `RedisHeartbeatManager.h/.cc` — process-local cleanup and heartbeat thread entry points.
- `heartbeat.cc` — worker startup integration example.

## Build and validation

Requires C++17, redis-plus-plus, hiredis, and host-provided `get_reg_val(const std::string&)` and `initializeConfiguration()`. Compile `RedisClient.cc`, `RedisJobStatus.cc`, and `RedisHeartbeatManager.cc` into the worker, with the project's Redis include/link settings. The `heartbeat.cc` file is an example main; integrate its startup sequence into the host worker rather than linking a second main.

The code has not been compiled or tested against live Redis or Redis Cluster. Verify the installed redis-plus-plus version supports `eval<std::vector<std::string>>`. Test lease acquisition/takeover and stale tokens, lease expiry during slow scans, heartbeat/fencing and lifecycle races, Phase A/Phase B recovery, dashboard reads concurrent with cleanup, malformed key types, counter integrity, and actual Cluster slot routing before deployment.
