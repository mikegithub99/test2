# Latest Independent Findings — `3-colocated`

**Audit date:** 2026-11-10  
**Audited branch:** `main`  
**Scope:** All files under `3-colocated`, including the C++ Redis client/status/heartbeat implementation, embedded Lua scripts, browser/admin utility, command-line utilities, and design documentation.

This is an independent source review. It is not a build, runtime, Redis, or Redis Cluster test. Findings below distinguish concrete defects from assumptions and known operational limitations.

## Executive summary

The overall design has a clear separation between the global `{registry}` slot and per-type `{serverType}` slots, and the registration Lua script declares its accessed keys explicitly. The audit identified correctness and recovery gaps that should be addressed before treating the implementation as production-ready.

| Priority | Finding |
|---|---|
| High | Whole-type browser deletion races with concurrent worker registration |
| Fixed | Lua membership validation used incorrect truthiness checks (F-02) |
| Medium | Type-prefixed server-ID uniqueness is assumed but not enforced at the C++ boundary |
| Medium | Registration can leave an undiscoverable, permanently stale registry entry after a partial failure |
| Fixed | Redis reset utility omitted per-type live-server sets (F-05; fixed in this update) |
| Low | Browser whole-type deletion does not discover orphan heartbeat keys |
| Informational | Legacy ACTIVE records without registration timestamps are intentionally not auto-expired |

---

## Findings

### F-01 — High: Whole-type administrative deletion races with worker registration

**Locations**

- `browser/redis_browser.py:594-641` — `_delete_server_type()`
- `RedisJobStatus.cc:473-484` — registration registry insertion
- `RedisJobStatus.cc:496-548` — metadata initialization and activation

**Problem**

The browser enumerates the type's current server IDs, deletes those instances, checks the type job index, and then deletes the type summary/state/live/retired keys and registry set. There is no type-level deletion marker, lock, or other mechanism that prevents a worker from registering a new server during this sequence.

A concurrent registration can therefore add a new registry member and initialize per-type data after the browser's initial enumeration. The browser can subsequently delete that new worker's state/summary keys and registry membership, or delete the registry set after the worker has added itself. The result can be a worker that believes registration succeeded but is no longer discoverable, or an ACTIVE worker whose lifecycle state was deleted.

The per-server DELETING tombstone protects deletion of one known server ID. It does not prevent a different ID from registering while the entire type is being removed.

**Recommendation**

Introduce a type-level lifecycle state (for example, ACTIVE/DELETING) in a key whose slot and atomicity model are explicitly designed for registration and type deletion. Registration must reject a type marked DELETING, and type deletion must atomically close registration before enumerating and removing instances. If that cannot be made atomic across the registry and type slots, use a two-phase type-retirement protocol with a durable registry-level fence and a documented recovery path. Do not rely only on a one-time enumeration.

### F-02 — Fixed: Lua membership checks used incorrect truthiness semantics

**Locations**

- `RedisJobScripts.inc` — `kTransitionScript`
- `RedisJobScripts.inc` — `kCompleteScript`

**Problem (fixed)**

These scripts previously tested the numeric result of Redis `SISMEMBER` using Lua's `not` operator. Redis returns integer `0` or `1`, and Lua treats numeric zero as truthy. As a result, a missing membership was not rejected by the intended integrity check.

**Fix applied**

Both membership checks in each script now compare the Redis result explicitly with `~= 1`. This correctly rejects a missing type-index or server-index membership.

The remaining `SISMEMBER` checks in this file were reviewed and already use explicit numeric comparisons.

**Regression coverage still recommended**

Add regression cases for each missing-index combination in both transition and completion operations. No runtime test was performed as part of this change.

### F-03 — Medium: Type-prefixed server-ID uniqueness is not validated by the implementation

**Locations**

- `RedisJobStatus.cc:364-369` — `validateLifecycleIdentity()`
- `RedisJobStatus.cc:451-452` — registration identity validation
- `RedisJobStatus.cc:468-484` — current-type-only duplicate checks
- `RedisJobStatus.cc:314-341` — host configuration loading

**Problem**

The new registration design correctly relies on the stated invariant that every `serverId` is type-prefixed and cannot collide across server types. However, the code reads `serverId` from host configuration and `validateIdentifier(serverId, "serverId")` checks only that it is non-empty. No code in this directory generates the ID or verifies that its prefix corresponds to `serverType`.

If the host configuration ever supplies an ID without the required prefix, or supplies a prefix for the wrong type, the current-type-only registry check will allow the same literal ID to be registered under another type. The global uniqueness property would then be false even though the code and documentation assume it.

**Recommendation**

Make the format contract explicit at the host boundary and validate it before registration. Define the exact encoding (including delimiter and escaping rules) rather than relying on an informal “prefix” convention. If IDs are generated outside this repository, add a test or integration assertion at that generation/configuration boundary. If validation cannot be added here because the format is owned by the host, document that as a mandatory external precondition.

### F-04 — Medium: Registration can leave a stale registry entry after a partial failure

**Locations**

- `RedisJobStatus.cc:473-494` — registry insertion
- `RedisJobStatus.cc:496-528` — type/server metadata initialization
- `RedisJobStatus.cc:533-544` — ACTIVE activation
- `RedisJobScripts.inc:488-505` — fencing behavior when state is absent

