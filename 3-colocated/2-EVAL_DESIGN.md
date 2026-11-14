# 3-colocated design evaluation

**Review scope:** source-level review of the 3-colocated implementation in the repository default branch as inspected on 2026-11-13.

**Purpose:** assess architectural soundness and production readiness, identify concrete risks, and define the work needed before production deployment.

## Executive assessment

The design has a strong foundation. It uses per-server-type Redis Cluster hash tags to colocate job state, indexes, summaries, heartbeats, and lifecycle state. Critical job transitions are implemented as Lua operations that validate ownership and accounting before mutation. Server cleanup is explicitly designed around the Redis Cluster slot boundary, with lifecycle fencing, idempotent cleanup, a registry-slot lease, and retained tombstones. The read APIs intentionally use best-effort semantics appropriate for a live monitor rather than imposing expensive point-in-time guarantees.

However, **the folder should not yet be considered production-ready**. The current source review found one likely build blocker, a recoverability gap in partial registration, configuration combinations that can undermine heartbeat/lease timing, and important test and operational-readiness gaps. The cleanup protocol is thoughtfully documented, but its safety claims still need executable fault-injection and Redis Cluster tests.

This is a source review, not a successful build, test run, formal verification, or production certification.

### Overall rating

| Area | Assessment |
|---|---|
| Architecture and key placement | Strong |
| Job lifecycle atomicity | Strong design; needs automated verification |
| Cleanup state machine | Thoughtful, complex, and test-sensitive |
| Redis Cluster discipline | Strong overall; requires real-cluster validation |
| Best-effort monitoring reads | Appropriate for the stated use case |
| Failure recovery | Partial-registration recovery needs improvement |
| Scalability | Acceptable for bounded workloads; unbounded operations need limits |
| Build and test evidence | Insufficient for production approval |
| Operational readiness | Needs metrics, alerts, and runbooks |

**Recommendation:** retain the architecture, but treat production deployment as conditional on resolving the blocking findings and completing the test plan in this document.

## What is designed well

### 1. Key layout follows the workload

The per-type hash tag groups job hashes, job indexes, summaries, heartbeat keys, live membership, server state, and retired-server indexes into one Redis Cluster slot. This makes the multi-key lifecycle and cleanup scripts possible without cross-slot Lua access. The separate registry hash tag makes the cross-type discovery boundary explicit.

The length-prefixed server-ID component in job references avoids ambiguity when identifiers contain separators. Server-type validation rejects braces, protecting the intended hash-tag layout.

### 2. Critical job mutations are atomic

Create, transition, and completion operations update the job hash, both job indexes, and the relevant counters in one Lua execution. The scripts check server lifecycle state and job ownership, validate index membership, and preflight counter values before writing.

The implementation also distinguishes expected business outcomes (such as not found, no-op, invalid transition, already exists, and server not active) from inconsistent Redis data. That is a useful API contract for callers.

Redis Lua is atomic with respect to command interleaving, but is not a rollback transaction. The preflight-first structure is therefore important and should be preserved.

### 3. Cleanup has a defensible state machine

The ACTIVE-to-DELETING fence serializes against heartbeat refresh and job lifecycle scripts in the same type slot. Cleanup revalidates the server state and job references before changing counters or deleting job data. Registry removal is separated because it belongs to another Cluster slot. Retained tombstones support retry and delayed ID reuse.

The distributed lease is correctly treated as scan coordination rather than the correctness boundary. Per-server state checks and idempotent scripts provide the stronger protection when scans overlap.

### 4. Read consistency matches the product requirement

The server-level and type-level job APIs are read interfaces, not part of the lifecycle write path. Enumerating candidate references in C++ and then filtering/reading them in Lua is a reasonable best-effort design for a live monitor. It deliberately allows a refresh to omit jobs added after enumeration and jobs removed before the Lua read.

This is not an event log and should not be presented as one. A UI may skip short-lived intermediate states between polls. That is an appropriate trade-off for operational visibility.

### 5. Configuration and startup intent are documented

