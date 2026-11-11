> **Architecture status (2026-11-11):** This document contains the earlier global-primary proposal and is retained as design history. It is superseded by [README.md](README.md), which records the selected Option 2 architecture: one independently elected cleanup manager per server type. Do not implement the global-primary election described below. The candidate selection, balancing, failure analysis, and rebalancing discussion below are retained for reference and must be interpreted through the Option 2 design in README.md.

---

# Dual-Role Worker and Cleanup-Manager Architecture

**Status:** Architecture proposal and implementation plan only. No source-code changes are included.

**Repository target:** mikegithub99/test2, root-level dual-role/DUAL_ROLE.md.

## 1. Executive summary

The existing application-server fleet can also provide the cleanup-manager primary/standby cluster. A separate fleet of dedicated manager machines is not required.

Every application process has two independent roles:

1. **Worker role:** registers its own process identity, publishes its own heartbeat, and processes its own jobs.
2. **Cleanup-manager role:** is eligible to scan all registered workers and perform expired-worker cleanup, but only while it holds cleanup leadership.

All 100 application processes continue their worker responsibilities. Normally, exactly one eligible process runs the global cleanup scanner. The other 99 are standby candidates and must not scan.

A process restart creates a new worker incarnation. The restarted process receives a new globally unique serverId and is a new manager candidate; it does not inherit the old process's leadership or fencing generation.

The most important design caveat is Redis Cluster slot locality: the global registry lease is in the {registry} slot, while cleanup scripts operate in individual {serverType} slots. A global lease token cannot simply be checked atomically by the existing per-type Lua scripts. The implementation must explicitly choose its fencing guarantee rather than claim strict stale-primary protection that the current slot layout cannot provide.



## 1.1 Architecture decision: preserve worker fencing and add manager fencing

**Decision for the implementation baseline:** Preserve the existing per-worker lifecycle fencing and add per-type manager fencing. The two mechanisms are complementary and must both be enforced by the cleanup mutation scripts.

This decision supersedes the earlier open choice between Target A and Target B in Section 11. Target A remains useful as historical context, but it is not the selected target for this implementation.

### Assumptions

1. There is one globally elected cleanup-manager primary at a time in normal operation. Standby application processes continue their own worker duties but do not scan.
2. The global election/coordination lease remains in the {registry} slot.
3. Each server type has manager authority represented in that type's {serverType} slot.
4. Every cleanup mutation for a type is executed through Lua that checks the caller's current per-type manager authority in the same slot as the mutation.
5. Existing worker lifecycle fencing remains authoritative for deciding whether an individual worker may be retired and cleaned.
6. The global lease and per-type authority are separate layers. Acquiring the global lease alone does not authorize cleanup mutations.
7. A new global primary is not considered ready to clean a given type until it has successfully established newer authority in that type's slot.
8. A process that cannot positively establish or renew the required authority fails closed and does not start or continue cleanup mutations for that type.

### Expectations

- All application processes continue worker registration, heartbeat publishing, and job processing independently of manager leadership.
- Only the global primary normally schedules cleanup scans.
- Standbys do not scan.
- A primary takeover includes establishing per-type authority before scanning those types.
- An old primary may still hold a valid per-type authority during a bounded handoff interval after losing the global lease. The new primary must not treat global election acquisition alone as completed takeover for that type.
- Once a newer per-type authority is established, every cleanup mutation script rejects the old generation.
- Manager fencing does not replace, weaken, or bypass worker fencing.
- Cleanup scripts retain their existing validation, idempotency, counter/index updates, and tombstone checks.

### Core invariants

1. **Worker identity invariant:** serverId is globally unique per process incarnation and is never reused.
2. **Worker fencing invariant:** no job create/transition/complete operation may succeed after the worker's lifecycle state has been fenced to DELETING.
3. **Manager authority invariant:** a cleanup mutation is accepted only when the supplied manager owner and generation match the current, unexpired authority record for that server type.
4. **Same-slot invariant:** the manager authority check and the cleanup mutation occur in one Lua script and one {serverType} slot.
5. **Monotonic generation invariant:** a type's manager generation strictly increases whenever authority is transferred; it is durable and is not reset when a lease expires.
6. **No stale reactivation invariant:** an old manager cannot recreate, renew, or reinstate its expired authority or overwrite a newer generation.
7. **Takeover readiness invariant:** global leadership acquisition is not sufficient to begin cleanup. The new manager must first establish per-type authority for each type it intends to scan.
8. **Fail-closed invariant:** uncertain acquisition/renewal or mismatched/expired authority means no new cleanup mutation for that type.
9. **Mutation coverage invariant:** every manager-owned Redis mutation in the cleanup path must be covered, not just the main job-deletion script.
10. **Worker/manager separation invariant:** manager authority does not grant permission to modify a worker's lifecycle if worker-level eligibility checks fail.

