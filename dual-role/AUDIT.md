# Audit: `3-colocated` vs `dual-role`

**Audit date:** 2026-11-11  
**Repository:** `mikegithub99/test2`  
**Baseline audited:** `main` at `efa9c9acc5d90d535641365d2b3d1eefd9260d6d`  
**Follow-up revision:** authority-map race fix in the source revision accompanying this audit  
**Scope:** Source-level comparison of worker lifecycle, heartbeat publication, cleanup-manager election, cleanup Lua, Redis Cluster key placement, configuration, and thread lifecycle.

## Executive assessment

**Dual-role remains the preferred target architecture, but is not production-validated.** Its independent per-server-type manager lease and token/positive-TTL checks inside cleanup Lua provide real stale-manager fencing while respecting Redis Cluster slot boundaries. 3-colocated has a simpler cleanup loop, but its single global lease is best-effort scan coordination: the per-type cleanup scripts do not validate that global lease, so an old scanner that resumes after lease loss is not strictly rejected by manager authority.

The prior high-priority dual-role defect—unconditional removal of a type's local authority after a delayed renew/release—has been fixed in the accompanying source change. Local authority changes now compare the exact token, acquisition results are ordered by generation, and scan failure releases only the token used by that scan.

**Recommendation:** continue with dual-role as the target, retain 3-colocated as the worker-lifecycle/cleanup regression baseline, and do not call either implementation production-ready until the validation work at the end of this document is completed.

## Findings

### [FIXED IN SOURCE] DUAL-1 — Concurrent renew/release could erase a newer local authority

**Original files:** `dual-role/CleanupManager.cc`, `dual-role/CleanupManager.h`.

Previously, `renew()` erased `m_authorities[type]` unconditionally after a failed renewal; `release()` erased it unconditionally after Redis I/O; the renewal-loop exception handler also erased by type. A delayed operation using token A could therefore erase token B installed by a later acquisition. The scan error handler called `release(type)`, which could read and release B even though the failing scan had been using A.

**Fix applied:**
- Added `eraseAuthorityIfMatches(type, token)`; it erases only if the map still contains that exact token.
- Added `releaseToken(type, token)`; scan failure releases only the token captured at scan start.
- `renew()` uses compare-and-erase on a non-success result.
- `release()` snapshots its token and delegates to token-specific release; completion cannot erase a newer map entry.
- The renewal loop snapshots (type, token) pairs and conditions exception cleanup on the snapshotted token.
- `acquire()` installs a returned authority only if no local authority exists or its generation is lower. A delayed lower-generation acquire response cannot overwrite a newer locally installed generation.

Redis Lua release already compares the exact token before deleting the lease, so the token-specific C++ release preserves the Redis-side compare-release guarantee as well.

**Status:** source fix applied; not compiler- or concurrency-test-verified. Required tests: delayed acquire response after a higher generation is installed; renew-vs-reacquire; release-vs-reacquire; scan failure while a newer token is installed; renewal exception while a newer token is installed; and ambiguous Redis replies.

### [MEDIUM] DUAL-2 — Redis-disabled startup still evaluates and validates manager/heartbeat configuration

**Files:** `dual-role/CleanupManager.cc`, `dual-role/WorkerHeartbeatPublisher.cc`, `dual-role/heartbeat.cc`.

Both singleton constructors evaluate their ConfigProvider and validate Redis-specific timing values before checking whether Redis job status is enabled. The example obtains both singleton instances even if Redis status reporting is disabled. In contrast, 3-colocated's `RedisHeartbeatManager` checks `RedisJobStatus::isEnabled()` before loading/validating manager timing configuration and returns without starting cleanup.

A malformed or unavailable Redis-only setting/provider can therefore fail an otherwise Redis-disabled worker startup.

**Recommendation:** check enabled status before evaluating Redis-only providers and validating Redis-only settings, or explicitly establish that the application always requires these settings even when Redis status is disabled. Test with Redis disabled and a provider that throws.

### [MEDIUM] DUAL-3 — Public one-shot scan/acquire APIs can leave an unrenewed lease

**File:** `dual-role/CleanupManager.h/.cc`.

