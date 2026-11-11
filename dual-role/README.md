# Dual-Role Worker and Cleanup Manager

## Architecture decision: independent per-type leadership (Option 2)

Each application process remains a worker. It may also be eligible to manage cleanup, but manager leadership never gates worker registration, job operations, or heartbeat publication.

There is **one active cleanup manager per server type**. Different processes may manage different types at the same time; one process may manage more than one type. Other processes are standby candidates for a type and do not scan that type unless they acquire its authority.

Example: process A manages `IMAGE`, process B manages `VIDEO`, and process C manages `AUDIO`. A process that loses `IMAGE` authority can remain the manager for `VIDEO` and continue all worker duties.

## Why per-type elections

The existing Redis Cluster layout intentionally places registry data in `{registry}` and all lifecycle/job data for one type in `{serverType}`. A global lease in `{registry}` cannot be atomically checked by a Lua script operating on a type slot. Checking a global lease in C++ before a type mutation leaves a pause/resume race.

Option 2 avoids that cross-slot authority dependency. Each type's manager lease and durable generation are in that type's existing slot. Election and protected cleanup mutations can therefore be made atomic within that slot. No new cross-slot Lua operation is introduced.

## Identities and invariants

- Worker identity is the configured `serverId`. It is globally unique across process incarnations, includes the type prefix, and must never be reused for another incarnation.
- Manager owner identity is a process-incarnation identifier. It is not the worker lifecycle state.
- Manager generation is a Redis-generated, monotonically increasing integer per server type.
- Worker lifecycle fencing (`ACTIVE` / `DELETING`) remains unchanged and continues to protect job create/transition/complete.
- Manager fencing is additive: every manager-owned per-type cleanup mutation must validate the current owner/generation and a live lease **inside the same Lua script before its first write**.
- The generation key must not expire or be reset during normal operation.
- A lease renewal must require the exact current token and a positive PTTL. It must never recreate an expired lease.
- Different types elect independently. There is no global cleanup primary.

## Key layout

All new keys use the existing type hash tag:

| Key | Purpose |
|---|---|
| `servers:cleanup-manager:{<serverType>}:lease` | Expiring owner/generation token |
| `servers:cleanup-manager:{<serverType>}:generation` | Durable monotonic generation |

These keys share a slot with `servers:state:{<serverType>}`, job indexes, summaries, heartbeat keys, and tombstones.

## Election protocol

1. A candidate discovers types from `servers:{registry}:types` (read-only cross-slot discovery).
2. For each type, an acquire Lua script checks the lease key. If it is still present, the candidate remains standby.
3. If no lease exists, the script increments the durable generation and writes a unique owner/generation token with PX TTL in one atomic type-slot operation.
4. Renewal succeeds only when the lease value exactly matches the candidate token and PTTL is positive; it then extends the TTL.
5. Release deletes the lease only when the token still matches.
6. A process tracks leadership independently for each type. Loss of one type's lease does not change its worker heartbeat or leadership for other types.

An ambiguous acquire/renew result is not treated as success. The candidate must re-read/verify the exact token and generation before doing work.

## Cleanup mutation requirements

The election primitives alone do not fence a cleanup mutation. Every manager-owned script must receive the type lease key in KEYS and the expected token in ARGV, and begin with the equivalent of:

```lua
if redis.call('GET', managerLeaseKey) ~= expectedToken or
   redis.call('PTTL', managerLeaseKey) <= 0 then
  return RESULT_STALE_MANAGER
end
```

This check must precede **all writes**. Apply it to:
- expiration fencing and tombstone creation;
- server job/index/counter/heartbeat/live-set cleanup;
- tombstone backfill;
- tombstone collection; and
- future manager-driven repair operations.

Keep every key in a given script in the same `{serverType}` slot. Do not pass `{registry}` keys to these scripts.

### Cross-slot registry housekeeping

Registry enumeration is read-only. Registry `SREM` after a type-local cleanup result remains a separate, idempotent cross-slot housekeeping operation; it cannot be atomically fenced by a type-slot script. This is safe only under the existing never-reused globally unique server-ID invariant: removing a retired ID cannot remove a later incarnation's membership. Do not treat registry SREM as the authority for cleanup correctness.

## Worker and manager lifecycle separation

- Worker heartbeat publication uses the existing `HeartbeatRunner` and `RedisJobStatus::refreshHeartbeat` path.
- Manager election is a separate component.
- A standby continues worker job processing and heartbeat publication.
- A manager may scan only types for which it currently holds authority.
- A manager must stop scheduling work for a type when renewal is lost or ambiguous.
- The worker `ACTIVE` / `DELETING` lifecycle fence is never replaced by manager fencing.

## Rebalancing

Per-type leadership changes are independent. To rebalance, stop or drain the current owner for the selected type and allow its lease to expire or compare-release it. A standby then acquires that type, receives a higher generation, and begins scanning only after acquisition succeeds. Other types remain unaffected.