### Predicable outcomes

| Event | Required outcome |
|---|---|
| Normal operation | One global primary scans; standby processes continue worker duties and do not scan. |
| Standby wins global election | It prepares per-type authority; it scans a type only after that type's authority is established. |
| Old primary resumes after newer type authority is established | Its cleanup Lua mutations for that type are rejected by generation/owner checks. |
| Old primary loses global lease but newer per-type authority is not yet established | The old type authority may remain valid until its own expiry or revocation; the new primary must not yet scan that type. This is a bounded handoff interval, not completed takeover. |
| Old manager's per-type authority expires | Its subsequent cleanup mutations are rejected, even if no replacement has yet acquired that type. |
| Redis response is ambiguous | The manager treats authority as uncertain and does not initiate mutations until it positively revalidates. |
| A type is newly discovered after global takeover | The manager must establish authority in that type slot before scanning it. |
| A process restarts | It receives a new serverId and must acquire fresh global and per-type authority; it inherits neither old identity nor generation. |
| Worker is already DELETING | Manager authority alone does not change worker eligibility; existing worker cleanup validation applies. |

### Required per-type authority protocol properties

The exact key schema and Lua API are implementation decisions, but the protocol must provide these properties:

- A slot-local authority record containing the current manager owner and generation, with an expiry/lease mechanism or equivalent atomic revocation semantics.
- A durable per-type monotonic generation allocator in the same type slot.
- Atomic per-type acquire/transfer: the new owner can establish a strictly newer generation only when the prior authority is no longer valid under the protocol.
- Atomic per-type renewal: only the exact current owner and generation can renew; renewal must not resurrect an expired authority.
- Atomic per-type release: only the exact current owner and generation can release.
- Every cleanup mutation checks owner, generation, and authority validity inside its Lua script.
- The new primary waits for per-type authority acquisition before scheduling cleanup for that type.
- A global lease loss stops new scheduling and causes the manager to relinquish or allow its per-type authorities to expire according to the specified handoff protocol.
- Per-type authority TTL, global lease TTL, renewal cadence, and takeover ordering are specified together and tested under pauses, network partitions, and ambiguous Redis responses.

A possible safe handoff model is that the former manager's slot-local authority expires independently; the new manager cannot establish its newer generation for that type until the old authority is no longer valid. The new manager then atomically advances the durable generation and acquires the type authority. This deliberately means global election and per-type readiness are separate events. The implementation must validate this protocol against Redis Cluster and all failure cases before coding.


## 2. Goals

- Separate local worker heartbeat publishing from global cleanup scanning.
- Allow every eligible application process to become cleanup leader.
- Normally have one active global cleanup scanner.
- Automatically promote a standby after primary failure or lease expiry.
- Keep worker registration, heartbeat, and job processing independent of manager leadership.
- Ensure a restarted process cannot inherit the previous process's worker or manager identity.
- Preserve existing worker lifecycle fencing, job accounting, tombstones, and cleanup validation.
- Respect Redis Cluster hash-slot boundaries.
- Provide observable and testable leadership, failover, and recovery behavior.

## 3. Non-goals

- Replacing Redis with a relational database.
- Changing the worker job lifecycle model.
- Reusing a serverId after process restart.
- Making the manager publish heartbeats on behalf of workers.
- Treating a lease by itself as a strict fencing mechanism.
- Claiming global atomic fencing across Redis Cluster slots without an additional design.
- Modifying the baseline 2-colocated implementation.

## 4. Preconditions and assumptions

These are deployment and architecture requirements. They must be verified rather than silently assumed.

### 4.1 Worker identity assumptions

1. Every process incarnation receives a new serverId.
2. The ID is prefixed with its server type as required by the current key layout.
3. The ID is globally unique across all process incarnations.
4. An ID is never reused for another process, including after tombstone collection.
5. A restarted process does not reclaim the previous process's ID.
6. The current process's serverId is available before manager election begins.

The serverId is the worker/process identity. It may also be used as the manager lease owner identity, but it is not the manager fencing token.

### 4.2 Redis assumptions

1. All candidates connect to the same logical Redis deployment and key namespace.
2. Existing Redis Cluster hash tags are preserved.
3. Manager election and lease operations are atomic and use explicit keys.
4. Lua scripts do not discover keys dynamically.
5. A manager that cannot prove continued leadership fails closed and stops initiating cleanup work.
6. Redis outage and failover behavior is included in testing and operational policy.

