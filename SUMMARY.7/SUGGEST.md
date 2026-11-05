# Redis Job Status — Focused Hardening Recommendations

## Current baseline

The lifecycle scripts use five keys sharing the `{serverType}` Redis Cluster hash tag. This keeps each create, transition, and complete operation in one slot and one Lua execution.

Recent C++ improvements in this folder already:
- Centralize the reserved job fields: `jobId`, `status`, `serverType`, and `serverId`.
- Centralize metadata argument construction in `appendMetadataArgs()`, which emits a pair count followed by key/value pairs.
- Filter reserved job metadata fields before calling the create script.
- Reject empty metadata field names in the C++ entry points.

The create Lua contract is therefore `ARGV[5] = metadata pair count`, followed by pairs beginning at `ARGV[6]`. Keep this C++/Lua contract synchronized.

These improvements strengthen input construction; they do not yet make the Lua scripts rollback transactions. Redis prevents interleaving while a script runs, but does not undo earlier writes if a later command errors.

## Recommended changes, in priority order

### 1. Validate every counter before the first write — highest priority

Create currently checks summary key types, then writes the job and indexes before calling `HINCRBY`. It does not first validate the existing `pending` and `total` values. A malformed counter or an increment overflow can therefore leave a partially created job.

Transition and complete also need stronger checks:
- Transition currently uses `tonumber()` for the old-state counter and total, but does not validate the destination counter before incrementing it.
- `tonumber()` does not prove a value is a valid Redis integer: decimals and exponent notation can be accepted as Lua numbers.
- Complete must validate the current-state and total counters before decrementing them.

Add one carefully tested Lua helper that checks Redis signed 64-bit integer syntax and range without relying on imprecise floating-point conversion. For every summary hash being changed, preflight all affected fields:
- Create: `pending` and `total` can each be incremented safely.
- Transition: old-state counter can be decremented (and is at least one); destination-state counter can be incremented safely.
- Complete: current-state and `total` counters can each be decremented (and are at least one).

Treat missing fields as zero only where that is the defined data-model behavior. Test malformed strings, decimals, exponent notation, whitespace, negative values where prohibited, and signed 64-bit boundaries.

### 2. Complete Lua argument and identity validation

Keep the existing C++ checks, and make the Lua scripts independently enforce their input contract before any mutation.

For create:
- Require a valid, non-negative metadata pair count.
- Require exactly two arguments per pair; reject missing or extra arguments.
- Reject empty and reserved metadata names (`jobId`, `status`, `serverType`, `serverId`).
- Validate required identity arguments.

For transition and complete:
- Compare the hash's stored `jobId` with `ARGV[1]`, in addition to checking stored server type and server ID.
- Validate the requested state against the exact supported state set.

The C++ `std::map` already gives unique metadata names and deterministic ordering. Preserve that behavior. The Lua checks are a defensive boundary, not a replacement for C++ validation.

### 3. Make preflight and mutation separate phases

For each lifecycle script, do all expected-failure checks before the first write: key existence and types, ownership, stored job ID, status, index membership, arguments, and exact counter validity/range. Then perform a short, fixed mutation sequence without adding ordinary-data validation after writes begin.

Also decide and document whether missing index or summary keys are permitted. The current type helper accepts absent keys, so each operation should explicitly define whether absence is valid.

### 4. Distinguish an existing job from a wrong-type job key

Create currently uses `EXISTS` and returns AlreadyExists for any existing key. Return AlreadyExists only when the key is a HASH representing an existing job; treat an existing key of another type as inconsistent data.

### 5. Test that validation failures do not change data

Add standalone Redis tests first, then Redis Cluster tests. For each expected validation failure, snapshot all affected keys and assert that their existence, types, fields, set members, and relevant TTLs remain unchanged.

Cover:
- Malformed, fractional, missing, and boundary counter values in both summaries.
- Destination-counter overflow on transition.
- Wrong key types and missing required index membership.
- Wrong stored job ID or ownership fields.
- Invalid metadata count, incomplete pairs, empty names, and reserved names.
- Existing job keys of both HASH and non-HASH types.
- Successful create, transition, and complete paths with exact counter and index results.

Add a hash-slot test for the five lifecycle keys. Run the integration suite against a real Redis Cluster as well as standalone Redis.

### 6. Handle Redis errors and ambiguous timeouts separately

Keep documented Lua result codes distinct from Redis command/runtime errors and connection failures. A timeout or disconnect after sending EVAL can leave the caller unsure whether Redis completed the operation. Do not blindly retry a potentially non-idempotent EVAL; inspect the resulting state or use an explicit operation-ID/deduplication design if automatic retries are required. Verify the actual redis-plus-plus retry configuration before documenting retry behavior.

## Implementation order

1. Add regression tests for current successful behavior and no-change-on-validation-failure.
2. Implement exact signed 64-bit counter validation and preflight in create, transition, and complete.
3. Add Lua metadata argument checks and stored-job-ID validation.
4. Correct create's wrong-type job-key result.
5. Run standalone and Cluster tests, including boundary and concurrency cases.
6. Document verified behavior in `SUMMAYAGENTS.md`.

## Guarantee to claim

Describe the result as **same-slot isolated Lua execution with preflight validation**. Do not describe it as automatic rollback or a formal all-or-nothing transaction: Redis does not undo successful writes that precede an unexpected runtime error.
