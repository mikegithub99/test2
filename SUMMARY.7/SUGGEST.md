# Redis Job Status — Atomicity Hardening Recommendations

## Purpose

This is a proposal for strengthening the existing .7/RedisJobScripts.inc Lua scripts and .7/RedisJobStatus.cc callers. It does not claim that these changes are already implemented or tested.

The existing key layout is a good foundation: lifecycle keys for one server type share the {serverType} Redis Cluster hash tag. Create, transition, and complete can therefore update multiple keys in one slot in one Lua execution. Registration separately updates its two {registry} keys in one script. No current lifecycle operation needs to mix registry and server-type hash tags.

The remaining risk is a runtime error after the first write. Redis Lua execution is isolated from interleaving commands, but earlier successful writes are not automatically rolled back if a later command errors.

## 1. State the guarantees precisely

Use these terms consistently:

- Same-slot: every key passed to a multi-key script maps to the same Redis Cluster slot.
- Atomic execution / isolation: Redis does not interleave another command while the script executes.
- No automatic rollback: an error does not undo writes already completed by the script.
- Preflight validation: check all expected failure conditions before the first mutation.
- Application recovery: reconcile or compensate after unexpected errors or ambiguous transport failures.

Do not call the current scripts rollback transactions or claim formal all-or-nothing behavior.

## 2. Highest-priority Lua changes

### 2.1 Validate every counter that will be mutated

The current create script checks summary key types but does not validate existing pending and total field values before writing. If a counter contains a non-integer string, HINCRBY can fail after the job hash and index sets have already been changed.

Transition and complete use tonumber for some counter checks, but tonumber alone is insufficient for HINCRBY:

- A value such as 1.5 converts to a Lua number but is not a valid HINCRBY integer.
- Transition checks the old-state counter but does not validate the destination-state counter before incrementing it.
- Every field that will be incremented or decremented in both summary hashes must be validated.

Add a carefully tested helper that validates Redis signed 64-bit integer syntax and range without relying on imprecise floating-point conversion. Missing fields should be treated as zero only where that behavior is explicitly part of the data model.

Required preflight:

| Script | Counter fields to validate in both type and server summaries |
|---|---|
| Create | pending and total; ensure both can be incremented without signed 64-bit overflow |
| Transition | old-state and new-state counters; old counter must be at least one and destination counter must have increment headroom |
| Complete | current-state and total; both must be at least one |

Test empty strings, whitespace, decimals, exponent notation, signs, leading zeros, negative values where prohibited, maximum/minimum signed 64-bit values, overflow, and underflow. Do not assume tonumber proves an integer is valid.

### 2.2 Preflight every key type and existence condition

For each script, define the exact allowed state of every key before writing:

- Job key: absent for create; HASH for transition and complete.
- Type and server job indexes: SET or absent only when the operation explicitly permits an absent index; otherwise require SET and the required membership.
- Type and server summaries: HASH or absent only if missing-summary initialization is explicitly supported.
- Every key that will be incremented, decremented, deleted, or set must have its expected type before the first mutation.

The current validtype helper accepts absent index and summary keys. Decide explicitly whether absent summaries are valid. Transition and complete currently reject absent summaries indirectly through counter preconditions; make that contract deliberate and test it.

### 2.3 Validate arguments and metadata inside Lua

The C++ caller currently filters reserved metadata names and creates even field/value pairs from a std::map. Keep those checks in C++, but make Lua defensive as well:

- Check the exact minimum ARGV length for each operation.
- For create, require the metadata tail to contain complete field/value pairs.
- Reject empty metadata field names if they are not supported.
- Reject reserved fields jobId, status, serverType, and serverId even if a future caller bypasses the C++ filter.
- Validate required identity arguments are non-empty.
- Validate the target state against the exact supported state set.
- Compare the stored jobId with ARGV[1] in transition and complete. The current scripts read jobId from ARGV but do not compare it with the job hash field.

The C++ std::map currently provides unique metadata field names and deterministic ordering. Preserve and test that contract.

### 2.4 Make preflight and mutation visibly separate

Structure each lifecycle script into two phases:

1. Read-only preflight: inspect existence, key types, ownership, job ID, status, index membership, counter syntax and ranges, arguments, and every expected precondition. Calculate the exact counter deltas.
2. Mutation: perform only the already-validated writes in a short, fixed sequence. Do not introduce new ordinary-data validation branches after the first write.