### 4.3 Process/deployment assumptions

1. Each process has one stable local process identity for its lifetime.
2. Worker identity registration and heartbeat publishing do not require manager leadership.
3. A process may process jobs while acting as cleanup leader.
4. There may be a short interval with no cleanup leader during lease expiration and takeover.
5. No specific host is assumed to restart first.
6. If preferred candidates are configured, other candidates remain eligible as fallback.

### 4.4 Configuration assumptions

Configuration should explicitly cover:

- Whether local worker heartbeat publishing is enabled.
- Whether this process is eligible to become a cleanup manager.
- Whether cleanup scanning is enabled in this process.
- Manager lease TTL.
- Manager lease renewal interval.
- Standby acquisition retry interval and backoff.
- Cleanup scan interval.
- Graceful shutdown/release timeout.
- Optional candidate priority.

All durations must be positive and validated. Renewal must occur sufficiently before expiry to tolerate expected scheduler and Redis latency. Redis-disabled mode must preserve the existing no-op behavior and must not initialize manager Redis infrastructure unnecessarily.

## 5. Existing architecture facts to preserve

- Global registry keys use the {registry} hash tag.
- Per-type lifecycle, job, counter, heartbeat, and cleanup keys use the {serverType} hash tag.
- The heartbeat key is scoped to a server type and serverId.
- Lifecycle state is stored per type and transitions between ACTIVE and DELETING.
- Existing create/transition/complete scripts check worker lifecycle state in the same type slot.
- Per-server cleanup validates the worker state, heartbeat absence, job references, job ownership/status, indexes, and counter sufficiency before applying cleanup.
- Cleanup is designed to be idempotent for already-cleaned workers.
- Existing cleanup lease is per scan and released at the end of the scan; it is not persistent primary/standby leadership.
- Existing renewal occurs at scan checkpoints, not on an independent background control loop.
- Each Redis-enabled RedisHeartbeatManager currently starts a cleanup thread during construction, independent of HeartbeatStart().
- Lua atomicity does not imply rollback of earlier writes if a script encounters a runtime error after mutation.

These facts constrain the design and must be checked against current source before implementation.

## 6. Separation of responsibilities

### 6.1 Worker responsibilities — every application process

Every process continues to:

- Initialize its own RedisJobStatus identity.
- Register its own unique serverId.
- Publish its own heartbeat.
- Create, transition, and complete its own jobs.
- Maintain its own job summaries and counters through existing APIs.
- Stop its own heartbeat publisher as part of worker shutdown.
- Never publish a heartbeat for another worker.

These operations must work whether the process is primary, standby, or manager-ineligible.

### 6.2 Manager election responsibilities — every eligible candidate

Every eligible process may:

- Attempt atomic leadership acquisition.
- Renew leadership while it owns it.
- Detect loss or uncertainty of leadership.
- Stop initiating cleanup work when leadership cannot be confirmed.
- Release leadership on graceful shutdown, only if it still owns it.
- Retry acquisition as a standby.
- Expose manager state and ownership diagnostics.

A standby must not run cleanup scans just because its worker heartbeat is active.

### 6.3 Cleanup responsibilities — active leader only

The active cleanup manager performs:

- Registry enumeration of server types and server IDs.
- Heartbeat-expiry and first-heartbeat-timeout detection.
- Existing per-server lifecycle fencing.
- Reading server job references.
- Existing atomic per-server job cleanup.
- Registry membership removal where appropriate.
- Tombstone backfill and retired tombstone collection.
- Scan progress, failure, and duration reporting.

Manager leadership does not replace worker lifecycle fencing.

## 7. Identity model: keep three concepts distinct

### 7.1 Worker/process identity: serverId

Example:

- First incarnation on host A: typeA-hostA-001.
- That process crashes.
- Restarted process: typeA-hostA-002.

The restarted process is a new worker. It registers and publishes heartbeat under its new ID. It does not inherit the old process's worker lifecycle state or job ownership.

### 7.2 Manager owner identity

The manager owner field identifies the process incarnation currently holding leadership. It should normally be the current process's serverId.

It is used for ownership diagnostics and compare-owner renewal/release. A restarted process has a different owner identity because it has a different serverId.

### 7.3 Manager fencing generation/token

A manager fencing token is a separate monotonically increasing leadership generation.

| Leadership event | Owner serverId | Fencing generation |
|---|---|---:|
| First primary | A-001 | 41 |
| Standby takeover | B-001 | 42 |
| A restarts and later takes over | A-002 | 43 |

