# 3-colocated Redis design review: cross-slot and consistency risks

**Review date:** 2026-11-16  
**Scope:** Source-level review of the current 3-colocated implementation, focusing on Redis Cluster slot boundaries, registration/cleanup failure windows, getter consistency, and related lifecycle invariants.  
**Files reviewed:** README.md, RedisJobStatus.h/.cc, RedisJobScripts.inc, RedisHeartbeatManager.h/.cc, LUA.md, and get-jobs/README.md.

This is a source review, not a formal proof. No Redis Cluster integration tests, fault-injection tests, or concurrency tests were run as part of this review.

## Executive summary

The per-server-type job lifecycle is generally well colocated: job hashes, indexes, summaries, heartbeat, live membership, and lifecycle state use the {serverType} hash tag. The scripts that coordinate these records pass their keys explicitly and execute within that slot. Heartbeat refresh and expiry fencing are serialized against the same state hash, and job mutations reject a server after it has been fenced.

The principal consistency weakness is registration. Registration deliberately crosses from the {registry} slot to the {serverType} slot through several independent operations. A process failure can leave registry membership and partial metadata without an ACTIVE state. The current scanner does not reconcile that shape: the fence script returns 0 when the state field is absent, so the registry member remains indefinitely.

This is primarily a registry/metadata hygiene and discoverability issue, not a type-counter corruption issue:

- getServerTypeSummary(serverType) reads the type summary hash directly. It does not derive counters from the registry.
- getServerSummaries(serverType) enumerates IDs from the registry, then reads extant per-server summary hashes. Its Lua script omits missing summary hashes.
- Consequently, a registry-only orphan does not add counters or produce a server-summary row. A partially written per-server summary hash can, however, produce a metadata-only row.
- getServerTypes() reads the global registry type set, which is not pruned when a type loses its last server. It is therefore a catalogue of types ever registered, not a reliable list of types that currently have active servers.

The cleanup protocol's cross-slot boundaries are explicit and mostly handled deliberately. The main limitations are non-atomic registration, non-snapshot getter discovery, and unbounded per-server cleanup/read batches.

## 1. Redis Cluster slot map

| Operation / data | Keys touched | Slot | Assessment |
|---|---|---|---|
| Job create/transition/complete | Job hash, type/server job indexes, type/server summaries, state hash | {serverType} | Colocated; one Lua script performs each lifecycle mutation. |
| Heartbeat refresh | Heartbeat key, live set, state hash | {serverType} | Colocated; refresh and ACTIVE check are atomic. |
| Expiry fence | Heartbeat, state, server job index/summary, live set, retired ZSET | {serverType} | Colocated; heartbeat absence and ACTIVE-to-DELETING transition are atomic. |
| Phase A cleanup | State, heartbeat, indexes, summaries, job hashes | {serverType} | Colocated; preflight and cleanup occur in one Lua invocation. |
| Registry insertion | Global type set and per-type registry set | {registry} | Colocated with each other; atomic registry update. |
| Registration state check | Per-type state hash | {serverType} | Separate C++ read before registry insertion; cannot be atomic with the registry script. |
| Registration metadata and activation | Type summary, server summary, state hash | {serverType} | Each operation is slot-compatible, but the current implementation uses separate Lua invocations. Same slot does not make those invocations one atomic operation. |
| Phase B | Per-type registry set | {registry} | Separate from Phase A by necessity; idempotent SREM. |
| Phase C | Registry membership check, then per-type tombstone collection | First {registry}, then {serverType} | Cannot be one cross-slot script. The retained DELETING tombstone is the guard against premature reuse. |
| getServerSummaries() | Registry SMEMBERS, then per-type summary hashes | First {registry}, then {serverType} | Two-stage best-effort read, not a global point-in-time snapshot. |
| getServerTypeSummary() | One type summary hash | {serverType} | Independent of registry membership. |
| getServerTypes() | Global type set | {registry} | Registry catalogue read only. |

The current key construction consistently uses the intended hash tags for the reviewed scripts. The code passes all multi-key Lua keys through KEYS; it does not rely on Lua to discover undeclared keys dynamically.