**Problem**

Registration commits registry membership before it initializes the type summary, server summary, and ACTIVE state. If the process crashes or a later Redis operation fails after the registry script succeeds but before activation, the registry set retains the ID while the per-type state hash has no lifecycle field for it.

The cleanup fence script returns without fencing when the state field is neither ACTIVE nor DELETING. Thus the normal cleanup scan does not remove this stale registry member. A subsequent registration attempt with the same ID is rejected by the registry membership check. The README acknowledges this partial-registration window and says such records need reconciliation, but the implementation has no automatic recovery mechanism.

**Recommendation**

Add a durable registration phase that can be recognized and safely recovered (for example, a PENDING registration state with a bounded lease/timeout), or add a reconciliation routine that distinguishes incomplete registration from a live server and repairs/removes it safely. A best-effort C++ rollback on exceptions can reduce ordinary failure residue but is not sufficient for process crashes. Document the manual repair procedure until automated recovery exists.

### F-05 — Fixed: Redis reset utility omitted live-server sets

**Location**

- `util/RedisJobStatusCleanup.cc` — `redisJobStatusPatterns()`

**Problem (fixed)**

The reset utility's key patterns included the registry, summaries, jobs, heartbeats, state hashes, retired indexes, and cleanup lease, but omitted `servers:live:{*}`. Consequently, stale live-server set membership could survive a reset. If the same server ID were reused, heartbeat fencing could mistake that membership for evidence of a prior heartbeat and fence the new process when its heartbeat key was absent.

**Fix applied**

Added `servers:live:{*}` to the reset utility's SCAN patterns. Both dry-run listing and confirmed `--delete` mode use the same pattern list, so live-server set keys are now included in the discovered key set.

**Regression coverage still recommended**

Add a test that seeds one key of every documented key family, runs dry-run discovery, and verifies that deletion mode finds all of them. No Redis runtime test was performed as part of this change.

### F-06 — Low: Whole-type browser deletion does not discover orphan heartbeat keys

**Locations**

- `browser/redis_browser.py:601-620` — orphan discovery
- `browser/redis_browser.py:633-641` — final type-key deletion
- `browser/redis_browser.py:531-535` — missing-state early return in per-server deletion

**Problem**

Whole-type deletion discovers server IDs from registry membership, state fields, server-summary keys, and server-job-index keys. It does not scan `heartbeat:server:{<type>}:*`. In addition, the per-server deletion script's “already absent” branch checks job-index, summary, and live-set artifacts, but not the heartbeat key. A heartbeat key orphaned from all the scanned indexes can therefore survive type deletion. It will normally expire by TTL, but the operation's confirmation text says it deletes all heartbeats.

**Recommendation**

Include heartbeat keys in orphan discovery, and include heartbeat existence in the per-server “already absent” test. Alternatively, explicitly narrow the UI's deletion guarantee and document that orphan heartbeat keys may remain until TTL expiry.

---

## Known operational limitation (documented, not a newly discovered defect)

### Legacy ACTIVE records without a registration timestamp

The README already documents that an older ACTIVE server with no heartbeat/live marker and neither `meta:registeredAt` nor the legacy `registeredAt:<serverId>` state field is not automatically expired by the first-heartbeat timeout. The fence script intentionally avoids guessing its age and returns without fencing it. Such records require an explicit audit/reconciliation during upgrades.

This is a deliberate fail-safe behavior, but it should remain part of deployment/upgrade checklists.

## Consistency and design checks that passed at source level

- The registration Lua script receives both registry keys explicitly and does not derive or discover additional Redis keys.
- The two registration Lua keys use the same `{registry}` hash tag.
- Job lifecycle scripts use the per-type key builders, and the lifecycle key list places the server state hash at `KEYS[6]`, matching the Lua scripts.
- The composite job reference uses a byte-length prefix in C++; the Python browser uses UTF-8 byte length, so the two implementations agree for non-ASCII server IDs.
- The reviewed multi-key lifecycle and cleanup scripts group their declared keys by server-type hash tag; registry operations are separate where the registry slot differs.
- The heartbeat refresh and fencing scripts serialize through the same per-type state hash, so a heartbeat racing a fence is resolved by Redis script execution order.

## Validation not performed

No C++ compilation, Python test run, Lua execution, standalone Redis test, or Redis Cluster test was performed as part of this audit or the fixes documented here. In particular, the findings above should be converted into regression tests before the next deployment. The repository directory contains no dedicated automated test suite for these C++/Lua lifecycle and admin workflows.

Recommended minimum regression coverage:

1. Transition and completion with each index membership missing independently and both missing.
2. Whole-type deletion racing a new server registration, including crashes between the registry and per-type phases.
3. Registration crashes/failures after registry insertion, after metadata initialization, and before activation.
4. Type-prefixed ID validation at the actual worker configuration/generation boundary.
5. Reset utility discovery of every key family, including live sets.
6. Browser whole-type deletion with orphan heartbeat keys and with a concurrent cleanup scan.
7. Redis Cluster execution for each multi-key Lua script, plus recovery after injected Redis errors at every multi-step boundary.