The token distinguishes leadership generations, even if a host or deployment slot is reused. It must not be derived from serverId, a timestamp, random UUID, or process-local counter.

The token allocator must be durable and monotonic for its leadership domain. It must not reset when the lease expires.

**Invariant:** serverId prevents worker-incarnation identity collisions; manager fencing generation distinguishes leadership generations and rejects stale manager authority.

## 8. Manager state machine

Each process should maintain an explicit local manager state:

- DISABLED: manager feature or eligibility is disabled.
- STANDBY: eligible but does not own leadership.
- ACQUIRING: attempting atomic acquisition.
- LEADER: Redis has confirmed leadership.
- LEADER_DEGRADED: renewal/Redis result is uncertain; no new cleanup work may start.
- STOPPING: shutdown is in progress.

Expected transitions:

1. Startup -> DISABLED if manager functionality is disabled.
2. Startup -> STANDBY if eligible.
3. STANDBY -> ACQUIRING on acquisition attempt.
4. ACQUIRING -> LEADER only after Redis confirms acquisition and returns the generation.
5. ACQUIRING -> STANDBY if another owner holds leadership.
6. LEADER -> LEADER_DEGRADED on failed or uncertain renewal.
7. LEADER_DEGRADED -> LEADER only after positive ownership revalidation before the local safety deadline; otherwise relinquish leadership.
8. Any active state -> STOPPING during shutdown.
9. A process with an expired or mismatched lease must return to STANDBY and must not use its old token.

A local boolean isLeader is only cached process state, not proof of Redis ownership.

## 9. Primary/standby handoff scenarios

### 9.1 Normal operation

- All 100 processes register and publish their own heartbeats.
- Eligible processes participate in standby acquisition attempts.
- One process owns cleanup leadership.
- Only that process runs cleanup scans.
- The leader renews independently of scan duration and between scans.

### 9.2 Primary crashes

- The primary stops renewing its lease.
- Standbys continue worker work and periodically attempt acquisition.
- Once Redis considers the lease expired, one standby atomically acquires leadership.
- The new manager receives a new fencing generation.
- The new manager starts scanning.
- The crashed worker's own heartbeat expires and its worker lifecycle cleanup proceeds through existing fencing.

### 9.3 Primary pauses and later resumes

This is more dangerous than a clean crash.

- The old process may pause due to scheduler stall, VM pause, network partition, or process suspension.
- Its lease may expire while paused.
- A standby may acquire leadership.
- The old process may later resume with stale local state.

Required behavior:

- Renewal compares exact owner and generation.
- Renewal must not recreate an expired lease.
- Release compares exact owner and generation.
- Renewal failure or uncertainty causes the old manager to stop scheduling cleanup.
- Strict rejection of stale mutations requires slot-local atomic fencing; a C++ lease check immediately before Lua is not sufficient to close the race.

### 9.4 Original primary restarts

Suppose A-001 was primary, B-001 took over, and host A restarts as A-002.

- A-002 registers as a new worker with a new serverId.
- A-002 starts as a standby candidate.
- It does not reuse A-001's lease owner identity.
- It does not reuse A-001's fencing generation.
- It cannot reclaim leadership because it was the original host.
- It must win the same atomic acquisition process as every other standby.
- If it later becomes leader, it receives a newer fencing generation.

### 9.5 Graceful primary shutdown

- Stop scheduling new scans.
- Let in-progress work reach a safe boundary or stop according to the selected cleanup design.
- Release leadership atomically only if owner and generation still match.
- Stop renewal/acquisition loops.
- Do not delete a lease owned by another process.
- Worker heartbeat shutdown remains a separate worker lifecycle decision.

### 9.6 Redis outage

- A manager unable to confirm renewal must fail closed.
- It must not assume leadership remains valid because a previous renewal succeeded.
- Standbys must acquire through Redis atomic operations, not local-time guesses.
- After Redis recovery, acquisition retries resume under the same ownership protocol.

## 10. Lease protocol requirements

The current per-scan lease must evolve if it is to represent persistent primary/standby leadership.

Required properties:

1. Acquire leadership once and retain it across scans.
2. Renew on an independent manager-control loop, including while idle between scans.
3. Run cleanup scans on a separate loop/scheduler.
4. Have standbys retry acquisition periodically with jitter/backoff.
5. Compare owner and generation during renewal.
6. Compare owner and generation during release.
7. Never allow renewal to recreate an expired lease.
8. On failed or uncertain renewal, stop scheduling cleanup and transition out of LEADER.
9. Avoid holding a process mutex over long Redis calls if that blocks shutdown or state transitions.
10. Make scan authorization depend on current manager state.