## 2. Finding: registration has recoverable and unrecoverable partial states

**Severity: High — correctness of registration/discovery and cleanup; not direct counter corruption.**

### Current sequence

In RedisJobStatus::registerServer(), the sequence is:

1. Read the server ID's field from servers:state:{serverType} in C++.
2. Run kRegisterServerScript in the {registry} slot. It adds the type to servers:{registry}:types and the ID to servers:{registry}:type:<serverType>.
3. Run kInitializeTypeSummaryMetadataScript in the {serverType} slot.
4. Run kSetServerSummaryMetadataScript in the {serverType} slot.
5. Run kActivateServerScript in the {serverType} slot. This writes meta:registeredAt and the ACTIVE state in one script.

The registry script is atomic for the two registry keys. The activation script is atomic for the timestamp and ACTIVE state. However, steps 3, 4, and 5 are separate Redis operations. The fact that they all target the same slot does not make the complete sequence atomic.

### Failure windows

| Failure point | Redis state left behind | Current cleanup behavior |
|---|---|---|
| After step 2, before step 3 | Registry ID; no per-type state or summary metadata | Fence sees no ACTIVE/DELETING state and returns 0. Registry ID remains indefinitely. |
| After step 3, before step 4 | Registry ID and type-summary metadata; no server state | Fence returns 0. Type metadata can claim this failed ID as meta:createdByServerId. |
| After step 4, before step 5 | Registry ID and possibly a metadata-only server summary; no server state | Fence returns 0. Registry ID and partial metadata remain. getServerSummaries() can return the metadata-only server row if the server-summary hash exists. |
| After step 5 | Registry ID, ACTIVE state, registration timestamp, metadata | Normal first-heartbeat grace and heartbeat-expiry cleanup applies. |

The key implementation detail is in kFenceExpiredServerScript: after checking heartbeat absence and reading the state field, it returns 0 unless the state is ACTIVE or DELETING. There is no branch for a registry member with no state field. The scanner therefore does not clean up any of the first three shapes.

### Getter impact

The getter behavior was verified in the C++ source:

- getServerTypeSummary() calls readCounters() on summary:type:{serverType}. It does not read either registry set. A registry-only orphan does not affect the returned counters.
- getServerSummaries() first calls SMEMBERS on servers:{registry}:type:<serverType>, then runs kReadServerSummariesSnapshotScript in the per-type slot. The Lua script checks each candidate summary key and only emits it if the key is a hash. Missing hashes are skipped.
- If step 4 created a server-summary hash before the crash, that hash is present and the getter includes it. Its metadata fields are filtered from the returned counters, so it can appear as a server row with an empty counter map.
- getServerTypes() reads servers:{registry}:types. The type remains listed even if its only registration attempt failed.

**Conclusion:** the registry-only crash case is harmless to type counters and is not visible in the server-summary list. Partial registration after metadata writes can leave misleading metadata or a metadata-only server row. All these incomplete-registration shapes are currently permanent unless manually reconciled.

### Recommendation

Treat incomplete registration as its own recovery case, separate from the existing ACTIVE/DELETING cleanup lifecycle. At minimum, document that the current scanner does not repair it. If automatic cleanup is desired, it must distinguish a genuinely abandoned registration from a slow/in-progress one and account for partial metadata. A simple state-absent check is not sufficient to claim that no per-type artifacts exist.

The stated operational rule that a server ID is not reused makes conditional cleanup much simpler; it avoids needing an identity token to protect a newer incarnation. The no-reuse rule should remain explicit. A process paused beyond a grace period and later resumed is a separate failure model: a timestamp alone does not fence such a process.

## 3. Finding: registration's cross-slot check-and-insert is not atomic

**Severity: Medium — depends on concurrent attempts using the same ID.**

Step 1 reads the per-type state hash; step 2 inserts registry membership in a different slot. No Redis Cluster Lua script can atomically perform both.

