# Implementation Evaluation — `3-colocated`

## Scope and confidence

This document records a source-level review of the current `main` branch of `3-colocated`. It covers the C++ headers and implementations, embedded Redis Lua scripts, heartbeat and cleanup logic, design documentation, the Python browser mock, and the utility programs.

This is **not** a report of a successful build or runtime test. No compilation, linting, standalone Redis test, or Redis Cluster test was performed as part of this review. Findings below distinguish source-level observations from behavior that still needs runtime validation.

The system's priorities, in order, are:

1. Preserve correctness of per-type aggregate job counters.
2. Make job lifecycle operations idempotent and safe under concurrency.
3. Fence dead servers and clean up their state without double-accounting.
4. Respect Redis Cluster hash-slot boundaries.
5. Keep documentation aligned with the actual implementation.

## Overall assessment

The architecture is well considered. In particular, colocating a server type's jobs, indexes, summaries, lifecycle state, heartbeat state, and cleanup data in the same Redis Cluster hash slot allows lifecycle and cleanup scripts to serialize their mutations. The three-phase cleanup process correctly recognizes that the global registry is in a different slot and therefore cannot be updated atomically with per-type state.

The normal job lifecycle paths have strong counter-safety properties: they validate affected counters before mutation, update related indexes and counters in a single Lua execution, and remove completed jobs so repeated completion does not decrement counters again.

The earlier cleanup-counter and stale-index findings in this document have been fixed in the current source. The remaining registration hardening is to ensure every registry key inspected by Lua is explicitly supplied through KEYS; registration now does this and retries if its type snapshot becomes stale. The remaining risks are primarily cross-slot registration recovery and the need for runtime validation.

**Source-level quality assessment: approximately 8.5/10.** The reviewed source has strong same-slot lifecycle and cleanup safeguards, exact counter preflight, stale-index rejection, and explicitly declared registry Lua keys. This is not a production-readiness score: compilation and failure/concurrency tests against Redis and Redis Cluster are still required.

## 1. Counter correctness

### 1.1 Normal lifecycle paths

The following design properties are sound:

- `createPendingJob()` validates the server lifecycle state and updates the job hash, type/server indexes, and type/server counters in one per-type-slot Lua script.
- `transitionJob()` validates job ownership, index membership, allowed status transition, and affected counters before mutation. Status and counter changes occur in the same script.
- `completeJob()` validates the active server, job identity, both indexes, current status, and affected counters before mutation. It decrements counters, removes both references, and deletes the job in one script.
- Lifecycle operations and Phase A cleanup use the same per-type slot. Redis serializes those Lua executions, so a normal create/transition/complete operation cannot interleave its writes with cleanup for that type.
- A successful first `completeJob()` returns `Success`; a repeated call while the server remains active returns `JobNotFound` without another decrement.
- If the server has already been fenced, the lifecycle call returns `ServerNotActive`. This is the appropriate result because lifecycle mutations are prohibited after fencing.

The shared counter validator used by the normal lifecycle scripts validates digit-only values, the signed 64-bit upper bound, decrement sufficiency, and increment overflow. The C++ counter parser is also strict about malformed, negative, trailing, and out-of-range values.

### 1.2 Cleanup counter preflight

<badge>Fixed in current source</badge>

`kCleanupExpiredServerScript` now validates counters using digit-string normalization and decimal-string comparisons. It rejects non-digit values, values above the signed 64-bit maximum accepted by Redis `HINCRBY`, and values smaller than the decrement required by the jobs being removed. It does not convert stored counters to Lua floating-point numbers.

The script validates all candidate jobs, ownership, statuses, index membership, and aggregate counter sufficiency before its first write. This matters because Redis Lua scripts prevent interleaving but do not roll back earlier writes after a later runtime error.

The normal lifecycle scripts use the shared exact counter validator and preflight increments/decrements before mutation. Both paths now use integer-safe validation appropriate to their operations.

### 1.3 Stale job-index membership

<badge>Fixed in current source</badge>

