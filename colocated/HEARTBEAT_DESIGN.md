# Colocated Heartbeat and Cleanup Design

Every worker process runs its own heartbeat publisher and starts one in-process `RedisHeartbeatManager`. The process-local singleton ensures one cleanup thread per process. No separate cleanup executable is required.

## Startup and timing
1. Initialize host configuration and Redis connection settings.
2. Construct one long-lived `RedisJobStatus`.
3. Call `registerServer()`.
4. Call `RedisHeartbeatManager::Start()`.
5. Publish heartbeat immediately, then every 10 seconds with a 30-second TTL.

The cleanup manager attempts a scan every five seconds. A worker that cannot acquire the lease skips registry discovery. The heartbeat loop has no explicit stop condition or signal handling and runs for the process lifetime. Application work runs on other threads.

## Distributed lease
The lease key is `servers:{registry}:cleanup-lease`. Each manager creates a unique token and acquires atomically with `SET key token NX PX 30000` semantics. Only the owner scans the registry. The manager checks/renews the lease at work boundaries about every five seconds. Renewal and release are Lua scripts that compare the stored token before changing the key. A stale owner cannot renew or delete a successor's lease. A process crash is recovered by TTL expiry.

The lease is a scan-load optimization, not a correctness lock. A long Redis operation may outlive the lease and allow overlapping scans. Per-server atomic fencing and idempotent cleanup must remain safe in that case.

## Ownership
`RedisJobStatus` retains configured enabled state, server type, server ID, and the shared Redis client pointer. It is not a singleton and owns no scheduling. `RedisClient::Instance()` remains the process-wide shared client. The worker integration owns heartbeat scheduling; the manager owns cleanup scheduling.

## Redis Cluster boundaries
| Key | Slot / purpose |
|---|---|
| `servers:{registry}:types` | Registry discovery |
| `servers:{registry}:type:<serverType>` | Registered IDs |
| `servers:{registry}:cleanup-lease` | Scan lease |
| `heartbeat:server:{<serverType>}:<serverId>` | Per-type heartbeat |
| `servers:live:{<serverType>}` | Per-type live set |
| `servers:state:{<serverType>}` | Per-type lifecycle state |
| Job/index/summary keys with `{serverType}` | Per-type lifecycle data |

Lease and registry operations use only the `{registry}` slot. Heartbeat refresh, fencing, and Phase A cleanup use only the `{serverType}` slot. Phase B registry removal is a separate registry-slot command. Never combine these slots in one Lua script.

## Cleanup protocol
Heartbeat refresh verifies `ACTIVE`, writes the heartbeat TTL, and adds live-set membership atomically. Fencing verifies heartbeat absence, `ACTIVE` state, and prior live-set membership, then changes the candidate to terminal `DELETING`. Job lifecycle scripts reject writes after fencing. Redis serializes refresh and fencing in the per-type slot.

Phase A operates only in the per-type slot. It preflights key types, job ownership, statuses, indexes, and counters before deleting the candidate's jobs/artifacts and adjusting type counters. It removes live membership and retains the `DELETING` tombstone. The operation is idempotent.

Phase B removes only the exact ID from `servers:{registry}:type:<serverType>` after Phase A is confirmed. If a manager crashes between phases, a later scan recognizes completed Phase A and retries Phase B.

## Failure behavior
| Scenario | Behavior |
|---|---|
| Multiple workers acquire | One lease owner; others skip scan. |
| Lease owner crashes | TTL expires; another worker takes over. |
| Stale owner renews/releases | Token check rejects it. |
| Scan outlives lease | Scans may overlap; per-server Lua arbitrates cleanup. |
| Two managers find same expired ID | Only one wins ACTIVE-to-DELETING fencing; cleanup is idempotent. |
| Heartbeat races with fencing | Redis ordering renews first or rejects refresh after fencing. |
| Crash after Phase A | Later scan retries Phase B. |

## Validation
Starting values: 30-second lease TTL, five-second lease renewal checks, five-second scan interval, 10-second heartbeat refresh, and 30-second heartbeat TTL. Tune using measured registry size, scan duration, and Redis latency. Monitor lease acquisition/renewal, scan duration, cleanup errors, and expired candidate age.

Not yet compiled or tested against live Redis or Redis Cluster. Validate concurrent lease acquisition, crash/takeover, stale-token operations, slow scans and overlap, heartbeat/fencing races, Phase A/Phase B recovery, counter integrity, and cluster slot behavior before deployment.