The singleton configuration model, disabled mode, heartbeat startup grace period, two independent TTLs, and cleanup phases are described in useful detail. The startup example registers the server before starting the heartbeat publisher.

The documentation is unusually explicit about cross-slot limitations and known identity assumptions. That is a good basis for production hardening.

## Findings requiring action

### P0 — Likely build blocker: invalid standard-library include

**Location:** RedisClient.h

The header includes the spelling:

    #include <std::variant>

The standard C++ header is:

    #include <variant>

The current include is not the standard header name and is expected to fail on conventional C++17 toolchains. Because RedisClient.h is included by the other core headers, this can prevent the implementation from compiling.

**Action:** replace the include with <variant>, then compile every translation unit with the actual production compiler and redis-plus-plus/hiredis versions. Do not infer build success from source inspection.

### P1 — Partial registration can leave an unrecoverable registry entry

**Location:** RedisJobStatus::registerServer; kRegisterServerScript; kActivateServerScript; cleanup discovery in RedisHeartbeatManager::scanOnce.

Registration spans multiple operations and two Cluster slots:

1. Check the per-type state hash.
2. Insert the server ID into the registry slot.
3. Initialize type metadata.
4. Initialize server metadata.
5. Activate the server in the per-type slot.

The README correctly acknowledges that a crash after registry insertion can leave a registry member without an ACTIVE state. The normal cleanup scan enumerates registry members, but the fence script returns without fencing when the state is absent. Such an entry can therefore remain in the registry indefinitely and block another registration attempt with the same ID.

This is an availability and operational-recovery gap, not evidence that job counters are corrupted.

**Action:** add an explicit registration-reconciliation protocol. It should distinguish an in-progress registration from an orphaned registry member, use a bounded registration lease or timestamp/token, and safely remove or resume abandoned registrations. Add crash injection after every registration step. Document the operator recovery procedure until automated reconciliation exists.

### P1 — Heartbeat and lease timing relationships are not validated

**Location:** RedisHeartbeatManager constructor and Config.

The constructor checks that each duration is positive, but it does not enforce sensible relationships between them. In particular:

- Heartbeat refresh interval should be comfortably shorter than heartbeat TTL.
- Cleanup lease renewal interval should be comfortably shorter than cleanup lease TTL.
- The first-heartbeat grace period should accommodate real startup and registration latency.

If refresh interval is equal to or greater than heartbeat TTL, a healthy worker can appear dead between refreshes. If lease renewal interval is equal to or greater than lease TTL, scans can routinely lose the lease and overlap. The per-server scripts are intended to keep overlapping scans safe, but this creates avoidable work and complicates operations.

**Action:** validate these relationships at startup, with a documented safety margin rather than merely checking positivity. Add tests for rejected configurations and delayed Redis responses.

### P1 — No demonstrated automated test or build evidence

**Location:** Build and validation notes; repository test/build integration.

The README states that the code has not been compiled or tested against live Redis or Redis Cluster. The design depends on subtle properties: Redis Cluster routing, Lua argument ordering, Redis++ return-type behavior, races between heartbeat and fencing, lease expiry, and recovery across three cleanup phases.

A source-level review cannot establish those properties.

**Action:** add automated C++ tests and integration tests against both standalone Redis and an actual Redis Cluster. Make the tests part of CI. At minimum, test every lifecycle result, malformed key types, counter underflow/overflow, registration interruption, heartbeat/fence races, lease takeover, duplicate cleanup, Phase A/B/C interruption and retry, tombstone retention, and reads during concurrent job churn.

### P1 — Unbounded per-server cleanup and job reads can block a Redis slot

**Location:** RedisHeartbeatManager::scanOnce; kCleanupExpiredServerScript; getServerJobs(); getTypeJobs(); kReadJobsSnapshotScript.

Cleanup reads every server job reference, sends every corresponding job key to one Lua invocation, and processes all jobs in one script. Job retrieval similarly enumerates the entire index, sends every job key to Lua, and returns every job hash. These operations have no explicit item, byte, or execution-time limit.