`kCreatePendingScript` now checks both the type-job and server-job indexes before its first write. If the job hash is absent but either index already contains the composite reference, creation returns `InconsistentData` rather than allowing `SADD` to be a no-op while counters are incremented.

### 1.4 Counter-path summary

| Operation | Assessment |
|---|---|
| Create pending job | Exact counter preflight, stale-index rejection, and atomic updates |
| Transition | Good preflight and atomic status/counter/index updates |
| Complete | Good preflight and atomic counter/index/job deletion |
| Server cleanup | Structural and exact counter validation before mutation; decrements derived from validated jobs |
| Repeated completion | Correct; no second decrement |
| Concurrent completion and cleanup | Serialized in the same per-type slot |
| Counter reads | Strict parsing; malformed values fail rather than silently becoming zero |

A sound cleanup design choice is that Phase A derives type-level decrements from the jobs it actually validates, rather than trusting the server summary counters that it is about to delete.

## 2. Registration, heartbeat, fencing, and cleanup

### 2.1 Registration

The current registration sequence is:

1. Add the server type and server ID to the global registry.
2. Initialize type metadata.
3. Initialize server-instance metadata.
4. As the final activation step, atomically generate `meta:registeredAt` using Redis `TIME` and set the per-type server state to `ACTIVE`.

This ordering is sensible: a server does not become active before registry membership and metadata initialization have completed. The timestamp and ACTIVE transition are atomic within the per-type slot.

Registration is not atomic across the global registry slot and the per-type slot. A crash between those operations can leave a stale global registry member without an active per-type state entry. The scanner skips missing state, so stale registry membership requires reconciliation.

The registration policy rejects an ID while it remains in any known per-type registry set or per-type state hash, including a DELETING tombstone. Phase C removes the tombstone, after which the ID may be reused. Workers must nevertheless generate a fresh ID for every process incarnation and must not replay registration late after retirement; Redis cannot distinguish a delayed old process from a new one once Phase C has completed.

### 2.2 Heartbeat and fencing

The normal heartbeat/fencing design is strong:

- Heartbeat refresh checks that the server is `ACTIVE`.
- Heartbeat TTL refresh and live-set membership are updated in one per-type-slot script.
- Fencing checks heartbeat/live membership and transitions `ACTIVE` to `DELETING` in the same slot.
- Heartbeat refresh racing with fencing is serialized by Redis.
- A server that has never heartbeated receives the configured first-heartbeat grace period.
- A server that has heartbeated before can be detected as dead when its heartbeat expires and it is no longer live.

Operational qualification: heartbeat-key expiration does not itself fence the server. The cleanup scanner must run and execute the fencing script. Until then, a server whose state remains `ACTIVE` can still pass lifecycle checks.

### 2.3 Phase A cleanup

Phase A validates the deleting state, heartbeat absence, server job references, job hashes, ownership, statuses, both indexes, and aggregate counter sufficiency before mutating data. It updates type counters and removes the server's jobs and related state in one per-type-slot script, retaining a tombstone for later phases.

Repeated Phase A execution is designed not to decrement counters a second time. The counter numeric-validation weakness described in section 1.2 is the significant exception to the otherwise strong preflight.

### 2.4 Phase B and Phase C

The registry removal and tombstone collection are correctly separated because the global registry and per-type data occupy different Redis Cluster slots.

The retired-server sorted set provides a durable cleanup index independent of the global registry. The HSCAN migration/backfill logic is sensible: existing retirement scores are preserved with `ZADD NX`, and a missing state hash is treated as an empty migration source.

The Phase C registry-absence check and per-type tombstone deletion cannot be atomic together. Registration checks per-type state hashes in C++ and then performs its registry-slot operation. IDs are reusable after Phase C removes the tombstone, so each process incarnation must use a fresh ID and must not replay registration after retirement.

### 2.5 Cleanup lease

The cleanup lease is appropriately treated as scan coordination rather than as the correctness mechanism. Correctness comes from per-server state transitions and the per-type Lua scripts. Token-checked lease renewal/release is appropriate.

