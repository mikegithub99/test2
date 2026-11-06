# NEW IDEA: Distributed Heartbeat Cleanup Manager

## Status

**Proposal only — not implemented or validated.**

This document proposes removing the separately deployed, deployment-wide RedisHeartbeatManager service. Instead, every worker/server process starts its own in-process cleanup manager. Redis coordinates which process performs registry-wide scans, while the existing per-server Lua scripts remain responsible for correctness.

The goal is to avoid operating a dedicated cleanup process without creating unnecessary duplicate scan traffic or weakening cleanup safety.

## 1. Proposed architecture

Every worker process runs two independent activities:

1. **Heartbeat publisher:** publishes that process's own heartbeat immediately after successful registration, then refreshes it every 10 seconds using a 30-second Redis TTL.
2. **Cleanup manager:** runs inside the worker process and periodically attempts to acquire a short-lived distributed scan lease. Only the lease holder performs a full registry scan.

The cleanup manager is started internally by the worker integration. A process-local singleton ensures that calling its start method more than once does not create multiple cleanup threads within the same process. This singleton is local to one process; it is not shared across machines.

There is no separately deployed cleanup executable in this design.

### High-level flow

~~~text
Worker process A                  Worker process B
+----------------------+          +----------------------+
| Own heartbeat loop   |          | Own heartbeat loop   |
| Internal cleanup     |          | Internal cleanup     |
| manager              |          | manager              |
+----------+-----------+          +-----------+----------+
           |                                  |
           +----------- Redis ----------------+
                       |
             Distributed scan lease
             (one normal scan owner)
                       |
             Registry-wide discovery
                       |
             Per-server atomic fencing
                       |
             Per-server Phase A cleanup
                       |
             Registry Phase B removal
~~~

Every process continues publishing only its own heartbeat. Cleanup scans can discover and clean any expired server ID, including IDs belonging to other processes.

## 2. Redis scan lease

Use a Redis key in the registry hash slot, for example:

    servers:{registry}:cleanup-lease

The lease coordinates expensive registry-wide discovery. It is an efficiency mechanism, not the correctness or fencing mechanism for an individual server.

### Acquire

Each cleanup manager generates a unique owner token for its process/lease attempt and tries to acquire the lease atomically:

    SET servers:{registry}:cleanup-lease <owner-token> NX PX <lease-ms>

Only a successful SET grants permission to perform a full scan. A process that does not acquire the lease skips that scan and retries on a later interval.

The owner token must be unique and unpredictable enough to distinguish lease owners. Do not use a shared constant token.

### Renew

If a scan may take longer than the lease duration, the owner renews it using a Lua script that:

1. Reads the lease key.
2. Compares its value with the caller's owner token.
3. Extends the TTL only if the token still matches.

A plain PEXPIRE is unsafe because a former owner could extend a lease that has already expired and been acquired by another process.

### Release

Release the lease using a Lua compare-and-delete script. Delete the key only if its value still equals the releasing process's owner token. Never use an unconditional DEL.

If a process crashes, its lease expires automatically and another process can take over. If a scan exceeds its lease and another process acquires it, scans may overlap; the per-server atomic cleanup protocol must remain safe in that situation.

### Lease sizing and scan cadence

Use a lease long enough for the normal scan duration, renew it while scanning, and release it after the scan. The exact TTL and renewal interval should be chosen from measured registry size and Redis latency rather than assumed in advance.

Each process can attempt lease acquisition on the existing five-second cleanup cadence, with small randomized jitter to avoid synchronized acquisition traffic. Processes that fail to acquire the lease should not enumerate the registry.

## 3. Per-server correctness remains in Redis Lua

The distributed lease must not replace or weaken the existing per-server atomic operations. The current per-server hash-slot scripts remain the authority for deciding whether a server is expired and whether cleanup is allowed.

### Heartbeat refresh

The heartbeat refresh script atomically:

- Requires the server's state to be ACTIVE.
- Sets the exact server ID as the heartbeat value with the configured TTL.
- Adds the server ID to the per-type live set.

### Fencing

For each candidate, the fencing script atomically:

- Checks that the heartbeat key is absent.
- Requires the server to have published at least one heartbeat (live-set membership).
- Changes the exact server ID from ACTIVE to terminal DELETING.

Heartbeat refresh and fencing use the same per-type Redis slot. Redis serialization ensures that either the refresh happens first and fencing sees the renewed heartbeat, or fencing happens first and a later refresh is rejected because the state is no longer ACTIVE.

### Phase A: per-type cleanup

After fencing, the existing cleanup Lua script validates and removes only the target server's artifacts in the per-type slot. It preflights job ownership, indexes, statuses, and counters before writing. It retains the DELETING tombstone and is designed to recognize already-completed cleanup without decrementing counters twice.

### Phase B: registry cleanup

After Phase A is confirmed committed, remove the exact server ID from:

    servers:{registry}:type:<serverType>

This is a separate operation because registry keys use the {registry} hash tag and cannot be included in the same Redis Cluster Lua script as the per-type keys.

If a process crashes between Phase A and Phase B, a later scan can recognize the DELETING tombstone and completed Phase A, then retry Phase B. Both phases must remain idempotent.

