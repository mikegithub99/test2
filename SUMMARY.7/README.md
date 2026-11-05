# Redis Job Status API

This directory contains the C++17 Redis-backed job lifecycle and worker-status implementation. The feature uses Redis hashes and sets for job records, indexes, counters, metadata, and heartbeats. Lua scripts perform the multi-key lifecycle updates atomically on the Redis node that owns the keys.

## Configuration and object lifetime

The default `RedisJobStatus` constructor reads these host-application registry values through `get_reg_val(const std::string&)`:

| Registry key | Purpose |
|---|---|
| `redisJobStatusEnabled` | Enables or disables status reporting |
| `redisJobStatusServerType` | Worker/server type used in Redis keys and job ownership |
| `redisJobStatusServerId` | Stable identity of this worker/server instance |

`redisJobStatusEnabled` accepts `true`/`false`, `1`/`0`, and `yes`/`no`, case-insensitively. An empty or unset value defaults to enabled. Configuration is read when each `RedisJobStatus` object is constructed; the object retains its configured server identity.

Separate `RedisJobStatus` objects may be used at different call sites, provided they are constructed with the same configured `serverType` and `serverId`. Call `registerServer()` to register that configured identity; it does not set or distribute identity to other objects.

When disabled, the class does not initialize or access `RedisClient`. Result-returning mutators return `RedisJobStatus::Result::Disabled`, void mutators do nothing, and read methods return empty collections.

Tests can inject a configuration provider:

```cpp
RedisJobStatus status([] {
  RedisJobStatus::Config config;
  config.enabled = false;
  return config;
});
```

When enabled, `RedisJobStatus` obtains the shared `RedisClient`. The client accepts an optional configuration provider on the first `RedisClient::Instance()` call. Its default provider reads `redisUrl` and `isRedisCluster` from the host registry. The first call initializes the singleton; later providers are ignored.

## Public API