## 3. C++ API and implementation

The C++ structure is generally clean:

- `RedisClient` abstracts standalone and Cluster clients behind a common interface.
- The singleton/configuration behavior is consistent with the documented first-construction semantics.
- Metadata fields owned by the API are reserved against caller override.
- Type-level and server-instance metadata have distinct purposes.
- Counter parsing is strict.
- Lua numeric results map to the public `Result` enum.

The `completeJob()` result behavior is appropriate for the intended Java UI flow:

- First successful deletion/completion: `Success`.
- Repeated request while the server remains active: `JobNotFound`.
- Request after the server is fenced: `ServerNotActive`.

A separate `AlreadyGone` result is not required for counter correctness. Distinguishing a previously deleted job from a job that never existed would require additional retained history and would not prevent a second decrement beyond the current behavior.

Smaller defensive improvement: C++ currently casts Lua integer results directly to `Result`. The scripts and enum are expected to remain synchronized, but checking that a returned integer is a known enum value would make unexpected script/protocol mismatches easier to diagnose.

The manager starts its cleanup thread when `RedisHeartbeatManager::Instance()` is first constructed, independently of `HeartbeatStart()`. This is documented, but host startup should deliberately control manager construction. Also, if status reporting is disabled, `RedisJobStatus` is inert while constructing the heartbeat manager can still initialize Redis and start cleanup. The host should avoid constructing the manager when disabled, or the components should have an explicit shared enablement policy.

No normal-path C++ lifecycle ordering defect was identified in this source review that invalidates the same-slot counter guarantees.

## 4. Redis Cluster correctness

The key layout consistently separates global registry/lease keys under `{registry}` from per-server-type lifecycle, heartbeat, job, summary, and cleanup keys under `{serverType}`.

The multi-key Lua operations reviewed use keys from one slot. Registration's registry Lua script now receives the global type set and every per-type registry set it may inspect as explicit `KEYS` arguments; it does not construct and access undeclared registry keys. The C++ caller supplies a snapshot of known types, and the script returns a retry result if the type set changed before execution. Since the script runs atomically, it can then check all supplied registry sets and insert the ID without a concurrent type-addition race.

Registration still checks per-type state hashes in C++ before the registry-slot script because those hashes occupy different Cluster slots. That state check plus registry insertion is not one cross-slot atomic operation. All keys within the registration Lua invocation share the literal `{registry}` hash tag; this is not a hash-slot mismatch.

This is a key strength: a script that works on standalone Redis can still fail with `CROSSSLOT` in Redis Cluster. The implementation keeps each multi-key script within one slot.

This conclusion is source-level only. Actual Cluster integration tests are still required.

## 5. Utilities and browser mock

### 5.1 `util/RedisJobStatusCleanup.cc`

The utility has a sensible safety posture:

- Dry-run by default.
- Explicit `--delete` required.
- Requires the user to type `DELETE`.
- Includes registry lease, retired indexes, state hashes, summaries, jobs, and heartbeat keys in its patterns.

It constructs `sw::redis::Redis`, not `RedisCluster`. Treat it as a standalone Redis utility, not a complete Cluster-aware keyspace cleanup tool. Scanning one Cluster node does not guarantee a complete scan of the Cluster keyspace.

### 5.2 `util/RedisStatusCli.cc`

The CLI correctly advances the SCAN cursor and displays hashes, sets, strings, and sorted sets. Showing retired sorted-set members and scores is useful for diagnosing tombstone retention.

It has the same standalone-only limitation as the cleanup utility. The README should state this explicitly.

### 5.3 `browser/redis_browser.py`

This is a Python mock UI, not the production deletion architecture.

Its metadata display and timestamp formatting are reasonable. Its direct Redis deletion paths attempt to validate job ownership, indexes, and counters atomically, which is useful for mock administration. They should not be treated as the authoritative production deletion path.