## 4. Redis Cluster constraints

The scan lease and registry discovery keys use the {registry} hash tag. Per-server heartbeat, state, job, and summary keys use the {serverType} hash tag.

A Lua script cannot atomically access keys from both hash slots in Redis Cluster. Therefore:

- Lease acquire, renew, release, and registry discovery operate on registry-slot keys.
- Heartbeat checks, fencing, and Phase A cleanup operate on one server-type slot.
- Phase B registry removal is a separate registry-slot command.
- Do not combine registry lease operations with per-type cleanup in one Lua script.

The lease controls who normally scans; it does not create a cross-slot transaction.

## 5. Failure and concurrency behavior

| Situation | Expected behavior |
|---|---|
| Two processes try to acquire the scan lease | Redis grants it to one owner; the other skips the full scan. |
| Lease owner crashes | Lease expires; another process can acquire it. |
| Scan outlives the lease | A second scan may overlap; per-server Lua fencing and cleanup prevent duplicate destructive effects. |
| Two scanners discover the same expired ID | Atomic fencing and idempotent Phase A ensure only one cleanup takes effect. |
| Worker heartbeat refresh races with fencing | Redis orders both scripts; refresh either renews first or is rejected after fencing. |
| Cleanup owner crashes after Phase A | A later scan retries Phase B using the retained DELETING tombstone. |
| Redis is temporarily unavailable | Heartbeat and cleanup operations report failures and retry according to their respective loops. |
| A process loses the scan lease | It must stop treating itself as scan owner; token-checked renew/release prevent it from modifying another owner's lease. |

The distributed scan lease is not a substitute for the per-server ACTIVE/DELETING state transition. Correctness must continue to hold even if the lease expires unexpectedly and two cleanup loops overlap.

## 6. Integration with the current C++ design

The intended worker startup sequence is:

1. Initialize host configuration and Redis client settings.
2. Construct the long-lived RedisJobStatus object.
3. Call registerServer().
4. Start the worker's own heartbeat publisher.
5. Start the in-process cleanup manager once for this process.

The existing RedisHeartbeatManager::Start() process-local singleton pattern can be adapted for this purpose. The manager should acquire the distributed lease before registry-wide discovery. Its scan operation should keep the lease alive for long scans and release it with owner-token verification.

RedisJobStatus continues to retain its configured enabled flag, server type, server ID, and shared Redis client pointer. It does not need to own heartbeat scheduling or cleanup scheduling. The worker integration owns the heartbeat loop; the manager owns its cleanup loop.

The dedicated RedisHeartbeatManagerMain.cc executable would no longer be required for deployments using this architecture. It may be removed or retained only as an optional standalone mode, but both modes should not be enabled unintentionally at the same time.

## 7. Operational and performance considerations

- One cleanup thread per worker process is simple, but the thread count grows with the number of workers.
- The distributed lease prevents every worker from repeatedly scanning the full registry.
- Lease acquisition attempts still create some Redis traffic from every worker; jitter can spread this traffic.
- A single lease holder can become a scan bottleneck for very large deployments. Measure scan duration and registry size.
- The lease should be renewed during long scans and always released with compare-and-delete.
- Monitor lease acquisition failures, scan duration, renewal failures, cleanup errors, and the age/count of expired candidates.
- Use bounded work batches if a full registry scan becomes too large, while preserving retryable progress.
- Avoid holding a lease indefinitely; TTL expiry is the crash-recovery mechanism.

## 8. Implementation and test plan

Before adopting this design:

1. Add registry-slot Lua scripts for token-checked lease renewal and release.
2. Add lease acquisition, renewal, and release to the in-process manager.
3. Gate full registry discovery on successful lease acquisition.
4. Ensure the lease is renewed during long scans and released on normal completion and exceptions.
5. Keep per-server fencing, Phase A cleanup, and Phase B registry removal as separate operations with their current idempotency guarantees.
6. Start the manager internally once per worker process after configuration and Redis initialization.
7. Remove the deployment requirement for a dedicated cleanup process and update the README and design documentation.

Tests should include:

- Many worker processes racing to acquire the lease.
- Lease-owner crash and takeover after TTL expiry.
- Lease expiry during a deliberately slow scan.
- Old lease owner attempting renewal or release after a new owner acquired the lease.
- Two cleanup scanners processing the same expired server ID.
- Heartbeat refresh racing with fencing.
- Crash after Phase A and retry of Phase B.
- Zero-job and multi-job cleanup, counter integrity, and malformed Redis data.
- Redis Cluster verification that lease scripts use only registry-tagged keys and per-server scripts use only server-type-tagged keys.
- Large registry tests to measure scan duration, lease renewal, and batch behavior.

## 9. Recommendation

Adopt the **in-process cleanup manager in every worker process, coordinated by a Redis scan lease**, while retaining the existing per-server atomic Lua fencing and cleanup protocol.

This removes the dedicated cleanup service as an operational dependency, provides automatic takeover when a scanner process exits, and avoids having every worker repeatedly scan the entire registry.

This is a design proposal only. It should not be considered implemented until the lease protocol, worker startup integration, Redis Cluster behavior, and concurrency/failure tests have been completed.