Avoid forced reassignment by deleting a live lease or resetting a generation. Candidate preference/priority can influence which process is expected to acquire after release/expiry, but priority is not authority; the Redis type-slot acquire script is authoritative. If a type has no eligible candidate, cleanup for that type pauses while worker processing continues.

For planned moves, use a drain sequence: stop scheduling new scans for the selected type, finish or abort at a safe checkpoint, compare-release the token, then allow the destination candidate to acquire. For unplanned loss, wait for TTL expiry. Do not assume that a C++ leadership check protects a later Lua mutation.

### Candidate selection and rebalancing policy

The default candidate policy is for all eligible candidates for a type to contend using the atomic type-slot acquire operation; Redis determines the winner. Optional priority tiers may prefer selected candidates, but an unavailable preferred candidate must not block fallback candidates indefinitely. Never implement acquisition as a separate “check absent, then set” sequence.

Rebalancing is performed per type. For a planned move, the current owner stops scheduling scans for that type, drains or stops at a safe checkpoint, and compare-releases its exact lease token. The destination candidate then acquires the lease and receives the next generation. For an unplanned loss, candidates wait for TTL expiry. Do not delete a live lease or reset a generation to force a move. Other type leaders are unaffected.

A type with no eligible manager pauses cleanup for that type; worker processing and heartbeats continue. Candidate priority is preference only—the Redis acquisition script remains the authority.

## Operational settings

The implementation defaults should be selected from measured Redis latency and process-pause behavior. Configure:
- manager lease TTL (milliseconds);
- renewal interval (milliseconds), comfortably shorter than TTL;
- type discovery / standby retry interval;
- cleanup scan interval;
- first-heartbeat grace timeout; and
- retired tombstone retention.

The lease renewal mechanism must run independently of a potentially slow scan. A long scan must not starve renewal. Even with independent renewal, the cleanup Lua script must validate the lease token and PTTL atomically.

## Observability

Record per type:
- owner identity and generation;
- acquiring / primary / standby / lost / stopping state;
- renewal success, failure, latency, and last confirmed renewal;
- scan duration and number of servers checked/fenced/cleaned;
- tombstones backfilled/collected;
- stale-manager script rejections; and
- Redis errors and takeover duration.

Do not expose raw lease tokens or credentials in metrics.

## Failure behavior

| Event | Expected behavior |
|---|---|
| Manager process crashes | Lease expires; a standby can acquire that type. |
| Manager pauses beyond TTL | Its old token is rejected by fenced scripts; a standby can acquire a higher generation. |
| Renewal response is ambiguous | Candidate stops initiating cleanup until exact authority is verified. |
| One type's lease is lost | Stop cleanup for that type only; other type leadership and worker activity continue. |
| Redis is unavailable | No candidate assumes leadership; workers retain existing Redis error/no-op semantics. |
| New type is registered | Candidates discover it and campaign independently. |
| Redis restore rolls back generation | Treat as an operational fencing recovery event; stop all old managers before reinitializing generations. |

## Validation requirements

Before production rollout, test:
- simultaneous acquisition by many candidates for one type;
- independent leaders for two distinct types/slots;
- crash and takeover;
- pause beyond TTL followed by old-manager resume;
- stale token rejection by every cleanup mutation script with **zero writes**;
- renewal never resurrecting an expired lease;
- generation monotonicity;
- tombstone backfill and collection under takeover;
- existing worker lifecycle/job-counter regression tests; and
- Redis Cluster slot validation for every multi-key script.

A source-level review is not a substitute for Redis integration tests. Do not declare the architecture production-validated until those tests pass.

## Implementation status

This directory contains the copied worker/job-status foundation, a worker-only heartbeat wrapper, per-type lease acquisition/renewal/release, and a callable single-type cleanup pass. The expiration-fence, server-cleanup, tombstone-backfill, and tombstone-collection scripts now validate the type-local lease token and positive PTTL before their first write. The scanner renews at checkpoints and compare-releases on scan failure. It is not yet a complete background service: automatic per-type discovery/campaigning, a dedicated independent renewal/control loop, scheduled scan orchestration, application startup/shutdown wiring, and production integration tests remain outstanding. Do not enable this scaffold as a production cleanup manager until those pieces are completed and validated.

## Source layout

- `WorkerHeartbeatPublisher.h/.cc`: worker-only heartbeat runner wrapper.
- `CleanupManagerScripts.inc`: per-type lease acquire/renew/release and mutation-guard Lua primitives.
- `CleanupManager.h/.cc`: per-type lease controller and callable `scanType()` cleanup pass; no autonomous manager loop yet.
- `RedisClient.h/.cc`, `RedisJobStatus.h/.cc`, `RedisJobScripts.inc`, and `HeartbeatRunner.h/.cc`: copied baseline worker/job-status implementation.
- `browser/` and `util/`: copied dashboard and command-line utilities.

The existing `3-colocated/` baseline is intentionally unchanged. This directory is the separate implementation track.