The registry Lua script does perform a duplicate-membership check and SADD in one {registry}-slot operation. Thus two concurrent attempts with the same ID cannot both successfully add the same registry member: the first adds it and the second observes membership and returns 0. The preceding state read is still not part of that atomic decision.

The practical failure is most apparent after an interrupted registration: the state hash is absent but the registry member remains, so a retry with that same ID is rejected by the registry script. Under the non-reusable-ID rule, retrying with the same ID is not a supported recovery path anyway; the stale membership still remains a cleanup concern.

No per-type claim or registration token is currently used. Under the stated unique, non-reused process-ID contract, a token is not required merely to prevent a different process incarnation from inheriting the ID. It would be a separate design requirement if same-ID retries/reuse were later allowed.

## 4. Finding: the type registry is an ever-seen catalogue

**Severity: Low / API semantics.**

kRegisterServerScript adds a server type to servers:{registry}:types. No reviewed cleanup path removes a type from that set when its last server is retired. getServerTypes() returns this set directly.

Therefore, its semantics are “types that have been registered at least once,” not “types that currently have a registered or active server.” This is not necessarily wrong: it may be an intentional persistent catalogue. But callers must not interpret it as a live-server list. A type can remain listed after every server ID has been removed, and an incomplete first registration can add a type that never becomes active.

The type summary hash is also retained after workers retire; this preserves historical metadata and zero counters. The getter accepts an explicit type and reads that materialized summary independently of the type registry.

## 5. Finding: summary reads are not a cross-slot snapshot

**Severity: Low / expected best-effort semantics.**

getServerSummaries() enumerates server IDs from the registry slot, then reads summary hashes in the per-type slot. A server may be added or removed between those steps. The Lua script gives a consistent read of the supplied summary hashes during its execution, but it cannot include IDs added after the earlier SMEMBERS and it omits hashes removed before the Lua read.

This is accurately described as best-effort rather than point-in-time global consistency. The getter does not re-check registry membership inside the per-type script, which it cannot do across slots. The current code's missing-hash behavior is appropriate for normal concurrent cleanup.

The job retrieval APIs have a similar two-stage pattern within one per-type slot: C++ enumerates the job index, then Lua rechecks membership and reads candidate hashes. Their documentation correctly describes the result as best-effort.

## 6. Finding: cleanup lease is coordination, not a lock

**Severity: Medium operational caveat; existing documentation is substantially accurate.**

The lease lives in the registry slot. Renewal is synchronous and occurs at checkpoints in scanOnce(); it is not a separate renewal thread. A slow Redis command or a large batch can outlast the lease. Another manager may then acquire it and overlap the scan.

The critical per-server scripts provide the real safety boundary:

- Heartbeat refresh and fencing serialize in the per-type slot and check the same ACTIVE state.
- Once fencing changes ACTIVE to DELETING, job lifecycle scripts reject further mutations.
- Cleanup re-reads the server job index and validates candidate count/membership, ownership, status, and counters before mutation.
- Cleanup recognizes the already-cleaned shape and avoids a second counter decrement.
- Phase B SREM is idempotent.

This supports the claim that overlapping scanners should not double-delete jobs or double-decrement counters when all mutations use the API scripts. The lease does not stop a stale scanner from continuing an already-started operation after losing the lease; it only detects lease loss at the next renewal checkpoint.

## 7. Finding: per-server cleanup and retrieval work is unbounded

**Severity: Medium scalability / Redis latency risk.**

The cleanup scanner reads all references from one server's job set, constructs a Lua key list containing every job hash, and invokes one cleanup script for the whole server. The cleanup script loops through all jobs and performs all deletions/counter updates in one atomic execution.

Similarly, getServerJobs() and getServerTypeJobs() enumerate the full index and pass every candidate job hash to one Lua call. The retrieval README explicitly notes this limitation; cleanup has the same general concern.

For a server with a very large job backlog, the client request, KEYS/ARGV lists, Lua runtime, and Redis blocking time can become large. Long operations also increase the chance that the scan lease expires before the next renewal checkpoint. This is not a cross-slot bug, but it is an operational bound that should be considered before scaling to large per-server job counts.