A short TTL improves failover speed but increases false failovers during transient delays. A long TTL slows takeover. Choose values from measured process pauses, Redis latency, and the operational failover objective.

### 10.1 Candidate selection

The simplest policy is for all eligible candidates to contend using an atomic acquisition operation. Redis determines the winner.

Optional priority tiers can prefer selected candidates, but must not allow an unavailable preferred candidate to block all fallback candidates indefinitely.

Never implement acquisition as a non-atomic "check absent, then set" sequence.

## 11. Redis Cluster fencing: selected design

**Selected target: global primary/standby election plus per-type manager fencing, while preserving existing per-worker lifecycle fencing.**

### 11.1 Why the global token alone is insufficient

The global manager lease belongs in the {registry} slot. Cleanup scripts operate on keys in a {serverType} slot. Redis Cluster Lua cannot atomically check a key in {registry} and mutate keys in {serverType}.

Therefore, a global lease check in C++ immediately before a cleanup Lua call is not strict fencing. The manager can lose its global lease between the C++ check and the Lua mutation, or pause and resume after takeover.

### 11.2 Two fencing layers are required

**Worker fencing — existing and preserved**

- Existing worker lifecycle state remains in the type slot.
- Worker job create/transition/complete scripts continue checking ACTIVE state.
- Cleanup continues to fence ACTIVE -> DELETING and validate heartbeat, job references, job ownership/status, indexes, and counters.
- Existing idempotency and tombstone rules remain in force.

**Manager fencing — new and additive**

- Manager authority is represented separately for each type in that type's {serverType} slot.
- Every cleanup mutation script checks the current manager owner and generation in that same slot.
- A stale manager generation is rejected before the script performs cleanup mutations.
- Manager authority does not override worker lifecycle checks.

### 11.3 Global election and per-type readiness are distinct

The global lease elects one preferred cleanup manager. It does not, by itself, authorize mutations in any type slot.

After acquiring global leadership, the new manager must establish per-type authority for each type before scanning it. The system must define takeover as complete for a type only after the newer authority is committed in that type's slot.

This distinction is essential during handoff. Until the new generation is installed, the former manager's per-type authority may still be valid. The new manager must not scan that type during this interval. The protocol must bound this interval using slot-local authority expiry/revocation and must ensure an expired authority cannot be used or renewed.

### 11.4 Required per-type authority protocol

The exact key names and Lua script boundaries are implementation decisions. The protocol must guarantee:

1. A durable, monotonically increasing generation per server type.
2. A slot-local authority record identifying owner, generation, and validity/expiry.
3. Atomic acquire/transfer that cannot replace a still-valid owner and cannot install an older generation.
4. Renewal only by the exact current owner and generation.
5. Renewal cannot recreate an expired authority.
6. Release only by the exact current owner and generation.
7. Every manager-owned cleanup mutation validates owner, generation, and authority validity inside its Lua script.
8. The new global primary cannot scan a type until it has acquired that type's authority.
9. A former primary cannot regain authority using stale local state or an old generation.
10. Newly discovered types must go through the same authority-acquisition barrier before scanning.

A viable protocol may use a slot-local expiring authority lease plus a durable generation key. When the old slot-local lease expires, its owner is rejected by cleanup scripts; a successor can then atomically advance the generation and acquire authority. This is a design direction, not an approved implementation until its Redis Lua atomicity, expiry behavior, and failure cases are specified and tested.

### 11.5 Mutation coverage

Review every cleanup-path mutation, not only the primary job cleanup script. At minimum assess:

- Per-server lifecycle fencing and tombstone creation.
- Per-server job/index/counter/heartbeat/live-set cleanup.
- Tombstone backfill and registry/tombstone maintenance.
- Retired tombstone collection.
- Any repair, reset, or administrative operation that can mutate the same cleanup state.

Read-only registry enumeration need not necessarily carry type authority, but no mutation may bypass the manager-authority rule. Cross-slot registry operations must remain outside per-type Lua scripts and be treated as separate coordination steps.

### 11.6 Redis Cluster layout

Preserve the existing {registry} and {serverType} hash tags. New manager authority and generation keys for a type must use that type's existing hash tag so the authority check and type cleanup mutation can execute atomically.

Do not put the global registry lease key into a per-type Lua script. Do not assume asynchronous copying of a global epoch into type slots is sufficient.

### 11.7 Expected guarantee and limitation