`acquire()`, `renew()`, `scanType()`, and `release()` are public. A caller can invoke `scanType(type)` without starting the background renewal loop. A successful scan leaves the lease in Redis, and `stop()` returns immediately if `m_running` was never set, so it does not release authority acquired through this path. The lease eventually expires, so the primary effect is delayed takeover rather than stale-manager mutation.

**Recommendation:** make low-level methods private if they are only internal lifecycle operations, or define a separate one-shot scan operation that acquires, scans, and compare-releases in a single managed lifecycle. Ensure stop handles held authority even if background loops were never started.

### [MEDIUM] DUAL-4 — Example does not yet demonstrate the host application's typed ConfigProvider

**Files:** `dual-role/heartbeat.cc`, `dual-role/CleanupManager.h/.cc`, `dual-role/WorkerHeartbeatPublisher.h/.cc`, and `RedisJobStatus` initialization.

The services accept `std::function<Config()>`, which is a valid injection seam. The example callbacks nevertheless read string values using `get_reg_val()` and parse them with `std::stoll`; its comments ask the integrator to replace this with the application's typed ConfigProvider. The example also obtains `RedisJobStatus::Instance()` without showing an injected provider for that singleton. Because all these services are singletons, the provider supplied on the first `Instance()` call wins.

**Recommendation:** adapt the actual host ConfigProvider into all relevant singleton initializations, or document and test the callback adapter as the supported integration contract. Verify first-call initialization order.

### [LOW] DUAL-5 — Manager loop acquires each type twice per scan cycle

**File:** `dual-role/CleanupManager.cc`.

`managerLoop()` calls `acquire(type)`, then `scanType(type)`, which calls `acquire(type)` again. This adds a Redis renewal and another interleaving point before each scan. It is not itself a fencing violation.

**Recommendation:** remove the redundant acquire or introduce a private scan method that accepts an already captured authority token.

## Architecture and Lua comparison

### 1. Manager election and stale-manager fencing

| Area | 3-colocated | dual-role |
|---|---|---|
| Election scope | One global lease in `{registry}` | Independent lease per `{serverType}` |
| Ownership | One global scanner | One manager per type; a process may own several types |
| Generation | Random owner token; no durable monotonic generation | Persistent Redis-incremented generation per type |
| Renewal | Scanner renews at scan checkpoints | Independent renewal loop plus scan checkpoints |
| Mutation authorization | Global lease is not checked by per-type cleanup Lua | Protected cleanup Lua checks exact type-local token and positive PTTL |
| Stale manager after lease loss | Not strictly fenced | Rejected by protected scripts after takeover/expiry |
| Cluster compatibility | Global lease cannot be checked atomically in type slot | Lease and protected data share the type slot |

In 3-colocated, the global lease reduces duplicate scanning but is not a hard lock. If a manager pauses beyond its lease and later resumes, per-server lifecycle fencing and repeat-safe cleanup protect worker/job state, but the old manager is not rejected solely because its global manager lease expired.

Dual-role's lease and generation keys use the same `{serverType}` hash tag as lifecycle and cleanup data. Its protected Lua scripts validate the token and positive PTTL before their first write. This is the stronger and more Cluster-compatible design.

### 2. Redis Cluster and Lua review

No cross-slot Lua access was identified in the reviewed cleanup paths.

- Worker lifecycle/job scripts use job hashes, indexes, summaries, heartbeat, and state keys tagged with the same `{serverType}`.
- Dual-role manager lease and generation keys also use `{serverType}`; protected cleanup scripts include the lease in the same slot.
- Registry type discovery and per-type server membership use `{registry}` keys outside the type-local scripts.
- Registry `SREM` after type-local cleanup and the registry membership read before tombstone collection are separate operations. They are intentionally not folded into type-local Lua.

The cross-slot registry housekeeping is not atomically manager-fenced. It relies on idempotence and the application invariant that server IDs are globally unique, type-prefixed, and never reused across process incarnations. Preserve this invariant.

**Dual-role protected mutation scripts reviewed:**
- `kFenceExpiredServerScript`
- `kCleanupExpiredServerScript`
- `kIndexUntrackedTombstonesScript`
- `kCollectRetiredServerTombstoneScript`

Each checks the manager token and positive PTTL before its first write. `kListExpiredServerTombstonesScript` only reads candidates; final collection is fenced.