```cpp
RedisJobStatus status;

status.registerServer(typeMetadata, serverMetadata);
status.refreshHeartbeat(30);

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

The configured identity is used by registration, job lifecycle operations, and heartbeats. The read APIs accept the server type and, where needed, server ID explicitly.

Supported job transitions are:

- `PENDING → ACTIVE`
- `ACTIVE → SUSPENDED`
- `SUSPENDED → ACTIVE`

Repeating the current state returns `NoOp`. A transition to `PENDING` is not supported. `completeJob()` removes the job and decrements its state and total counters.

The C++ result values are part of the Lua/C++ interface and must remain aligned:

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

## Metadata behavior and reserved fields

Metadata values are stored as strings in Redis hashes.

- Job metadata is supplied to `createPendingJob(jobId, metadata)` and stored on the job hash.
- Server-type metadata is supplied as the first argument to `registerServer()` and merged into the type summary.
- Server-instance metadata is supplied as the second argument to `registerServer()` and merged into that instance's summary.
- A metadata update replaces supplied matching fields and leaves unspecified metadata fields unchanged.

All summary metadata hash fields are prefixed with `meta:`. Counter-reading APIs omit fields with this prefix, so descriptive metadata is not parsed as numeric counters.

### Job metadata reserved fields

These job hash fields are owned by `RedisJobStatus`:

- `jobId`
- `status`
- `serverType`
- `serverId`

Caller metadata entries using these names are ignored when constructing the Lua job-creation arguments; the API writes the authoritative values itself. Metadata field names must not be empty.

The reserved job-field names are centralized in `RedisJobStatus.cc` and checked through one predicate. To reserve another job field from caller-supplied metadata, add its name to `kReservedJobMetadataFields`. If the field also becomes an API-owned value in the job hash, update the job creation script and the job-reading logic consistently.

### Server-type metadata reserved fields

The following system metadata fields are initialized on first creation of a type summary:

- `meta:createdAt` — UTC timestamp in ISO-8601 form.

This value is set with `HSETNX`, so later registrations do not overwrite it. Caller-supplied server-type metadata named `createdAt` is rejected. This reserved name is centralized in `RedisJobStatus.cc`.

Server-instance metadata currently has no additional reserved field names beyond the requirement that field names are non-empty.

## Redis key layout

| Redis type | Key pattern | Purpose |
|---|---|---|
| SET | `servers:{registry}:types` | Global server-type discovery |
| SET | `servers:{registry}:type:<serverType>` | Registered server IDs for one type |
| HASH | `summary:type:{<serverType>}` | Type counters and type metadata |
| HASH | `summary:server:{<serverType>}:<serverId>` | Server counters and server metadata |
| HASH | `job:{<serverType>}:<serverId-length>:<serverId>:<jobId>` | Job state, ownership, and custom fields |
| SET | `jobs:type:{<serverType>}` | Composite references for current jobs of a type |
| SET | `jobs:server:{<serverType>}:<serverId>` | Composite references for current jobs owned by a server |
| STRING with TTL | `heartbeat:server:{<serverType>}:<serverId>` | Last heartbeat value for a server |
| SET | `servers:live:{<serverType>}` | Server IDs that have successfully published a heartbeat |
| HASH | `servers:state:{<serverType>}` | Per-server lifecycle state: `ACTIVE` or terminal `DELETING` |

The job key and both job indexes use a length-prefixed composite reference derived from the configured server ID and original application job ID. This avoids ambiguity if a server ID contains separator characters. The job hash's `jobId` field retains the original application job ID for display.

## Redis Cluster hash-tag design

Redis Cluster uses the text inside the first non-empty pair of braces as a hash tag. Keys used together by a multi-key Lua script must resolve to the same hash slot.

Job lifecycle keys use `{serverType}` as their hash tag. The job hash, type and server job indexes, and type and server summaries for a given server type therefore share a slot. Different server types can be distributed across different slots.

Server registration updates two registry sets in one Lua script:

- `servers:{registry}:types`
- `servers:{registry}:type:<serverType>`

Both use the literal `{registry}` hash tag. The registry keys intentionally use a different slot from the type-partitioned lifecycle data. Type-summary metadata initialization and server-summary metadata updates are separate operations because those summary keys use the `{serverType}` hash tag rather than `{registry}`.

When extending a Lua script, pass every accessed Redis key explicitly through `KEYS` and preserve the same-slot requirement. Lua scripts execute without interleaving from other Redis commands, but they are not general rollback transactions; validate key types and state before writing.

## Lua argument conventions

The scripts are embedded as C++ raw string literals in `RedisJobScripts.inc`, which is included by `RedisJobStatus.cc`. It is not a standalone Lua file.

The C++ implementation uses `appendMetadataArgs()` to append a metadata pair count followed by the accepted field/value pairs. This keeps the C++ argument encoding consistent across job metadata, server-type metadata, and server-instance metadata. The include/exclude policy is kept separate from the argument-formatting helper.

For job creation, the six keys are the job hash, type job index, server job index, type summary, server summary, and per-type lifecycle state hash. The script rejects writes unless this server ID is `ACTIVE`. `ARGV[1..4]` contain the original job ID, server type, server ID, and composite reference. `ARGV[5]` is the number of accepted job metadata pairs; pairs begin at `ARGV[6]`.

The type-summary metadata initialization script receives the creation timestamp, metadata pair count, then the metadata pairs. It does not duplicate the server ID in type metadata. The server-summary metadata script receives the metadata pair count followed by its pairs. Keep these layouts synchronized with the corresponding C++ argument construction whenever either side changes.

## Source files

- `RedisClient.h/.cc` — shared Redis client dependency currently located in the repository's separate `.7/` directory; it is included/linked by this implementation, not mirrored into `SUMMARY.7/`.
- `RedisJobStatus.h/.cc` — public API and C++ implementation.
- `RedisJobScripts.inc` — the shared collection of embedded Lua scripts used by job lifecycle, heartbeat refresh, fencing, and cleanup. Keep all Lua in this file; the manager includes it rather than maintaining separate script bodies.
- `RedisHeartbeatManager.h/.cc` — process-wide background worker that fences expired IDs, validates and removes their per-type artifacts, then removes registry membership in a separate Redis Cluster slot.
- `heartbeat.cc` — example heartbeat runner that refreshes immediately and then periodically on a worker thread.
- `examples/` — example programs, where included.
- `redis_job_correctness.py` — Redis data correctness checker, where included.
- `redis_browser.py` — interactive Redis browser, where included.

## Build and validation

The implementation requires C++17, redis-plus-plus, hiredis, and a host-provided `get_reg_val(const std::string&)` implementation. After `registerServer()` completes, it starts one process-wide `RedisHeartbeatManager` for the configured `(serverType, serverId)`. The manager refreshes its own heartbeat every 10 seconds with a 30-second TTL and scans for expired registrations every 5 seconds. A server is eligible for fencing only after its first successful heartbeat has published it to the live set. Initialize the host configuration registry before constructing `RedisJobStatus` or making the first `RedisClient::Instance()` call. Exact compiler and linker flags depend on the host application's Redis client installation.

No build, unit test, Redis integration test, or Redis Cluster test has yet been run for the heartbeat-manager integration. Validate against the project's actual build and test setup, including standalone Redis and Redis Cluster, before deploying.


## Heartbeat cleanup manager

`RedisHeartbeatManager.h/.cc` implements the process-wide background cleanup worker
described in `HEARTBEAT_DESIGN.md`. It is started automatically, once per
process, after `registerServer()` has completed successfully. The host
configuration registry and Redis client settings must be initialized first.
The manager refreshes the current process heartbeat and scans registered IDs; its per-type fence only considers an
`ACTIVE` ID after its first heartbeat has published live-set membership. It
fences expired IDs in their per-type Redis Cluster slot, removes their jobs and
per-server summary in Phase A, then removes the ID from the registry in Phase B. The terminal `DELETING` state is
retained to prevent accidental reactivation of a process ID and to make a
crash between phases recoverable. Since server IDs are guaranteed unique and
never reused, a replacement process registers under a different ID.

Compile `RedisHeartbeatManager.cc` alongside `RedisJobStatus.cc` and
`.7/RedisClient.cc` (with `.7/` on the include path), with the same C++17, redis-plus-plus, hiredis, and host
configuration dependencies. No build or Redis integration test has been run
for this implementation yet.