Return the existing documented result codes for expected inconsistent-data conditions. Avoid using Redis runtime errors to represent normal validation failures.

This reduces the known failure surface; it does not create rollback.

### 2.5 Keep the mutation phase small

- Use fixed, known-good keys and validated arguments.
- Keep the write sequence short and deterministic.
- Avoid scans, long loops, or unrelated work inside lifecycle scripts.
- Do not add registry keys to lifecycle scripts; {registry} and {serverType} intentionally occupy separate slots.
- Preserve C++ KEYS order, ARGV order, and the result-code contract.

Do not split a lifecycle mutation into separate client commands. That would lose the current isolation guarantee and introduce interleaving windows.

## 3. C++ caller hardening

### 3.1 Validate the request before EVAL

Keep the current non-empty identifier checks and server-type brace restriction. Define and enforce a consistent identifier policy for maximum byte lengths, encoding, control characters, and metadata field names.

Before calling evalResult, ensure:

- The generated keys contain the intended exact hash tag.
- Key count matches the script contract: five lifecycle keys and two registration keys.
- Argument order and count match the script contract.
- Create metadata has an even number of field/value arguments and no reserved field names.
- TTLs and any numeric arguments fit Redis-supported ranges.

C++ checks improve error messages, but Lua remains the consistency boundary because Redis data may have been changed by another client.

### 3.2 Add a hash-slot contract test

Add a test that builds the exact production keys and computes their Redis Cluster slot.

Assert that:

- All five lifecycle keys for a server type map to the same slot.
- Both registration keys map to the same slot.
- No script mixes {registry} and {serverType} keys.
- Permitted punctuation in server IDs and job IDs does not change the hash tag.

This catches accidental key-builder drift before deployment.

### 3.3 Separate application results from Redis failures

Lua result codes (Success, NoOp, JobNotFound, InvalidTransition, InconsistentData, JobAlreadyExists, Disabled) are application outcomes. Redis command errors, script runtime errors, connection loss, timeout, and failover are different failure classes.

Preserve this distinction in the C++ API and logs:

- Map documented Lua return codes to application results.
- Surface Redis command/runtime errors distinctly; do not convert every exception into InconsistentData.
- Log operation name, server type, server ID, job reference, error class, and whether a reply was received, subject to data sensitivity.
- Never log credentials or a Redis URL containing secrets.

### 3.4 Treat timeouts as ambiguous outcomes

If the client times out or loses its connection while a script is executing, the caller may not know whether Redis completed the script and the reply was lost. Do not blindly retry every failed EVAL.

Recommended handling:

1. Distinguish definite pre-execution failure from ambiguous post-send timeout/disconnect where the client library permits it.
2. For an ambiguous result, read the job hash, indexes, and summaries and reconcile against the requested operation.
3. Retry only when observed state proves the operation did not take effect, or an operation-specific idempotency design makes retry safe.
4. Report an inconsistency for operator attention rather than silently repairing data in the request path.

Duplicate create, same-state transition, and complete of a missing job have some idempotent characteristics, but those outcomes do not always prove that a particular timed-out request was the one that committed.

### 3.5 Consider operation IDs if automatic retries are required

If callers need safe automatic retries across timeouts, consider adding an operation ID and a deduplication record in the same {serverType} slot and same Lua script. The script checks whether the operation ID was already applied, performs the mutation once, and records its result.

Define uniqueness and retention for operation IDs. This improves retry semantics; it is not rollback and does not protect against arbitrary runtime errors after writes unless the script also performs complete preflight validation.

### 3.6 Review RedisClient retry behavior

Inspect .7/RedisClient.h and .7/RedisClient.cc and the underlying client library's retry policy. Document which connection failures are retried, whether a command may already have reached Redis, and whether EVAL is retried automatically. Avoid retrying non-idempotent operations after an ambiguous send.

## 4. Test strategy

Use disposable Redis instances in standalone and Cluster modes. For every expected validation failure, snapshot all involved keys before the operation and assert that every key remains unchanged, including existence, type, fields, set members, and relevant TTLs.

### Lua preflight matrix

Test each of create, transition, and complete with:

- Wrong type for every key position.
- Missing required job, index, or summary key.
- Missing one required index membership.
- Owner mismatch in serverType or serverId.
- Stored jobId mismatch.
- Missing or unknown status.
- Invalid state transition.
- Missing counter fields where required.
- Nonnumeric, fractional, exponent-form, whitespace-padded, or out-of-range counters.
- Destination counter at signed 64-bit maximum.
- Metadata field/value count mismatch and reserved metadata field names.
- Existing job key on create.