Redis scripts execute without interleaving on the serving node. A very large server or type can therefore create a large request/reply, high memory use, and long periods during which other commands for that slot wait. A single unusually large server can also delay cleanup progress for other servers in that type slot.

**Action:** define supported maximums and measure realistic worst-case cardinality and metadata size. Add load tests at and above expected peaks. For monitoring, introduce bounded pagination or a continuation strategy if needed, while documenting that pages are not one point-in-time snapshot. For destructive cleanup, do not simply split the existing script into independent deletes: retain the DELETING fence and design a resumable, idempotent cleanup protocol with durable progress and correct counter accounting.

### P2 — Job lifecycle scripts do not verify the stored jobId field

**Location:** kTransitionScript and kCompleteScript in RedisJobScripts.inc.

The scripts verify stored serverType/serverId, status, and both index memberships. They receive the original job ID but do not compare it with the job hash's jobId field. The key is derived from the requested server ID and job ID, so normal API-created data is consistent; however, a manually corrupted or incorrectly populated hash can pass these ownership checks while containing a different jobId.

**Action:** compare HGET(jobKey, 'jobId') with the supplied job ID in both scripts, and make cleanup perform the same identity check against the reference/key identity where practical. Return the existing inconsistent-data result before any mutation.

### P2 — Job snapshot parser does not enforce all required fields

**Location:** parseJobsSnapshot in RedisJobStatus.cc.

The parser verifies serverType and, for the server-specific API, serverId. It does not explicitly reject missing or empty jobId/status/serverId fields, nor does it validate that status is one of the supported states. In the type-wide API, a missing serverId could be returned to the Java client as an apparently valid job.

**Action:** validate all API-owned fields and supported status values before constructing a Job. Treat malformed records as inconsistent data and fail the read with a clear diagnostic. Add parser tests for missing fields, unknown status, malformed field counts, and ownership mismatch.

### P2 — Main README has stale job-read consistency wording

**Location:** README.md, Dashboard read consistency.

The main README says that if cleanup changes the job index between reference discovery and the Lua snapshot, the read returns an error and callers may retry. The current kReadJobsSnapshotScript instead skips references that are no longer members and skips missing job hashes. That is the intended best-effort behavior documented in get-jobs/README.md.

The two documents currently describe different behavior for the same API.

**Action:** update the dashboard section in README.md to match the current best-effort semantics. State clearly that additions after enumeration are omitted, stale candidates are skipped, and the Lua filtering/hash read is atomic only for the supplied candidate list.

### P2 — Registration timestamps use std::gmtime

**Location:** RedisJobStatus::registerServer.

std::gmtime returns a pointer to shared static storage and is not guaranteed to be thread-safe. If multiple threads register servers in one process, concurrent calls can race while formatting the timestamp.

**Action:** use a thread-safe UTC conversion appropriate to the supported platforms (for example, gmtime_r or gmtime_s behind a small portability wrapper), or generate the creation timestamp using Redis TIME in Lua as is already done for the server registration timestamp.

### P2 — Runtime observability is too limited for production operations

**Location:** RedisHeartbeatManager and HeartbeatRunner.

The background threads log exceptions to stderr and retry on the next interval. There are no structured counters or health signals for heartbeat refresh failures, cleanup scan failures, lease acquisition/loss, servers fenced, cleanup retries, Phase B backlog, tombstone collection, or read latency/size.

This makes it difficult to distinguish a quiet system from a stalled cleanup manager or a Redis connectivity problem.

**Action:** expose structured logging and metrics, including last successful heartbeat, last successful scan, scan duration, lease acquisition/loss, per-phase outcomes, cleanup backlog, and job-read candidate/returned counts and latency. Add alerts for sustained heartbeat failures and old DELETING tombstones.

### P2 — Singleton initialization order is a strict integration contract

**Location:** RedisJobStatus::Instance; RedisClient::Instance; RedisHeartbeatManager::Instance; README startup instructions.

Each singleton permanently captures the configuration provider supplied on its first call. RedisHeartbeatManager's constructor calls RedisJobStatus::Instance() itself. If application code first reaches the manager before configuring the status singleton, the manager can cause the default provider to win; a later custom provider is ignored.

