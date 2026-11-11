# Dedicated Redis Heartbeat Manager — Primary/Standby Design

**Status:** Architecture proposal only — not implemented  
**Scope:** `3-colocated` Redis job-status and heartbeat subsystem  
**Intended change:** Introduce one or more dedicated cleanup-manager processes, with primary/standby failover, while ordinary worker processes continue to register and publish heartbeats.  
**Explicit non-goal:** This document does not authorize or describe a source-code change already made. No source files are changed by this proposal.

---

## 1. Executive summary

The current `RedisHeartbeatManager` combines two distinct responsibilities in one process:

1. Publishing heartbeats for the local worker process.
2. Running the global expired-worker cleanup scan.

Its constructor starts a cleanup thread for every Redis-enabled process. Consequently, every worker process can become a scanner contender, even though only one scanner normally owns the existing short-lived cleanup lease at a time.

The proposed architecture separates those responsibilities:

- **Worker processes** continue to use `RedisJobStatus` to register their own server identity, mutate/read job status, and publish their own heartbeat. They do not run cleanup scans.
- **Dedicated cleanup-manager processes** do not represent application workers and do not register themselves as worker servers. They run the cleanup scheduler and coordinate leadership/failover.
- **A primary manager** performs cleanup scans.
- **Standby managers** remain ready, monitor leadership, and take over after the primary's ownership expires or is relinquished.
- **Worker lifecycle fencing remains authoritative** for preventing new job mutations against a worker that has entered `DELETING`.
- **Manager fencing is a separate concern.** It prevents a former primary from continuing cleanup after a standby has taken over.

The existing cleanup lease is currently a *per-scan coordination lease*: it is acquired at the beginning of a scan and released at the end. It is not persistent primary/standby leadership. A standby can therefore acquire it between scans, and successive scans can be performed by different processes.

A persistent leadership lease is needed if the deployment wants a stable primary between scans and automatic standby promotion. However, a lease alone is not a complete stale-primary fence. A paused process can resume after its lease expires. Cleanup mutations must have a fencing boundary that is checked atomically in the same Redis Cluster hash slot as the mutation.

**Important Redis Cluster constraint:** the current global lease key, `servers:{registry}:cleanup-lease`, is in the `{registry}` hash slot. Cleanup Lua scripts operate on keys in a per-type `{serverType}` slot. A Lua script in a type slot cannot atomically check the global registry-slot lease key; passing it as another key would violate Redis Cluster's same-slot requirement. Do not simply add the global lease key to the existing per-type cleanup scripts and assume this is safe.

This document recommends a **two-level design**:

1. A global manager lease may elect one preferred primary for operational simplicity.
2. Each server type has a **type-local leadership lease and monotonically increasing fencing epoch**, stored in that type's hash slot. Every cleanup mutation for that type validates the type-local epoch in the same Lua script and slot.

The type-local fence is the correctness boundary. The global lease is coordination and role selection. If simpler operations are preferred over a single global primary, the global lease can be omitted and managers can compete directly for per-type leadership; this naturally shards cleanup ownership by server type.

---

## 2. Current implementation and why it should be separated

### 2.1 Current process behavior

Today, `RedisHeartbeatManager::Instance()` constructs one manager singleton per process. When Redis job status is enabled, the constructor:

- obtains the `RedisJobStatus` singleton;
- loads and validates heartbeat/cleanup configuration;
- obtains the Redis client;
- marks the manager enabled; and
- starts the cleanup thread.

The cleanup thread repeatedly calls `scanOnce()`, catches scan exceptions, and waits for the configured scan interval. This happens independently of whether `HeartbeatStart()` has been called. `HeartbeatStart()` starts the local heartbeat publisher, but it is not the switch that starts the cleanup scanner.

The existing scan obtains `servers:{registry}:cleanup-lease` using a unique owner token and a TTL. It renews that lease only at scan checkpoints, then releases it at the end of the scan. The lease therefore reduces normal simultaneous scanning but does not establish a long-lived primary role.

### 2.2 Problems with the current coupling