For every expected validation failure, assert no writes occurred. For successful paths, verify exact job hashes, set memberships, and all counters in both summaries.

### Unexpected runtime error tests

Where feasible, add fault-injection tests using disposable keys to demonstrate that Redis does not undo earlier writes after an unexpected runtime error. These tests document the platform guarantee; they are not expected normal application paths.

### Concurrency and topology tests

- Run concurrent create/transition/complete requests for the same job and for different jobs in one server type.
- Verify no lost counter updates or illegal state transitions.
- Run against a real Redis Cluster and verify all keys for each script are same-slot.
- Exercise connection drop/timeout around script execution and test ambiguous-outcome handling.
- Test failover separately; Lua isolation alone does not establish failover durability.

## 5. If true rollback is a hard requirement

Redis Lua, Redis Functions, and MULTI/EXEC do not provide general automatic rollback of earlier successful writes after a runtime command error. WATCH-based optimistic transactions do not add SQL-style rollback either.

Possible alternatives:

### Option A — Keep Redis and harden preflight (recommended near-term)

Keep one same-slot Lua script per lifecycle mutation. Validate all expected failure conditions before the first write, test failure paths, and add reconciliation for unexpected errors.

This is the least disruptive option and fits the current hash-tag layout. It provides isolated execution and strong practical safety for known inputs and schema invariants, but not a formal rollback guarantee.

### Option B — Make one Redis key authoritative

If all authoritative state needed for a mutation can be represented in one Redis key, update that key with one Redis command. A single command avoids a multi-command partial-write window for the authoritative state.

Trade-off: query indexes and summaries become harder. They must be derived, treated as caches, or updated asynchronously. This does not work if correctness still depends on synchronous updates to several independent keys.

### Option C — Use an ACID database for authoritative state

If strict all-or-nothing multi-record updates are a business requirement, store authoritative job state, relations/indexes, and counters in a database with documented ACID transactions (for example PostgreSQL). Redis can remain a cache, queue, or read-optimized projection updated through an outbox or change-data-capture process.

This is the clearest route to true transactional rollback semantics, at the cost of another datastore and operational complexity.

### Option D — Application-level journal and compensation

Record an operation intent and prior values, apply the mutation, then mark it committed. A recovery worker can compensate or finish an interrupted operation. The journal and governed data need a clear atomic boundary, and recovery must be idempotent.

This is a saga/compensation design, not Redis-provided rollback. Readers may see intermediate states unless they honor the operation state.

### Option E — Stage a new version and switch a pointer

Write a complete new version into a staging namespace, validate it, then atomically switch one pointer/key so readers use the new version. Clean up old versions asynchronously.

This requires pointer-aware readers, version retention, and concurrency control. It is not a drop-in change for the current schema.

## 6. Recommended implementation order

1. Add tests that capture current successful behavior and verify same-slot key construction.
2. Fix counter validation, especially create's missing preflight and integer/range validation for every field each script mutates.
3. Harden Lua arguments and identity checks, including metadata pairs, reserved names, exact states, and stored jobId comparison.
4. Make preflight and mutation phases explicit.
5. Harden caller error handling to distinguish Lua result codes, Redis errors, and ambiguous transport outcomes.
6. Review client retry policy; prohibit blind automatic EVAL retries after ambiguous sends.
7. Add operation IDs only if retry requirements justify the API/schema change.
8. Run standalone and Cluster integration suites, including fault injection and concurrency tests.
9. Update SUMMAYAGENTS.md with verified behavior and changed contracts.

## 7. Acceptance criteria

- Every script's keys are proven to share one Cluster slot.
- Every expected validation failure is detected before the first write.
- Every counter field to be changed is validated as a valid in-range Redis integer before the first write.
- Create, transition, and complete validate the identity, ownership, state, indexes, key types, and arguments they rely on.
- C++ preserves the distinction between Lua business results, Redis script errors, and ambiguous transport outcomes.
- Retry behavior is documented and tested.
- Standalone and Cluster tests cover success, no-op, invalid inputs, wrong key types, malformed counters, concurrency, and ambiguous timeout handling.
- Documentation calls the result same-slot atomic execution with preflight validation, not automatic rollback.