This is documented, but the API makes the ordering easy to get wrong.

**Action:** make initialization explicit in the host startup path and test the order. Consider replacing implicit first-call provider capture with an explicit application bootstrap/factory that constructs and retains the configured services before worker threads start.

## Additional design risks to manage

### Server identity must be unique per process incarnation

The cleanup protocol depends on a fresh, type-prefixed server ID for each process incarnation. Tombstone retention is finite; after Phase C, Redis permits the same ID to be registered again. A delayed old process that reuses that ID can no longer be distinguished from the new process by the stored identity alone.

The README already documents this correctly. Treat fresh process identity as a hard production requirement, not a recommendation. Prefer an unguessable incarnation component and ensure a restarted process cannot reuse an old ID.

### The cleanup lease is not a fencing token

The lease coordinates scan load, but it is not passed into per-server scripts and cannot stop a paused former lease owner from resuming. The current safety argument depends on lifecycle state, unique server IDs, and idempotent per-server scripts. Preserve that boundary. Do not make correctness depend on the lease being continuously held.

### Cross-slot workflows are intentionally not globally atomic

Registration and cleanup cross the registry/per-type slot boundary. Their phases are individually atomic and retryable, but the overall workflow is not a single transaction. Operational tooling must be able to identify and reconcile partial states, especially stale registry members and DELETING tombstones.

### Best-effort reads are not an audit trail

The current job APIs expose current state for live monitoring. Polling may miss intermediate states, and the current-state hash is deleted on completion. If historical duration, audit, or forensic requirements emerge, add a separate durable event/history mechanism rather than making the live read path pretend to provide event sourcing.

### Direct external writes to managed keys are unsupported

The invariants rely on all job and lifecycle changes going through the provided API and Lua scripts. Manual Redis writes, ad hoc scripts, or other clients that bypass the state checks can invalidate indexes and counters. Production access controls should restrict writes to the owning service, and administrative repair tooling should validate before mutating.

## Production-readiness checklist

Before production approval:

- [ ] Correct the standard-library include and build all translation units with the production toolchain.
- [ ] Add CI builds and automated C++ unit tests.
- [ ] Add standalone Redis integration tests.
- [ ] Add actual Redis Cluster integration tests covering hash-slot routing for every multi-key script.
- [ ] Implement or document a reliable partial-registration reconciliation procedure.
- [ ] Enforce heartbeat/TTL and lease-renewal/TTL configuration relationships.
- [ ] Add bounds/load tests for job count, metadata size, Lua duration, request size, and reply size.
- [ ] Add fault-injection tests for every registration and cleanup phase boundary.
- [ ] Add race tests for heartbeat versus fencing and job mutation versus fencing.
- [ ] Verify cleanup idempotence and counter invariants under overlapping managers.
- [ ] Tighten job hash identity and snapshot-field validation.
- [ ] Reconcile the main README's dashboard consistency description with the current best-effort implementation.
- [ ] Add structured logs, metrics, alerts, and operational recovery runbooks.
- [ ] Verify persistence, replication, backup, restore, failover, and data-loss objectives against the actual Redis deployment configuration.
- [ ] Run a rolling-upgrade test for legacy timestamps and tombstone backfill.
- [ ] Conduct a production-like soak test with realistic job churn and failure injection.

## Final conclusion

The architecture is coherent and well matched to a Redis-backed live job-status service. Its strongest properties are deliberate per-type colocation, atomic lifecycle mutations, explicit server fencing, retry-aware cleanup phases, and best-effort monitoring reads. The design is substantially more disciplined than a collection of independent Redis commands.

The main gap is not a need to replace Redis with SQL or to redesign the consistency model. It is the gap between a carefully reasoned source design and a verified production system. Fix the likely compile blocker, close the partial-registration recovery gap, enforce safe timing configuration, bound large operations, and establish automated standalone/Cluster failure testing. Until that evidence exists, classify the implementation as **promising and production-oriented, but not yet production-verified**.