Redis Lua scripts are atomic with respect to interleaving, but they do not provide general rollback on runtime errors. The scripts preflight key types, ownership, index membership, statuses, and counters before mutation, which is good. Fault injection is still needed to test failures and corrupted data.

### 3. Worker lifecycle and heartbeat

The worker/job lifecycle source in dual-role is copied from 3-colocated. Both retain the `ACTIVE` / `DELETING` lifecycle fence. Job create, transition, and completion require ACTIVE; heartbeat refresh checks ACTIVE; expiration fencing transitions an expired worker to DELETING; cleanup validates job ownership/index membership/status/counters before deleting and decrementing.

Dual-role separates `WorkerHeartbeatPublisher` from `CleanupManager`. Standby processes can continue worker heartbeat publication and job processing without holding cleanup authority. The heartbeat runner starts its thread on construction and joins on destruction.

A killed process does not need orderly shutdown for crash recovery: its heartbeat and manager lease TTLs expire in Redis. Graceful stop is an optimization for prompt release, not the crash-recovery mechanism. Actual takeover/cleanup delay depends on TTLs, scan cadence, Redis availability, and processing time.

### 4. Registration and cross-slot boundaries

Both implementations keep registry operations in `{registry}` and lifecycle operations in `{serverType}`. Registration checks per-type state, performs registry insertion separately, initializes metadata, then activates the server in the type slot. A process crash between these separate operations can leave a partial/stale registry membership. The scanner ignores entries without an ACTIVE state, so such entries may remain as registry clutter. This is inherited from the shared baseline, not introduced by the dual-role manager.

The server-ID invariant is essential: globally unique, type-prefixed, never reused for another process incarnation. Tombstone retention is not a permanent identity reservation.

## Overall comparison

| Criterion | 3-colocated | dual-role |
|---|---|---|
| Worker lifecycle fencing | Good source-level design | Same baseline |
| Cleanup idempotence/counters | Preflight and repeat-safe cleanup | Same baseline plus manager fencing |
| Stale cleanup-manager rejection | Best-effort global lease only | Strong per-type Lua fencing |
| Redis Cluster layout | Deliberate registry/type split | Preserves split and colocates authority with type data |
| Manager topology | One global scan leader | Independent leader per type |
| Worker/manager separation | Combined manager owns heartbeat and cleanup | Separate worker heartbeat and cleanup manager |
| Operational complexity | Lower | Higher |
| Local authority concurrency | Simpler | Fixed compare-token/generation handling; still requires tests |
| Validation evidence | No completed Redis/Cluster validation established | No completed build/unit/Redis/Cluster validation established |

## Final assessment

**Architecture choice: dual-role.** It provides the correct authority boundary for Redis Cluster: a manager's authority is checked atomically in the same slot as each protected cleanup mutation. It also supports independent leadership by server type and decouples worker heartbeat from cleanup-manager leadership.

**Implementation readiness: neither is production-validated.** The high-priority local-authority race has been fixed in source, but that fix needs deterministic concurrency tests. Dual-role still has the disabled-Redis configuration-order issue, public one-shot lease-lifecycle hazard, and incomplete host ConfigProvider example. 3-colocated remains a useful, simpler baseline, but its global lease must not be represented as strict stale-manager fencing.

Recommended path: retain 3-colocated as the worker-lifecycle and cleanup regression baseline; continue dual-role after addressing DUAL-2 through DUAL-4; then run C++ builds, unit tests, standalone Redis tests, and Redis Cluster tests.

## Validation limitations

This is a source-level audit. No C++ build, compiler diagnostics, unit tests, live Redis tests, Redis Cluster slot tests, load tests, or fault-injection tests were run for this follow-up.

Before production rollout, test:
- concurrent acquisitions for one type and independent acquisitions for different types;
- delayed acquire responses and out-of-order generations;
- renew/release/reacquire/scan-failure races;
- manager crash, lease expiry, takeover, and old-manager resume;
- stale-token rejection by every protected mutation script with zero writes;
- ambiguous Redis command outcomes;
- generation monotonicity and Redis restore/rollback procedure;
- tombstone backfill/collection during takeover;
- lifecycle and counter regression behavior; and
- actual Redis Cluster slot placement for every multi-key script.

The source-level statement that no cross-slot Lua access was identified is not runtime Cluster certification.