The intended production flow is that the Java UI sends a deletion request to the responsible server, and the server uses the existing C++ lifecycle operation (`completeJob()`) to perform the deletion. That keeps counter mutation in the authoritative C++ path.

## 6. Documentation accuracy

### 6.1 README

The README is substantially accurate and unusually thorough. It describes the key layout, hash-slot boundaries, metadata ownership, registration ordering, heartbeat behavior, three cleanup phases, tombstone migration, dashboard read consistency, and known cross-slot limitations.

No major architectural claim was identified as contradicting the implementation in this review.

Recommended clarifications:

| Topic | Assessment | Recommendation |
|---|---|---|
| Registration order | Accurate | Keep |
| Metadata generation | Accurate | Keep |
| Heartbeat and first-heartbeat timeout | Accurate | Keep |
| Three-phase cleanup | Accurate | Keep |
| Cleanup lease guarantees | Accurate with stated limits | Keep |
| Dashboard snapshots | Accurate | Keep |
| Counter preflight | Exact digit-string validation in normal lifecycle and cleanup | Current source validates before mutation |
| Utilities | Incomplete | State that both C++ utilities currently use standalone Redis |
| Browser | Needs context | Identify Python browser as a mock, not the production UI |

The registration-key declaration contract should be documented precisely: every key inspected by registration Lua is passed in KEYS, with a retry if the supplied type snapshot is stale. Registration's C++ state-hash checks remain cross-slot and therefore are not atomic with the registry operation.

### 6.2 `HEARTBEAT_DESIGN.md`

This document is consistent with the current architecture and is a useful technical design reference. The cleanup implementation now uses exact digit-string counter preflight; keep this design document aligned if counter-validation details are expanded there.

### 6.3 `NEWIDEA.md`

This document is stale and should not be mistaken for a current implementation guide:

- It refers to `RedisHeartbeatManager::Start()`, while the current API is `HeartbeatStart()`.
- It does not properly present the current Phase C tombstone collection.
- It is largely superseded by the README and `HEARTBEAT_DESIGN.md`.

Recommendation: mark it clearly as a historical proposal or update it to point to the current design.

## 7. Quality summary

| Area | Assessment |
|---|---|
| Normal job lifecycle and counters | Strong |
| Concurrent lifecycle operations | Strong |
| Repeated completion behavior | Strong |
| Cleanup counter validation | Needs improvement |
| Heartbeat and fencing | Strong |
| Three-phase cleanup | Strong, subject to explicit identity assumptions |
| Redis Cluster key design | Strong |
| Registration failure recovery | Documented limitation; reconciliation needed |
| C++ API and configuration | Good |
| Dashboard reads | Good, with expected cross-slot limitations |
| Utilities | Useful, standalone-only |
| Python browser mock | Reasonable mock; not production architecture |
| README | Good and substantially accurate |
| Validation evidence | Insufficient for production confidence |

## 8. Recommended work before deployment

Prioritized by impact on the system's stated goals:

1. Add defensive validation for unexpected Lua result integers in C++.
2. Update `NEWIDEA.md` and clarify the standalone-only utility limitation in the README.
3. Add automated tests for counter underflow, overflow, malformed values, partial/corrupted indexes, stale registration snapshots, and cleanup racing with create, transition, and completion.
4. Run those tests against standalone Redis and Redis Cluster, including failure injection around all three cleanup phases and concurrent registration of the same ID under newly introduced types.

The earlier cleanup-counter and stale-create-index findings are fixed in the current source. The remaining priority is runtime validation and recovery testing, not a redesign of the architecture.

## Conclusion

The implementation has a strong architectural foundation. Same-slot Lua serialization, explicit lifecycle states, idempotent cleanup phases, strict normal-path counter validation, and clear Redis Cluster boundaries are all appropriate choices for this system.

The reviewed source now has exact cleanup counter preflight, stale-index rejection during creation, and explicit registry Lua key declarations with stale-snapshot retry. The principal remaining confidence gap is the absence of compile, standalone Redis, and Redis Cluster tests, especially around concurrent registration and cross-slot recovery.