- Every enabled worker process starts a cleanup thread, creating unnecessary threads, Redis traffic, lease contention, and scan wakeups.
- The heartbeat publisher and cleanup scanner have different scaling and availability requirements.
- A worker restart or deployment event can affect both heartbeat publishing and scanner participation, even though they are independent responsibilities.
- The per-scan lease does not express a stable primary/standby relationship.
- A scan can take longer than the lease TTL or block between renewal checkpoints. A second scanner can acquire the lease while the first process is still alive.
- The existing design relies on per-worker lifecycle fencing and idempotent cleanup to make overlapping scans safe. That is a valid safety foundation, but it is not the same as hard fencing of a former cleanup-manager leader.

### 2.3 Target process topology

Run ordinary application workers and cleanup managers as separate deployment roles:

| Role | Register worker identity? | Publish worker heartbeat? | Create/update jobs? | Run cleanup scans? |
|---|---:|---:|---:|---:|
| Application worker | Yes | Yes | Yes, for its own identity | No |
| Primary cleanup manager | No | No | No | Yes |
| Standby cleanup manager | No | No | No | No, until it takes over |
| Administrative/browser client | No | No | Only explicitly supported admin operations | No |

A cleanup manager is infrastructure, not a worker. It should not create a fake `serverType/serverId` entry in the worker registry merely to obtain access to Redis. It should use a dedicated manager configuration and Redis client access path.

---

## 3. Responsibility boundaries

### 3.1 Application worker responsibilities

An application worker remains responsible for its own lifecycle only:

1. Initialize the Redis job-status API with its configured `serverType` and globally unique `serverId`.
2. Call `registerServer()` when its process identity is ready.
3. Publish its heartbeat at the configured refresh interval.
4. Create, transition, and complete jobs through the supported API.
5. Stop publishing heartbeats during orderly shutdown and allow its lifecycle to be retired according to the existing protocol.
6. Never run the global cleanup scan loop.

The worker must not:
- acquire or renew cleanup-manager leadership;
- enumerate all server types or worker IDs for cleanup;
- fence other workers;
- delete other workers' job indexes or counters;
- collect retired-worker tombstones; or
- decide that another worker has expired.

The worker's own lifecycle fence remains in Redis. Job create/transition/complete Lua scripts must continue checking that the owning worker is `ACTIVE` in the same per-type slot as the job mutation.

### 3.2 Dedicated cleanup-manager responsibilities

A cleanup manager is responsible for cluster-wide cleanup orchestration:

- participate in manager election and standby promotion;
- maintain its manager ownership/leadership state;
- schedule scans;
- enumerate registered server types and worker IDs;
- invoke the existing worker-expiration fence;
- collect the relevant job references and invoke atomic per-worker cleanup;
- backfill and collect retired-worker tombstones;
- record metrics, logs, and scan outcomes;
- stop initiating work immediately when it detects loss of leadership;
- release leadership on graceful shutdown where safe; and
- leave unfinished work recoverable by a later manager.

It must not:
- register as an application worker;
- publish a worker heartbeat;
- create or mutate application jobs;
- bypass the worker lifecycle state machine;
- make non-atomic per-key cleanup changes that should be performed by the existing Lua scripts; or
- assume that being the global primary automatically fences a former process in every Redis Cluster slot.

### 3.3 Redis responsibilities

Redis remains the coordination and state authority:

- worker state and heartbeat data determine worker expiration;
- per-type Lua scripts serialize worker lifecycle transitions and job cleanup;
- manager lease keys determine which manager may act as leader;
- manager fencing epochs reject stale manager operations;
- registry keys enumerate server types and registered worker IDs;
- tombstones preserve retirement history and permit delayed collection.

The manager process is an orchestrator. It should not replace the existing atomic Redis state transitions with a sequence of client-side read/modify/write operations.

---

## 4. Leadership, leases, and fencing

### 4.1 Why a lease is still useful

A lease is useful for failure detection and automatic standby promotion. The primary periodically renews an expiring ownership record. If it crashes, loses Redis connectivity, or stops renewing, the lease expires and a standby can acquire leadership.

Without an expiring ownership mechanism (or an equivalent external consensus/coordination service), a standby cannot reliably distinguish a failed primary from a slow one. A manually configured active/standby flag is not sufficient for automatic failover.

The existing lease is not yet suitable as persistent leadership because it is acquired and released around each individual scan. It should not be described as a primary lease in its current form.

### 4.2 Lease versus fencing token

These solve different problems:

- **Lease:** determines when a new manager is eligible to take over.
- **Fencing token/epoch:** identifies the generation of leadership and prevents an older generation from mutating state after a newer generation has taken ownership.

Consider this sequence:

1. Manager A owns a lease.
2. A pauses for a long GC pause, process suspension, or network stall.
3. A's lease expires.
4. Manager B acquires leadership.
5. A resumes and continues executing code it started before the pause.

The lease does not physically stop A. It only makes A's ownership expired. If A can still issue Redis commands, it may continue unless the mutation itself rejects A's stale ownership generation.

Therefore a robust primary/standby design needs both lease-based takeover and an atomic fencing check on cleanup mutations.

### 4.3 Redis Cluster slot constraint

Current key placement is intentional:

- Global registry and current cleanup lease: `{registry}`.
- Worker state, heartbeat, job indexes, counters, and cleanup scripts for a type: `{serverType}`.

Redis Cluster requires all keys accessed by one Lua script to be in the same hash slot. A per-type cleanup script cannot safely add `servers:{registry}:cleanup-lease` as a key and atomically check it. The global lease is in another slot.

This is not a minor implementation detail. It rules out the simplistic design “pass the global lease key into every existing per-type cleanup script.”

There are three viable design levels:

**Option A — Keep the current per-scan lease as best-effort coordination.**  
The per-worker lifecycle fence, atomic cleanup scripts, and idempotency continue to protect correctness. This is the smallest change, but it does not provide stable primary/standby roles or strict stale-manager fencing.

**Option B — Persistent global lease only.**  
A global lease elects a preferred primary and is renewed between scans. The manager checks ownership at scan checkpoints and stops after loss. This improves role stability and failover behavior, but there remains a race between the last ownership check and a per-type mutation. Because the global key is cross-slot, this is not a hard mutation fence by itself. Document it as coordination, not strict fencing.

**Option C — Type-local manager lease and fencing epoch (recommended correctness boundary).**  
For each server type, store the manager lease and fencing epoch in the `{serverType}` slot. Every cleanup Lua script for that type receives the type-local manager lease/epoch key(s) and validates the current epoch/owner before mutating that type's data. Since the manager fence and cleanup data share the slot, the check and mutation can be atomic.

Option C supports one manager owning many types, or multiple managers owning disjoint types. It also allows standby takeover independently per type. A global registry-slot lease may still be retained as a coarse preferred-primary election, but it is not relied upon to fence per-type mutations.

### 4.4 Recommended type-local fencing model

For each registered server type, introduce type-local manager coordination keys. Exact key names should be finalized during implementation, but they must include the same `{serverType}` hash tag as the data they protect. Conceptually:

- `cleanup-manager:lease:{<serverType>}` — expiring owner lease.
- `cleanup-manager:epoch:{<serverType>}` — monotonically increasing integer generation.

The exact literal key syntax should be reviewed against the repository's existing key naming conventions before implementation. The critical requirement is the shared hash tag, not the spelling.

Acquisition should be a Lua operation in that type's slot which:
1. checks whether a valid lease already exists;
2. if it does, returns “not acquired”;
3. if it does not, increments the persistent epoch;
4. writes the new owner token and lease TTL; and
5. returns the new epoch and ownership result.

Renewal must atomically compare the current owner token and epoch, then extend the TTL only if both still match. A stale manager must not be able to renew a successor's lease.

Release must atomically compare owner token and epoch before deleting the lease. A stale manager must never delete a successor's lease.

Every cleanup mutation for that type must validate the supplied epoch (and, where appropriate, owner token) against the current type-local leadership record inside the same Lua script and before the first mutation. A stale epoch returns a specific “leadership lost/stale manager” result and performs no cleanup writes.

The epoch must not be reset when the lease expires or is released. It must be durable for at least as long as any old manager could resume and attempt a write. Do not delete the epoch as part of normal cleanup or tombstone collection. If the epoch is lost through Redis data loss/restore, the system needs an explicit recovery procedure; it must not silently restart at an older generation while old processes may still be alive.

### 4.5 Should there be one global primary or per-type primaries?

There are two deployment choices:

**Single preferred primary with per-type fences**
- A global lease in `{registry}` elects one preferred primary process.
- That primary acquires/renews type-local leases for all server types it scans.
- Standbys wait for global leadership, then acquire type-local leases.
- Type-local fencing remains the hard correctness boundary.
- Operationally easy to reason about as one primary and one or more standbys.
- A manager crash may require a new leader to acquire each type lease as the old per-type leases expire.

**Sharded per-type leadership**
- Managers compete for individual type-local leases.
- One manager can own several types; different managers can own different types.
- A standby can take over only the types whose leases expire.
- No global lease is required for correctness.
- Higher parallelism and reduced blast radius, but operationally this is a set of per-type primaries rather than one global primary.

For the stated requirement of one dedicated primary and standby backup(s), start with the **single preferred primary plus per-type fences**. Keep the global lease as a role/election mechanism, and make the per-type epoch validation mandatory for cleanup correctness. Consider per-type sharding later if scan throughput or hot-type isolation requires it.

### 4.6 Leadership acquisition and startup sequence

A manager should not scan merely because its process has started.

Suggested sequence:

1. Load and validate manager-specific configuration.
2. Connect to Redis and verify required commands/scripts are available.
3. Generate a process-instance owner token with sufficient uniqueness (for example, random process nonce plus deployment identity; never use a reusable hostname alone).
4. Attempt to acquire the global preferred-primary lease.
5. If acquisition fails, enter standby mode. Do not run scans.
6. If acquisition succeeds, enumerate known server types and acquire the type-local lease/epoch for each type it intends to scan.
7. Begin leadership renewal.
8. Only after the relevant type-local lease has been acquired and its epoch is known may the manager scan that type.
9. Reconcile its local role state with Redis before every new scan and at defined checkpoints.

A manager should tolerate an empty registry and types appearing while it is primary. It must acquire the type-local fence for a newly discovered type before performing cleanup in that type's slot.

### 4.7 Renewal and lease loss

The current scan code renews the per-scan lease only at checkpoints. That is insufficient for persistent leadership, especially while idle between scans.

Persistent leadership requires renewal independent of scan progress. Use a dedicated renewal mechanism (for example, a small manager-owned renewal thread or event loop) that:

- renews the global lease at a fraction of its TTL;
- renews each owned type-local lease at a fraction of its TTL;
- uses compare-owner-and-epoch Lua operations;
- records last successful renewal and renewal latency;
- marks leadership lost immediately when renewal fails or ownership cannot be confirmed;
- signals the scan loop to stop starting new work; and
- prevents new cleanup calls from being issued under a locally stale leadership state.

The renewal interval should be comfortably shorter than the TTL and account for Redis latency, scheduling pauses, and deployment jitter. Do not set TTL and renewal interval so close that a normal transient pause causes frequent leadership churn. Values should be configuration, with safe validation and documented defaults established through operational testing.

If renewal fails:
1. mark the affected leadership state as lost;
2. stop scheduling scans for the affected scope;
3. do not attempt to reacquire and continue using the old epoch;
4. cancel/abort the current scan at the next safe checkpoint;
5. let any in-flight per-type Lua script complete atomically; its epoch check determines whether it was authorized at execution time;
6. do not release a lease unless compare-owner-and-epoch confirms that this manager still owns it; and
7. re-enter standby/re-election logic.

A network timeout is ambiguous: the renewal may have succeeded in Redis even if the client did not receive the response. Treat ownership as unconfirmed until a subsequent compare-token check succeeds. Never assume that a timeout means either “renewal definitely failed” or “renewal definitely succeeded.”

### 4.8 Graceful shutdown

On orderly shutdown:

1. stop scheduling new scans;
2. signal the active scan loop to stop at a safe checkpoint;
3. wait for in-flight Redis operations to finish or time out;
4. release each type-local lease only with compare-owner-and-epoch;
5. release the global preferred-primary lease only with compare-owner token;
6. stop renewal; and
7. close Redis resources.

If shutdown is abrupt, TTL expiry provides recovery. Correctness must not depend on graceful release.

---

## 5. Cleanup scan behavior under the new model

The cleanup algorithm should remain substantially the same. The architectural change is who is allowed to run it and how manager authority is fenced.

For each server type:

1. Confirm the manager still owns the type-local lease and epoch.
2. Enumerate worker IDs from the type's registry.
3. For each worker, invoke the existing per-worker expiration-fence Lua script.
4. If the worker is eligible for retirement, the script atomically changes `ACTIVE` to `DELETING` and records retirement state/tombstone information.
5. Read the worker's job references.
6. Invoke the existing cleanup Lua script, augmented with a type-local manager-epoch validation before any mutation.
7. Remove the registry membership idempotently after successful cleanup or confirmed already-cleaned state.
8. Backfill retired tombstones as currently designed.
9. For eligible tombstone candidates, perform the existing cross-slot registry check outside the type-local script, then invoke the type-local tombstone collection script with manager-epoch validation.
10. Recheck leadership at scan checkpoints and before moving to another batch/type.

The worker lifecycle fence remains necessary even with manager fencing. They protect different things:

- Worker lifecycle fence: “May this worker still create/change jobs?”
- Manager leadership fence: “May this manager still perform cleanup?”

Do not remove or weaken the worker `ACTIVE`/`DELETING` checks when adding manager leadership.

### 5.1 In-flight work and safe interruption

A manager cannot reliably interrupt a Redis Lua script already executing on the server. The correct boundary is therefore the atomic script itself:

- If the old manager's type-local epoch is still current when the script executes, the script may proceed.
- If a new manager has acquired a later epoch, the old manager's script must reject the stale epoch before any mutation.
- A script that began before takeover and completed atomically before the new epoch was installed is an earlier authorized operation. The new manager must tolerate its result and continue idempotently.
- A stale manager must not be allowed to mutate after the successor's epoch is current.

This is why the manager epoch check must be inside every relevant mutation script, not merely in C++ immediately before calling Redis.

### 5.2 Which scripts and paths need review

Implementation must inventory every manager-driven mutation path, not just the main job cleanup script. At minimum review:

- worker expiration fencing / `ACTIVE` to `DELETING`;
- per-worker job/index/counter cleanup;
- retired tombstone backfill if it mutates persistent state;
- retired tombstone collection;
- any registry or per-type membership changes performed by the scanner;
- any future repair/reconciliation operation invoked by the manager.

Read-only enumeration can occur outside Lua, but the manager must not assume that a read performed while it was leader authorizes a later write after leadership changes.

For mutations spanning `{registry}` and `{serverType}`, Redis Cluster cannot provide one atomic cross-slot script. Keep the existing split protocol and idempotency, and ensure the type-local fence protects the type-local mutation. Registry membership operations should remain idempotent and be designed so a retry or overlap is safe. Do not claim a cross-slot atomic transaction where none exists.

---

## 6. Configuration and API separation

The current heartbeat manager configuration mixes heartbeat publishing and cleanup settings. The new design should make the role explicit.

Possible configuration concepts (names are illustrative and not a prescribed API):

### Worker configuration
- Redis job status enabled/disabled.
- Worker `serverType`.
- Globally unique worker `serverId`.
- Heartbeat TTL.
- Heartbeat refresh interval.
- Optional first-heartbeat timeout metadata.

Worker configuration must not require cleanup lease TTL, manager election settings, scan interval, or tombstone retention settings if the worker never runs cleanup.

### Cleanup-manager configuration
- Manager role enabled/disabled.
- Global preferred-primary lease TTL and renewal interval, if using global election.
- Type-local manager lease TTL and renewal interval.
- Cleanup scan interval.
- First-heartbeat timeout.
- Retired tombstone retention.
- Batch/page sizes and scan limits, if introduced.
- Redis connection settings and timeouts.
- Shutdown/drain timeout.
- Metrics/logging identity.

A deployment should be able to run:
- many workers with heartbeat publishing enabled and cleanup disabled;
- one dedicated manager configured to participate in election;
- one or more standby managers with the same cleanup scope/configuration; and
- Redis-disabled workers that preserve the existing no-op API behavior.

### 6.1 API design considerations

Prefer explicit role separation rather than a single ambiguous boolean that silently changes process behavior.

Possible directions:
- Keep a worker-facing heartbeat publisher API separate from a manager-facing cleanup service.
- If retaining `RedisHeartbeatManager` for compatibility, split its internals into independent heartbeat-publisher and cleanup-manager components, and make cleanup participation opt-in.
- Ensure worker construction does not start cleanup threads by default.
- Ensure `scanOnce()` cannot bypass a disabled/standby role. It is currently a public method; merely suppressing thread creation is insufficient if callers can invoke it directly.
- Make manager-only methods inaccessible to ordinary worker code where practical.
- Keep configuration providers role-specific so a worker does not fail startup because manager-only configuration is missing or invalid.
- Do not instantiate a fake worker identity to reuse `RedisJobStatus` if that causes a manager to appear in the worker registry.