A future bounded cleanup design needs to preserve the fence-first rule and ensure counters/indexes remain correct across batches. It should not simply split the current cleanup script into independent arbitrary chunks without a durable cleanup progress protocol.

## 8. Finding: first-heartbeat fencing depends on a registration timestamp

**Severity: Medium for legacy/corrupt records; low for new registrations.**

For a server that has never heartbeated, kFenceExpiredServerScript reads meta:registeredAt from the server summary, falling back to the legacy registeredAt:<serverId> field in the state hash. If neither timestamp is available, it returns 0 and the ACTIVE server is not fenced by the first-heartbeat timeout.

New registrations are protected because kActivateServerScript writes meta:registeredAt and ACTIVE in the same per-type Lua invocation. The README already documents the legacy-record limitation. It remains an important operational point for upgrades or manually altered Redis data.

## 9. Other implementation/documentation consistency notes

1. **Type-summary creation timestamp uses the application clock.** registerServer() builds meta:createdAt with C++ std::time / std::gmtime; activation and cleanup timestamps use Redis TIME. This is descriptive metadata, not a counter or lifecycle decision, but values can reflect host clock skew.
2. **The server-ID contract is operational.** The README says IDs are type-prefixed, globally unique, and fresh per process incarnation. The reviewed validateIdentifier() checks non-emptiness (and rejects braces in serverType) but does not itself enforce a server-ID prefix or global uniqueness. The host/application configuration must uphold that contract.
3. **Lua atomicity is not rollback.** The scripts generally preflight types, ownership, indexes, and counters before writing, and the README/LUA.md correctly call out that a runtime error after writes does not roll them back. This remains a general failure-mode limitation.
4. **Counter relationships are not globally reconciled on every write.** Scripts validate individual counters and prevent underflow/overflow for the fields they mutate. They do not independently prove that total == pending + active + suspended on every operation. External mutation or prior corruption can therefore remain visible unless a separate audit/reconciliation is added.
5. **A source comment is stale.** The comment above kCreatePendingScript describes KEYS[1..5], but the script and C++ caller use KEYS[1..6], with the per-type state hash at KEYS[6]. This does not change runtime behavior but should be corrected to avoid misleading future maintenance.

## 10. Overall assessment and suggested priorities

| Priority | Item | Suggested action |
|---|---|---|
| 1 | Incomplete registration is not reconciled | Decide whether permanent registry/metadata orphans are acceptable. If not, design a distinct incomplete-registration cleanup path; do not treat state absence alone as proof that no per-type metadata exists. |
| 2 | Registration is several independent operations | Preserve the distinction between same-slot placement and single-script atomicity. Consider combining per-type metadata writes and activation in one Lua invocation if eliminating partial per-type metadata is valuable. Registry insertion will still remain a separate cross-slot step. |
| 3 | Type discovery semantics | Clarify in API documentation that getServerTypes() returns an ever-seen type catalogue, not currently active types. |
| 4 | Unbounded cleanup/read batches | Establish expected maximum jobs per server and benchmark; add a bounded/progress-tracked design if large backlogs are possible. |
| 5 | Legacy timestamp and identity assumptions | Keep migration checks and the fresh, non-reused process-ID contract explicit. |
| 6 | Minor documentation/robustness items | Correct the create-script KEYS comment; consider stronger validation of metadata Lua arguments and server-ID format at the API boundary. |

### Bottom line

The main per-type job lifecycle and heartbeat/fencing operations are correctly colocated and use per-type Lua scripts for their critical atomic decisions. The cross-slot protocol is unavoidable for registry operations and is appropriately split into phases for normal expired-server cleanup.

The important uncovered gap is registration's inverse failure path: **registry insertion happens before per-type metadata and activation, but the cleanup scanner only knows how to fence an ACTIVE server or retry a DELETING tombstone. It does not handle a registry member whose per-type state is absent.** This leaves permanent orphan membership and, depending on exactly where registration stopped, partial metadata. It does not by itself corrupt the type counters, and the getters' actual behavior is described above.

No source files or tests were changed as part of this review.
