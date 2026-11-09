# Colocated Heartbeat and Cleanup Design

## Ownership and startup

Each worker process uses the process-wide `RedisJobStatus::Instance()` and `RedisHeartbeatManager::Instance()` singletons. The status singleton reads the worker identity from host configuration on its first call. The manager singleton starts one cleanup-scanning thread per process; `HeartbeatStart()` starts the process heartbeat publisher and obtains the status singleton itself.

The supported startup sequence is:

```cpp
initializeConfiguration();

const auto status = RedisJobStatus::Instance();
status->registerServer(typeMetadata, serverMetadata);

const auto manager = RedisHeartbeatManager::Instance();
manager->HeartbeatStart();

return runWorkerApplication();
```

Use that same status singleton for all job lifecycle operations. The constructors are private; direct stack construction and `std::make_shared<RedisJobStatus>()` are not supported. The singleton's configuration provider is honored only on its first call.

## Timing

Defaults are declared in `RedisHeartbeatManager::Config` in `RedisHeartbeatManager.h`:

- Heartbeat TTL: 30 seconds.
- Heartbeat refresh interval: 10 seconds.
- Cleanup lease TTL: 30,000 milliseconds.
- Cleanup lease renewal interval: 5 seconds.
- Cleanup scan interval: 5 seconds.
- First-heartbeat startup grace period: 60 seconds.
- Retired tombstone retention: 24 hours.

Override these values by supplying a configuration provider on the first `RedisHeartbeatManager::Instance(provider)` call. They are not host registry settings. The heartbeat publisher is implemented in `HeartbeatRunner.cc`; cleanup scanning is implemented in `RedisHeartbeatManager.cc`.

## Distributed lease

The lease key is `servers:{registry}:cleanup-lease`. Each manager creates a unique token and acquires atomically with SET NX PX semantics. Only the owner scans the registry. Renewal and release are Lua scripts that compare the stored token before changing the key. A process crash is recovered by TTL expiry.

The lease is a scan-load optimization, not a correctness lock. A long Redis operation may outlive the lease and allow overlapping scans. Per-server atomic fencing and idempotent cleanup must remain safe in that case.

## Ownership and Redis Cluster boundaries

Registry keys and the lease use the `{registry}` hash tag. Heartbeats, lifecycle state, job indexes/hashes, and summaries use the `{serverType}` tag. Never combine these groups in one Lua script. Phase B registry removal is separate.

## Three-phase cleanup protocol

Heartbeat refresh verifies `ACTIVE`, writes the heartbeat TTL, and adds live-set membership atomically. Live-set membership is the durable indication that the server has published at least one heartbeat. Fencing verifies heartbeat absence and `ACTIVE` state. If the server has heartbeated before, normal heartbeat TTL expiry makes it eligible. If it has never heartbeated, fencing waits for the registration timestamp plus `firstHeartbeatTimeoutSeconds` (60 seconds by default). The timestamp and state are in the per-type state hash, and the eligibility check plus `ACTIVE → DELETING` transition execute atomically in the same per-type slot as heartbeat refresh. A first heartbeat racing the timeout therefore either wins before fencing or is rejected after fencing. Job lifecycle scripts reject writes after fencing.

- **Phase A — fence and clean (per-type slot):** registration records `registeredAt:<serverId>` in `servers:state:{<serverType>}` using Redis TIME. The fence script checks heartbeat absence and ACTIVE state. Previously heartbeating servers are eligible after heartbeat TTL expiry; never-heartbeated servers become eligible only after the configured first-heartbeat startup grace period. Fencing atomically changes `ACTIVE` to `DELETING` and records the retirement timestamp in `servers:retired:{<serverType>}` using Redis TIME. The cleanup script preflights job ownership, statuses, indexes, key types, and counters; deletes the candidate's jobs and per-server summary; updates type counters; removes heartbeat/live membership; and retains the tombstone.
- **Phase B — remove registry membership (registry slot):** after Phase A is confirmed, remove only the exact ID from `servers:{registry}:type:<serverType>`. A crash between A and B is retried by a later scan.
- **Phase C — collect old tombstones (per-type slot):** discover old IDs from the retired sorted set (not the registry), confirm registry absence in the registry slot, then run Lua that checks the tombstone, Redis-time retention age, and absence of heartbeat, server-job index, server summary, and live membership. It removes only that server's hash field and sorted-set entry. Default retention is 24 hours, configured by `RedisHeartbeatManager::Config::retiredTombstoneRetentionSeconds`.