The exact public API and compatibility strategy require a separate implementation review.

---

## 7. Failure scenarios and expected behavior

| Scenario | Expected behavior |
|---|---|
| Primary manager process crashes | Its leases expire; a standby acquires leadership and resumes scanning. |
| Primary is paused longer than lease TTL | Standby may take over; when old primary resumes, its old type-local epoch is rejected by cleanup Lua scripts. |
| Primary loses Redis connectivity | It cannot renew; it stops initiating work and treats leadership as unconfirmed. |
| Renewal response times out | Manager does not assume ownership; it verifies owner/epoch before continuing. |
| Standby starts while primary is healthy | Standby remains idle and does not scan. |
| Primary shuts down gracefully | It stops scans and compare-releases its leases; standby can take over promptly. |
| Primary crashes during a scan | The next manager repeats the scan; worker lifecycle fencing and idempotent cleanup make repetition safe. |
| A scan takes longer than a lease TTL | Renewal runs independently; if renewal is lost, type-local epoch validation prevents stale writes after takeover. |
| A worker registers while cleanup enumerates IDs | Existing registration/activation protocol and worker lifecycle scripts remain the safety boundary; scanner retries/re-enumeration may be needed for liveness. |
| A worker becomes active after a scan read but before its cleanup fence | The expiration-fence Lua script rechecks current heartbeat/state atomically and must not retire a live worker. |
| Two managers accidentally believe they are primary | Only the manager with the current type-local epoch can mutate that type through fenced scripts. |
| A new server type appears after primary startup | Primary discovers it on a later registry pass and acquires that type's local leadership fence before scanning it. |
| Redis failover/restore loses or rolls back the epoch | Treat as a leadership-state recovery event; do not assume old epochs remain fenced. Require operational recovery/epoch reinitialization while all old managers are stopped or otherwise fenced. |
| Global lease expires but type-local lease has not | New preferred primary waits for or safely takes over each type after its type-local lease permits it; it must not bypass the type-local fence. |
| Type-local lease expires while global primary remains healthy | Primary must stop cleanup for that type, reacquire with a new epoch if eligible, and never continue using the old epoch. |

### 7.1 Liveness versus safety

Safety means an obsolete manager cannot perform unauthorized cleanup mutations. Liveness means expired workers are eventually discovered and cleaned.

The lease TTL, scan interval, batch size, Redis latency, and renewal scheduling affect liveness and failover time. The worker lifecycle fence, type-local manager epoch validation, and idempotent scripts provide safety.

A conservative lease TTL can reduce unnecessary failovers but lengthens standby takeover after a crash. A short TTL improves takeover time but increases sensitivity to scheduling pauses and Redis latency. Select values from measured production latency and pause behavior, not guesswork.

---

## 8. Observability and operations

Expose metrics and structured logs with manager identity, leadership generation, server type, and scan correlation ID.

Recommended metrics:

### Leadership
- current role: primary / standby / acquiring / lost / stopping;
- global lease acquisition successes/failures;
- type-local lease acquisition successes/failures by server type;
- current fencing epoch by server type;
- renewal successes/failures and latency;
- time since last confirmed renewal;
- leadership-loss count and reason;
- takeover duration;
- graceful-release success/failure.

### Scanning
- scan start/end time and duration;
- server types and worker IDs enumerated;
- workers checked, fenced, cleaned, already cleaned, and skipped;
- tombstones backfilled, considered, retained, and collected;
- per-type scan duration;
- Redis command/script errors;
- scan aborts due to leadership loss;
- repeated cleanup attempts and idempotent outcomes.

### Alerts
- no manager holds leadership for longer than the expected failover window;
- multiple managers report primary (should be impossible at the mutation boundary, but still a useful alert);
- renewal failures exceed threshold;
- a manager's last confirmed renewal is approaching lease expiry;
- scan duration consistently exceeds the scan interval;
- tombstone backlog grows continuously;
- expired workers remain in `DELETING` for an unexpectedly long time;
- per-type cleanup errors or counter validation failures occur.