After a newer generation has been established in a type slot, cleanup mutation scripts reject the former primary for that type. Before that per-type handoff is complete, the old slot-local authority may remain valid until its defined expiry/revocation point; the successor must not scan that type yet.

The architecture therefore provides strict stale-manager rejection **after per-type authority transfer**, not instant cross-slot revocation at the moment the global lease changes owner. If the requirement is instead that all type slots reject the old manager at the exact instant the global lease changes, Redis Cluster's cross-slot constraint requires a different key-slot architecture or another coordination design.

## 12. C++ component responsibilities and integration

The current RedisHeartbeatManager combines heartbeat publishing and cleanup scanning. Separate these lifecycles while preserving worker-facing APIs.

### 12.1 Worker heartbeat publisher

Responsibilities:

- Construct only when Redis and heartbeat publishing are enabled.
- Start/stop the local heartbeat runner.
- Publish only the local worker's heartbeat.
- Have no global cleanup scan thread.
- Continue operating when the process is standby.
- Preserve Redis-disabled no-op behavior.

### 12.2 Cleanup manager controller

Responsibilities:

- Determine process eligibility.
- Maintain election state.
- Acquire, renew, and release leadership.
- Track owner identity and fencing generation.
- Signal scanner start/stop on leadership changes.
- Prevent scan work when not leader.
- Expose manager state and metrics.

### 12.3 Cleanup scanner

Responsibilities:

- Run only when authorized by the controller.
- Recheck local authorization at documented checkpoints.
- Continue using existing per-server fencing and cleanup scripts.
- Stop scheduling new work on leadership loss.
- Report scan progress and errors.
- Not own heartbeat publishing or manager election.

The current scanOnce() method is public. The implementation must prevent a standby from bypassing role checks through direct calls. Make it private/internal where feasible, or guard it with explicit scanner authorization.

### 12.4 Worker API compatibility

Registration, job creation, transition, completion, and heartbeat APIs must not require manager leadership.

Callers should not need to know whether their process is primary or standby. Election should be internal to the manager component and observable through diagnostics.

### 12.5 Startup ordering

1. Initialize RedisJobStatus and the local worker identity.
2. If Redis is disabled, preserve no-op behavior and do not initialize manager Redis infrastructure.
3. Start local heartbeat publishing if enabled.
4. Initialize manager controller if this process is eligible.
5. Enter standby/acquisition loop.
6. Start cleanup scanning only after leadership is confirmed.

### 12.6 Shutdown ordering

1. Mark manager controller as stopping.
2. Stop scheduling new cleanup scans.
3. Signal active scan to stop at a safe checkpoint if supported.
4. Release leadership only if owner and generation still match.
5. Stop renewal/acquisition loops.
6. Stop local heartbeat publishing according to worker shutdown lifecycle.
7. Complete normal worker/job shutdown.

Stopping cleanup leadership must not accidentally stop a worker heartbeat while that worker is still expected to be alive.

## 13. Configuration model

Names are illustrative and should follow existing project conventions.

| Setting | Purpose |
|---|---|
| heartbeatPublishingEnabled | Enables local worker heartbeat publishing |
| cleanupManagerEligible | Allows this process to campaign for leadership |
| cleanupScanningEnabled | Enables scanner capability; leadership is still required |
| managerLeaseTtlMilliseconds | Leadership lease lifetime |
| managerLeaseRenewIntervalMilliseconds | Renewal cadence |
| managerStandbyRetryIntervalMilliseconds | Standby acquisition retry cadence |
| cleanupScanIntervalSeconds | Delay between scans while leader |
| managerCandidatePriority | Optional candidate preference |
| managerShutdownReleaseTimeoutMilliseconds | Graceful release budget |

Invariants:

- cleanupManagerEligible=false never disables worker registration or heartbeat.
- cleanupScanningEnabled=true never implies leadership.
- A process can be worker-enabled and manager-ineligible.
- Redis disabled bypasses manager and heartbeat Redis initialization as required by current behavior.
- Simultaneous acquisition by all 100 candidates is safe.

## 14. Observability

Per process, expose or log:

- Local serverId.
- Manager eligibility.
- Manager state.
- Current manager owner ID, if known.
- Current fencing generation, if applicable.
- Last successful renewal time and lease remaining time.
- Time since last successful scan.
- Current scan start time and duration.
- Number of types and servers examined.
- Number fenced, cleaned, already-cleaned, and tombstones collected.
- Scan exceptions and acquisition/renewal/release failures.

Deployment-wide, monitor:

- Current leader owner and generation.
- Number of standby candidates.
- Leadership changes and takeover duration.
- Periods with no leader.
- Concurrent scan observations.
- Cleanup duration and backlog.
- Redis latency/errors affecting renewal.
- Per-type server/job counts and reconciliation discrepancies.

Never log Redis credentials or other secrets.

## 15. Failure and race scenarios to design and test

1. Two or more standbys attempt acquisition simultaneously.
2. Primary crashes immediately after acquisition.
3. Primary crashes during a scan.
4. Primary pauses longer than lease TTL and later resumes.
5. Renewal request times out but may have reached Redis.
6. Release races with standby acquisition.
7. Primary loses Redis connectivity while workers remain active.
8. Redis failover changes lease/epoch visibility.
9. Standby starts before its worker registration is complete.
10. Original primary restarts with a new serverId.
11. Old worker heartbeat expires while the new incarnation is already active.
12. Manager role is disabled while heartbeat publishing remains enabled.
13. Scanner is invoked directly from a standby.
14. A blocking Redis call outlasts the lease TTL.
15. Leadership is lost between a checkpoint and a per-type Lua mutation.
16. Cleanup spans multiple Redis Cluster type slots.
17. Tombstone backfill and collection overlap.
18. Lua runtime error occurs after earlier writes; scripts do not provide rollback semantics.

For each race, tests must assert resulting lifecycle state and counter/index consistency.

## 16. Implementation phases

### Phase 0 — specify and review the selected dual-fencing protocol

Before coding:

- Preserve existing worker lifecycle fencing; do not replace it.
- Specify global primary/standby election separately from per-type authority acquisition.
- Define the per-type authority record, durable generation allocator, TTL, renewal, release, and takeover barrier.
- Define when takeover is considered complete for each type.
- Define behavior for a former manager during the interval between global lease loss and per-type authority expiry/transfer.
- Inventory every cleanup mutation and identify the Lua authority check that protects it.
- Define handling for newly discovered server types.
- Verify the globally unique, never-reused serverId guarantee.
- Review actual key builders and Lua KEYS lists for Cluster slot boundaries.
- Prove that no stale generation can renew, reacquire, or overwrite a newer generation.

**Exit criterion:** reviewed protocol specification and failure-state table, with no claim of instantaneous cross-slot revocation.

### Phase 1 — separate worker heartbeat and cleanup lifecycles

- Remove automatic cleanup-thread startup from every Redis-enabled worker.
- Keep local heartbeat publishing independently startable.
- Add explicit manager eligibility and scanner configuration.
- Make standby scanner invocation impossible or harmless.
- Preserve Redis-disabled no-op behavior.

**Exit criterion:** all workers can register and publish heartbeats while no cleanup scanner runs.

### Phase 2 — implement manager election

- Add atomic acquisition.
- Add independent renewal loop.
- Add compare-owner-and-generation release.
- Add standby retry/backoff.
- Add state transitions and metrics.
- Ensure renewal does not depend on scan progress.

**Exit criterion:** multiple processes elect one normal leader while all standbys remain worker-active.

### Phase 3 — integrate scanner authorization

- Start scans only after leadership confirmation.
- Stop scheduling work on leadership loss.
- Preserve existing per-server lifecycle fencing and cleanup scripts.
- Recheck authorization at documented checkpoints.
- Prevent public scanner calls from bypassing authorization.

**Exit criterion:** only the active manager normally scans; loss of leadership stops new scan work.

### Phase 4 — implement per-type manager fencing

- Add durable per-type monotonic generation and slot-local manager authority.
- Implement atomic acquire/transfer, renew, and release operations.
- Add manager owner/generation/validity checks to every cleanup mutation script.
- Keep existing worker lifecycle checks and cleanup validation intact.
- Enforce the per-type readiness barrier before scanning each type.
- Test old-manager pause/resume, lease expiry, ambiguous Redis results, and type-by-type takeover.
- Ensure a type is never scanned by the successor before its newer authority is established.

**Exit criterion:** tests prove that after per-type authority transfer, every stale-manager cleanup mutation is rejected, while existing worker-fencing behavior remains unchanged.

### Phase 5 — staged rollout

1. Deploy with manager election disabled; verify workers still heartbeat and process jobs.
2. Enable eligibility on a small subset.
3. Observe one leader and standby behavior.
4. Kill the leader and measure takeover.
5. Restart the old leader and verify it remains standby until it wins a new election.
6. Test pause/resume and Redis outage scenarios.
7. Expand eligibility to the full fleet.
8. Remove old per-process scanning only after metrics and tests validate the new manager.