The timestamp is recorded atomically with fencing. Redis hashes have no per-field TTL, so Phase C performs HDEL on one server ID rather than expiring the whole state hash. The legacy tombstone migration incrementally HSCANs state hashes and backfills missing sorted-set entries with `ZADD NX`; those legacy entries receive a fresh full retention period.

## Registration invariant and startup failure limitation

Each server ID is globally unique, never reused, and `registerServer()` is called exactly once. No delayed or retried startup registration may resume after retirement. The first heartbeat remains mandatory for liveness; the registration grace period is only a deadline for an incomplete startup, not a substitute heartbeat.

If registration completes and the worker never sends its first heartbeat, the cleanup scanner fences it after `firstHeartbeatTimeoutSeconds` (60 seconds by default). If the first heartbeat arrives before that deadline, the live-set marker is established and the normal heartbeat TTL governs subsequent liveness. Both the heartbeat refresh and fence check run in the per-type slot, so the race is serialized.

The Phase C registry check and per-type collection cannot be one atomic operation because they use different Cluster slots. Safety relies on the unique, never-reused ID and once-only registration contract. Phase C also removes the per-server `registeredAt:<serverId>` field with the state tombstone.

**Upgrade note:** ACTIVE records created before `registeredAt:<serverId>` was added may lack a registration timestamp. The timeout fence deliberately leaves such never-heartbeated records untouched rather than inventing an age. Heartbeated legacy records still expire through the live marker and heartbeat TTL. Audit/reconcile legacy ACTIVE records without a timestamp during rollout.

**Known partial-registration window:** `registerServer()` currently activates per-type state before adding the ID to the global registry. If the process crashes after activation but before registry insertion, the ACTIVE state is not visible to the current registry-driven scanner. The new first-heartbeat timeout handles a registered server that never heartbeats; it does not discover this earlier partial-registration window. This remaining case requires per-type state scanning or an explicit staged registration/reconciliation protocol. It cannot be solved by a Lua script spanning both Cluster slots. Do not claim that all partial registration crashes are automatically recovered.


## Lease expiry and stale managers

A paused manager can resume after its lease expires, even after another manager takes over. The lease token prevents stale renewal or release, but is not passed to per-server scripts and is not a hard execution fence. Correctness is maintained by per-server Lua state checks:

- Only one manager can win the atomic `ACTIVE → DELETING` transition.
- Lifecycle mutations require `ACTIVE`; after fencing they are rejected.
- Phase A cleanup rechecks `DELETING` and is repeat-safe.
- Phase B removes only the exact registry member.
- Phase C requires the tombstone, elapsed retention, registry absence, and absent per-server artifacts.

These guarantees depend on all lifecycle mutations and destructive cleanup remaining behind the guarded Lua scripts and on server IDs never being reused.

## Read consistency

`getServerJobs()` discovers references, then reads and revalidates the index and job hashes in one Lua snapshot. If cleanup changes the index between discovery and snapshot execution, the read fails and should be retried.

`getServerSummaries()` discovers IDs in the registry slot, then reads extant summary hashes in the per-type slot. Those steps cannot form one global point-in-time snapshot across Redis Cluster slots; summaries removed between discovery and retrieval are omitted.

## Failure behavior

- Lease owner crashes: TTL expires and another worker can acquire the lease.
- Stale owner renews/releases: token comparison rejects the operation.
- Scan outlives lease: scans may overlap; per-server Lua fencing and idempotent cleanup protect data.
- Two managers find the same expired ID: only one wins fencing; cleanup is repeat-safe.
- Crash after Phase A: a later scan can complete Phase B.
- Crash after Phase B: the retired sorted set keeps the tombstone discoverable for Phase C.
- Crash during registration or before first heartbeat: partial-startup records may require operator reconciliation, as described above.
- Stale manager resumes after Phase C: its old fence sees no ACTIVE state and its cleanup sees no DELETING state; neither recreates the server state.

## Validation

This is a source-level design description. The code has not been compiled or tested against live Redis or Redis Cluster. Validate concurrent lease acquisition, crash/takeover, stale-token operations, slow scans and overlap, heartbeat/fencing races, Phase A/Phase B recovery, tombstone retention and collection, partial registration failures, counter integrity, atomic reads during cleanup, malformed key types, and actual Cluster slot behavior before deployment.