Do not log raw secrets or Redis credentials. Manager owner tokens should be opaque and should not be exposed as high-cardinality metric labels; log a safe shortened identifier if needed.

### Operational procedures

Document procedures for:
- deploying a new manager version with standby first;
- verifying standby status before promoting/restarting primary;
- draining and stopping a primary;
- diagnosing repeated lease loss;
- handling a Redis outage or failover;
- recovering after Redis data restore that may roll back fencing epochs;
- disabling all cleanup managers temporarily;
- manually reconciling stuck `DELETING` workers or tombstones; and
- confirming that ordinary workers have cleanup scanning disabled.

---

## 9. Migration plan

A staged migration is safer than changing all worker behavior and leadership semantics in one release.

### Phase 1 — Separate worker and cleanup roles

- Introduce explicit worker-vs-manager configuration.
- Keep worker registration and heartbeat publishing unchanged.
- Disable automatic cleanup-thread startup in ordinary workers.
- Ensure direct calls to `scanOnce()` are also blocked for worker/standby roles.
- Deploy one dedicated manager using the existing per-scan lease.
- Verify that worker heartbeats continue while worker processes no longer scan.

At this phase, cleanup is still coordinated by the current short-lived lease. This removes worker scan overhead but does not yet provide persistent primary/standby leadership.

### Phase 2 — Add persistent manager leadership

- Implement global preferred-primary lease acquisition, renewal independent of scans, and compare-token release.
- Add standby state and retry/backoff.
- Keep the distinction explicit: global lease is role coordination, not the hard per-type mutation fence.
- Validate crash and graceful-shutdown takeover behavior.

### Phase 3 — Add type-local fencing epochs

- Introduce per-type manager lease/epoch keys in each `{serverType}` slot.
- Implement atomic acquire, renew, release, and epoch-validation Lua scripts.
- Add the epoch check to every manager-driven per-type mutation before any write.
- Ensure stale epochs return a distinct result and do not partially mutate data.
- Keep epoch keys out of ordinary worker cleanup and tombstone garbage collection.

This phase is required before claiming hard stale-primary fencing.

### Phase 4 — Failure and load testing

Test at least:
- primary process kill during idle and during each scan phase;
- primary pause longer than lease TTL followed by standby takeover and old-primary resume;
- Redis timeout during acquire, renew, release, and cleanup;
- overlapping scan attempts;
- worker heartbeat refresh during expiration checks;
- worker registration during type enumeration;
- multiple server types on different Redis Cluster slots;
- large server/job counts and long-running scans;
- tombstone backfill and collection during takeover;
- Redis restart/failover and persistence recovery; and
- manager rolling deployment.

Verify not only final counts but also that stale-epoch scripts make **zero writes**.

### Phase 5 — Remove obsolete worker scanner behavior

After the dedicated manager path is stable:
- remove or deprecate worker-side automatic cleanup startup;
- simplify worker configuration so it no longer requires manager-only values;
- update README and deployment documentation;
- retain explicit administrative recovery procedures; and
- keep the old behavior disabled by default rather than silently falling back to every worker scanning.

---

## 10. Testing requirements and acceptance criteria

The change should not be considered complete based only on code review or a successful compile.

### Functional acceptance
- Ordinary workers register and publish heartbeats as before.
- Ordinary workers never start cleanup scan threads.
- A dedicated primary starts scanning only after acquiring leadership.
- Standbys do not scan while they lack leadership.
- A standby takes over after primary lease expiry or graceful release.
- Only the current type-local fencing epoch can perform cleanup mutations.
- A former primary resuming after takeover is rejected by every fenced cleanup mutation.
- Existing worker lifecycle fencing and job counter/index validation remain intact.
- Cleanup is repeat-safe across crashes and retries.
- Redis-disabled behavior remains a no-op as currently documented.

### Cluster acceptance
- Every Lua script's keys share a Redis Cluster slot.
- The type-local manager epoch and all protected per-type mutation keys share the same `{serverType}` hash tag.
- No per-type Lua script reaches into the global `{registry}` slot.
- Cross-slot registry updates remain explicitly non-atomic and idempotent.
- Tests exercise at least two server types on distinct slots.

