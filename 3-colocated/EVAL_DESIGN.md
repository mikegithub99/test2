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

The main implementation gap found in this review is that the cleanup script's counter preflight is weaker than the shared validation used by the normal lifecycle scripts. Because Redis Lua scripts are atomic with respect to interleaving but do not roll back earlier writes after a later runtime error, this is material to the system's highest-priority invariant.

**Source-level quality assessment: approximately 7.5/10.** The design is strong, but the implementation should not be described as production-validated until the counter-validation gap is fixed and the failure/concurrency cases below are exercised against Redis, including Redis Cluster.

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

### 1.2 Finding: cleanup counter preflight is weaker

**Priority: high. Fix before relying on the aggregate-counter integrity guarantee.**

In `RedisJobScripts.inc`, `kCleanupExpiredServerScript` has a separate `enough()` validator that parses counter values with Lua `tonumber()` and compares the result to the decrement.

This is weaker than the shared exact digit-string validator used by create, transition, and completion:

1. Lua numeric conversion uses floating-point representation and cannot exactly represent every signed 64-bit integer.
2. The cleanup validator does not enforce the maximum signed 64-bit value accepted by Redis `HINCRBY`.

Redis Lua scripts prevent other commands from interleaving during execution, but they do **not** roll back writes already performed if a later Redis command raises a runtime error. Consequently, a malformed or out-of-range counter can pass a floating-point comparison and then cause a later `HINCRBY` to fail after an earlier counter has already been changed. That can leave aggregate counters partially updated.

**Recommendation:** use the same exact digit-string and signed 64-bit validation for cleanup as for the normal lifecycle scripts. For cleanup's potentially larger decrement, compare digit strings or otherwise use an exact integer-safe method; do not convert the stored counter to a Lua floating-point number. Complete all validation before the first write.

### 1.3 Finding: create does not reject stale index membership

**Priority: medium.**

`kCreatePendingScript` checks that the job hash does not already exist and validates the index key types, but it does not check whether the job reference is already present in either index.

If a job hash is absent but a stale reference remains in one or both indexes, creation can proceed. A subsequent `SADD` may be a no-op while the script increments counters as if it had created a new indexed job.

This is primarily a defensive consistency gap; the normal lifecycle scripts are intended to maintain these structures together.

**Recommendation:** before the first write, check that the reference is absent from both the type and server job indexes. If either already contains the reference while the job hash is absent, return `InconsistentData`.

### 1.4 Counter-path summary

| Operation | Assessment |
|---|---|
| Create pending job | Good preflight and atomic updates; add stale-index absence checks |
| Transition | Good preflight and atomic status/counter/index updates |
| Complete | Good preflight and atomic counter/index/job deletion |
| Server cleanup | Good structural validation and aggregation from actual jobs; counter numeric validation needs strengthening |
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

The once-only `registerServer()` call, globally unique server IDs, and never-reused IDs are important assumptions in the safety argument. They are host-application invariants, not globally enforced uniqueness constraints in Redis.

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

The Phase C registry-absence check and per-type tombstone deletion cannot be atomic together. This is an accepted architectural boundary, relying on globally unique, never-reused server IDs and the rule that a retired registration is not resumed or replayed late.

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

The multi-key Lua operations reviewed use keys from one slot. Registration and Phase B/C appropriately split work across slots rather than attempting cross-slot Lua transactions.

This is a key strength: a script that works on standalone Redis can still fail with `CROSSSLOT` in Redis Cluster. The implementation accounts for that constraint.

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
| Counter preflight | Overstates uniformity of validation | Fix cleanup validator and describe the guarantee precisely |
| Utilities | Incomplete | State that both C++ utilities currently use standalone Redis |
| Browser | Needs context | Identify Python browser as a mock, not the production UI |

The most important documentation qualification is the cleanup counter preflight. Cleanup does validate before writing, but its numeric validation is not currently equivalent to the exact signed-64-bit validation used in the normal lifecycle scripts.

### 6.2 `HEARTBEAT_DESIGN.md`

This document is consistent with the current architecture and is a useful technical design reference. It should receive the same cleanup-counter qualification until the validator is fixed.

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

1. **Fix cleanup counter preflight** to use exact signed-64-bit validation rather than Lua floating-point comparisons.
2. Add stale-index absence checks to `kCreatePendingScript`.
3. Add defensive validation for unexpected Lua result integers in C++.
4. Update `NEWIDEA.md` and clarify the standalone-only utility limitation in the README.
5. Add automated tests for counter underflow, overflow, malformed values, partial/corrupted indexes, and cleanup racing with create, transition, and completion.
6. Run those tests against standalone Redis and Redis Cluster, including failure injection around all three cleanup phases.

The first two items should be addressed before broader polish. The architecture does not need redesign, and the current completion result model should be retained. The main task is to make the defensive guarantees in cleanup and corrupted-data scenarios as strong as those already present in the normal job lifecycle.

## Conclusion

The implementation has a strong architectural foundation. Same-slot Lua serialization, explicit lifecycle states, idempotent cleanup phases, strict normal-path counter validation, and clear Redis Cluster boundaries are all appropriate choices for this system.

The most important remaining risk is exceptional-path counter integrity. Fixing cleanup's weaker numeric preflight and rejecting stale index membership during creation would materially improve confidence. After that, the priority should be automated failure/concurrency testing on real Redis and Redis Cluster rather than architectural changes.
