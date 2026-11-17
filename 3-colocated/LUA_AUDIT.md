# Redis Lua Audit

**Scope:** Lua scripts embedded in `RedisJobScripts.inc`, their C++ invocation paths in `RedisJobStatus.cc` and `RedisHeartbeatManager.cc`, and the design notes in `LUA.md`.

**Review type:** Static code review only. No Redis instance, Redis Cluster, build, or automated tests were run.

## Executive summary

The scripts generally follow a sound pattern: keys are passed explicitly, Cluster operations are grouped by hash slot, key types and state are checked before mutations, and counter decrements are preflighted. The registration and retirement workflows also document the important cross-slot boundaries.

The main improvement opportunities are defensive validation consistency and operational handling of the intentionally split registry/per-type registration sequence. No source changes were made as part of this audit.

## Findings

### 1. Medium — Lifecycle scripts do not explicitly validate the job hash type

**Scripts:** `kTransitionScript`, `kCompleteScript`

Both scripts check `EXISTS jobKey` and then read fields with `HGET`, but do not first require `TYPE jobKey == 'hash'`. If a corrupted job key exists with another Redis type, `HGET` raises a Redis runtime error rather than returning the scripts' `RESULT_DATA_INCONSISTENT` result.

This happens before any writes, so it does not create a partial lifecycle update. Still, it makes corruption handling inconsistent with the explicit type checks applied to the index and summary keys.

**Recommendation:** Check the job key type explicitly: return `RESULT_NOT_FOUND` for `none`, accept `hash`, and return `RESULT_DATA_INCONSISTENT` for any other type. Keep this as a separate source change, with tests for a string-typed job key.

### 2. Medium — Registration spans two Redis Cluster slots and can leave registry membership ahead of activation

**Scripts/call path:** `kRegisterServerScript`, `kRegisterAndActivateServerScript`; `RedisJobStatus::registerServer()`

The code first inserts the server ID into the global `{registry}` slot, then initializes per-type metadata and ACTIVE state in the `{serverType}` slot. The two operations cannot be one Redis Cluster Lua execution. If the second operation fails after registry insertion, the registry can contain an ID that has no corresponding ACTIVE state or completed metadata initialization.

The code comments and `LUA.md` correctly describe this boundary, so this is a known consistency window rather than a hidden atomicity claim.

**Recommendation:** Define and document a recovery/reconciliation policy for registry entries whose per-type state is absent. If registration retries are expected to repair this case, add a deliberate idempotent retry path and tests covering failure between the two script calls. Do not imply cross-slot atomicity.

### 3. Low — Shared helper logic is duplicated across lifecycle scripts

**Scripts:** `kCreatePendingScript`, `kTransitionScript`, `kCompleteScript`

The `keytype()` and `validtype()` helper bodies are repeated in all three scripts. The counter validation helper is already maintained as a shared prelude, demonstrating the existing composition approach.

**Recommendation:** This is a maintainability opportunity, not a correctness defect. The proposed helper-prelude refactor was previously considered and abandoned; this audit does not recommend making that change now. Keep the current source unchanged unless the refactor is explicitly reopened.

### 4. Low — Some argument validation relies on the C++ caller's construction guarantees

**Scripts:** `kCreatePendingScript` and `kCleanupExpiredServerScript`

The cleanup script validates its count and key/argument lengths before using the supplied job references. The create script converts `ARGV[5]` with `tonumber` and uses it as a loop bound without an equivalent explicit integer/range/argument-length check.

The current C++ caller constructs this count and its field/value pairs internally, so this is not directly exposed as an untrusted client input in the reviewed call path. However, the script is less defensive if it is ever invoked through a new call path or reused independently.

**Recommendation:** Consider validating that the count is a non-negative integer and that the argument length exactly matches the expected field/value layout before the first write. Add malformed-argument tests if the script's invocation surface expands.

### 5. Low — Script atomicity should not be confused with rollback

**Scripts:** Applies to all scripts that perform multiple writes.

Redis runs each Lua script without interleaving commands from other clients, but a runtime error after a successful write does not roll back earlier writes. The file header and `LUA.md` already state this correctly.

**Recommendation:** Preserve the current validate-before-write discipline. For future changes, review every command after the first mutation for possible runtime errors, including argument validity, key types, and Redis command constraints. Where practical, add tests that deliberately corrupt key types and values before execution.

## Positive observations

- Per-type job, summary, heartbeat, and state keys use the `{serverType}` hash tag; global registry and lease keys use `{registry}`. The reviewed script call paths keep each script's keys within the appropriate slot.
- The counter prelude validates decimal syntax and signed 64-bit bounds using string comparisons, avoiding Lua number precision loss for large counters.
- Lifecycle counter decrements are checked for positive balances before `HINCRBY`.
- Cleanup validates the server-job index against the supplied references and verifies ownership, status, and index membership before deleting jobs or decrementing counters.
- The fencing and tombstone-collection phases use Redis `TIME` for retirement timing, avoiding reliance on host clock agreement.
- Registration validates key types and metadata argument layout before writing the per-type metadata and ACTIVE state.
- Snapshot scripts explicitly declare their Redis keys and tolerate expected concurrent index changes in the job snapshot path.

## Suggested test coverage

1. Transition and completion against a job key of the wrong Redis type.
2. Missing, malformed, negative, fractional, and out-of-range counter values for create, transition, completion, and cleanup.
3. Counter values at zero, the signed 64-bit maximum, and one below the maximum.
4. Wrong-type index, summary, state, heartbeat, retired-index, and lease keys.
5. Registration failure after registry insertion but before per-type activation, followed by the documented recovery/retry behavior.
6. Cleanup with missing job hashes, mismatched ownership, missing index membership, stale candidate references, and repeated/concurrent cleanup attempts.
7. Tombstone backfill and collection across empty state hashes, existing retired timestamps, and retention-boundary timestamps.
8. Empty and non-empty snapshot inputs, including concurrent additions/removals and wrong-type indexed records.

## Audit conclusion

The reviewed Lua design is coherent and has useful defensive checks. The most actionable code-level improvement is explicit job-hash type handling in transition and completion. The most important system-level concern is the documented cross-slot registration recovery window. The remaining items are defensive-hardening or maintainability recommendations.

No Lua scripts or C++ source files were modified by this audit.