Do not run the old all-worker scanners and new leader scanner concurrently during rollout without an explicit overlap plan.

## 17. Required automated tests

### Identity
- Restarted process receives a new serverId.
- New process cannot renew/release the previous incarnation's lease.
- Old worker cleanup cannot delete new worker keys or jobs.

### Election
- Simultaneous acquisition yields one owner.
- Only matching owner/generation can renew.
- Only matching owner/generation can release.
- Expired lease can be acquired by a standby.
- Renewal never resurrects an expired lease.
- Fencing generation strictly increases across leadership changes.

### Lifecycle separation
- Standby continues heartbeat publishing.
- Standby continues job processing.
- Manager loss does not stop worker heartbeat.
- Worker heartbeat stop does not implicitly transfer manager leadership except as part of process shutdown.
- Ineligible worker cannot scan.

### Failover
- Primary crash during idle period.
- Primary crash during scan.
- Primary pause beyond TTL and later resume.
- Original primary restarts with new serverId.
- Two standbys race to take over.
- Redis timeout during renewal and release.

### Cleanup and dual-fencing regression
- Existing per-server fence rejects job mutations after DELETING.
- Existing worker lifecycle checks remain present in all relevant job and cleanup scripts.
- A manager with the current type owner/generation can perform authorized cleanup.
- A former manager with an older generation is rejected by every cleanup mutation script after authority transfer.
- An expired type authority cannot be renewed or used.
- A new manager cannot scan a type before establishing its authority there.
- Type-by-type takeover remains safe when Redis calls time out or return ambiguous results.
- Type/server counters and indexes remain consistent.
- Tombstone backfill and collection are covered by manager authority.
- Multi-type cleanup respects Redis Cluster slots.
- Lua scripts use explicit keys and no cross-slot key access.
- A manager authority failure does not bypass or weaken worker lifecycle fencing.

### Load
- 100 processes publish heartbeats.
- 100 eligible candidates produce one normal scanner.
- Scan duration measured under realistic server/job counts.
- Renewal remains timely during slow scans and Redis latency.
- Failover time and cleanup backlog measured after primary failure.

Do not declare completion based only on source inspection. Run unit/integration tests against Redis and Redis Cluster tests for every multi-key Lua path.

## 18. Acceptance criteria

1. All application workers continue normal work without manager leadership.
2. Exactly one manager is the normal global scanner, unless a deliberately sharded per-type model is selected and documented.
3. Standbys do not run cleanup scans.
4. A standby takes over after lease expiry within the agreed failover objective.
5. A restarted former primary receives a new serverId and does not inherit old manager authority.
6. Renewal and release are owner/generation checked.
7. After newer per-type authority is established, a stale manager cannot execute any covered cleanup mutation for that type.
8. No successor scans a type before establishing its per-type authority.
9. Existing worker lifecycle and job accounting invariants remain intact.
10. All Lua scripts respect Redis Cluster slot constraints.
11. Metrics expose global leader, per-type authority owner/generation, standby, renewal, scan, and failover behavior.
12. Crash, pause/resume, per-type handoff, failover, and counter-consistency tests pass.
13. Documentation and implementation agree about the per-type handoff interval and stale-manager guarantee.

## 19. Final architecture

- Every application process is a worker for its own globally unique process-incarnation serverId.
- Every eligible worker is a cleanup-manager candidate.
- One candidate normally owns global cleanup leadership; standbys continue worker responsibilities and do not scan.
- Global leadership is coordination, not sufficient authority to mutate per-type cleanup data.
- The global primary must establish per-type manager authority before scanning each type.
- Each type's manager authority and durable generation live in that type's {serverType} slot.
- Every cleanup mutation validates current manager owner/generation/validity atomically in the same type slot.
- A former primary is rejected by cleanup mutation scripts after a newer generation is established for that type.
- During handoff, the successor does not scan a type until its newer authority is established; old slot-local authority may remain valid until its defined expiry/revocation point.
- Existing worker lifecycle fencing remains intact and is checked in addition to manager authority.
- Worker fencing protects against stale workers; manager fencing protects against stale cleanup managers.
- The two mechanisms are additive and solve different problems.
- Manager owner identity may be the current serverId, but manager generation is separate, durable, and monotonically increasing per type.

Implementation should first specify and test the per-type authority handoff protocol against Redis Cluster constraints. Then separate worker heartbeat publishing from cleanup scanning, implement global primary/standby election, add per-type manager authority, and integrate the scanner so it cannot mutate a type without both current manager authority and the existing worker lifecycle checks.