### Concurrency acceptance
- Pause the old primary after it reads data but before it invokes a cleanup script.
- Allow the lease to expire and the standby to acquire a newer type-local epoch.
- Resume the old primary.
- Confirm that its stale cleanup script returns the stale-leader result and performs no writes.
- Repeat around expiration fencing, job cleanup, tombstone backfill, and tombstone collection.

### Operational acceptance
- Metrics distinguish standby from primary and show renewal health.
- Alerts identify loss of all cleanup leadership.
- Runbooks cover takeover, drain, Redis recovery, and epoch recovery.
- Deployment proves that worker fleet size no longer increases cleanup-scanner count.

---

## 11. Existing design properties to preserve

The dedicated-manager change must preserve these existing invariants:

1. `serverId` is globally unique across process incarnations and includes the server type prefix, as guaranteed by the application.
2. A worker's job mutation scripts check its lifecycle state in the same type slot.
3. The `ACTIVE` to `DELETING` transition is atomic with the worker expiration decision.
4. Cleanup validates actual job references, ownership, statuses, indexes, and counter sufficiency before mutating.
5. Already-cleaned cleanup is recognized and does not decrement counters a second time.
6. Per-type counters are updated atomically with the corresponding job mutation/cleanup.
7. Tombstone collection rechecks the relevant per-type state before deletion.
8. Registry membership operations remain idempotent across retries and overlapping cross-slot phases.
9. The global registry slot and per-type slots remain intentionally separate.
10. Redis Lua atomicity does not imply rollback after a runtime error; scripts must continue preflighting key types and counter invariants before the first mutation.
11. External direct Redis writes that bypass the supported API remain outside the consistency guarantee.
12. A cleanup manager is not a worker and must not weaken worker lifecycle checks.

---

## 12. Open design decisions before implementation

These decisions should be resolved in a source-level design review before code is written:

1. **Global versus per-type leadership:** confirm the initial deployment uses one preferred global primary with type-local mutation fences, or chooses fully sharded per-type primaries.
2. **Key names and data types:** finalize type-local lease and epoch keys, TTL semantics, and persistence behavior.
3. **Epoch representation:** choose an integer range and overflow policy; use an atomic Redis increment and never reset under normal operation.
4. **Lease durations:** establish TTL, renewal interval, acquisition retry/backoff, and shutdown drain timeout from measured latency.
5. **Renewal implementation:** decide thread/event-loop ownership, synchronization with scan shutdown, and behavior on ambiguous command timeouts.
6. **New server-type discovery:** decide how often the primary refreshes the registry and how it acquires leadership for newly observed types.
7. **Script coverage:** enumerate every cleanup mutation and prove that each is protected by the correct type-local epoch.
8. **Public API compatibility:** decide whether `RedisHeartbeatManager` remains as a facade or is split into separate worker heartbeat and manager cleanup classes.
9. **Configuration ownership:** separate worker heartbeat settings from manager-only scan/leadership settings.
10. **Redis durability/recovery:** define how epoch monotonicity is restored after backup/restore or a failover that may roll back data.
11. **Deployment model:** define how many dedicated managers run, how standby readiness is monitored, and how rolling upgrades avoid two active scan loops.
12. **Manual administration:** define whether browser/admin whole-type deletion is allowed while a manager is active and how it coordinates with worker registration and manager fencing.

---

## 13. Recommendation

Adopt the dedicated-process separation first: workers register and publish their own heartbeats, while only dedicated manager processes run cleanup. This immediately removes cleanup-thread and scan contention from the worker fleet.

For high availability, use a persistent primary/standby election lease. But do not treat the global `{registry}` lease as a hard fence for per-type cleanup. The Redis Cluster slot layout prevents that lease from being checked atomically inside the current per-type scripts.

The recommended robust end state is a global preferred-primary role plus **type-local manager leases and fencing epochs**, with every manager-driven per-type mutation validating its epoch atomically in the same slot. The global lease coordinates which process should lead; the type-local epochs prevent stale managers from changing data. The existing worker lifecycle fence continues to protect worker/job correctness.

This is an architectural proposal, not a proof of correctness and not a claim that implementation or tests have been completed. The detailed Lua key inventory, error semantics, and failure-injection tests are mandatory parts of the implementation work.
