# Colocated Heartbeat and Cleanup Design

Every worker process runs its own heartbeat publisher and starts one in-process RedisHeartbeatManager. The process-local singleton ensures one cleanup thread per process. No separate cleanup executable is required.

## Startup and timing
1. Initialize host configuration and Redis settings.
2. Construct one long-lived RedisJobStatus.
3. Call registerServer().
4. Call RedisHeartbeatManager::Start().
5. Publish heartbeat immediately, then every 10 seconds with a 30-second TTL.

The cleanup manager attempts a scan every five seconds. A worker that cannot acquire the lease skips registry discovery. The heartbeat loop has no explicit stop condition or signal handling and runs for the process lifetime. Application work runs on other threads.

## Distributed lease
The lease key is `servers:{registry}:cleanup-lease`. Each manager creates a unique token and acquires atomically with SET NX PX semantics. Only the owner scans the registry. Renewal and release are Lua scripts that compare the stored token before changing the key. A stale owner cannot renew or delete a successor's lease. A process crash is recovered by TTL expiry.

The lease is a scan-load optimization, not a correctness lock. A long Redis operation may outlive the lease and allow overlapping scans. Per-server atomic fencing and idempotent cleanup must remain safe in that case.

## Ownership
RedisJobStatus retains configured enabled state, server type, server ID, and the shared Redis client pointer. It is not a singleton and owns no scheduling. RedisClient::Instance() remains the process-wide shared client. The worker integration owns heartbeat scheduling; the manager owns cleanup scheduling.

## Redis Cluster boundaries
Registry keys and the lease use the {registry} hash tag. Heartbeats, lifecycle state, job indexes/hashes, and summaries use the {serverType} tag. Never combine these groups in one Lua script. Phase B registry removal is separate.

## Cleanup protocol
Heartbeat refresh verifies ACTIVE, writes the heartbeat TTL, and adds live-set membership atomically. Fencing verifies heartbeat absence, ACTIVE state, and prior live-set membership, then changes the candidate to terminal DELETING. Job lifecycle scripts reject writes after fencing. Redis serializes these scripts in the per-type slot.

Phase A operates only in the per-type slot. It preflights key types, job ownership, statuses, indexes, and counters before deleting the candidate's jobs/artifacts and adjusting type counters. It removes live membership and retains the DELETING tombstone. Phase B removes only the exact ID from the registry slot after Phase A is confirmed. If a manager crashes between phases, a later scan retries Phase B.

Lua scripts execute atomically relative to other commands, but Redis Lua does not roll back earlier writes after a runtime error. Cleanup performs preflight validation before its first write to reduce this risk.

## Read consistency analysis
Earlier multi-command reads could interleave with cleanup. getServerJobs() now discovers references, then invokes one Lua script with the index and every referenced job hash declared as keys. The script revalidates that the index still contains those references and reads all hashes atomically. If cleanup changes the index between discovery and script execution, the read fails and should be retried.

getServerSummaries() discovers server IDs in the registry slot, then reads all extant per-type summary hashes in one Lua execution. Because registry and per-type data occupy different Redis Cluster slots, discovery and summary retrieval cannot be one global point-in-time snapshot. Summaries removed between the steps are omitted.

## Failure behavior
- Multiple workers acquire: one lease owner; others skip scan.
- Lease owner crashes: TTL expires; another worker takes over.
- Stale owner renews/releases: token check rejects it.
- Scan outlives lease: scans may overlap; per-server Lua fencing is authoritative.
- Two managers find the same expired ID: only one wins ACTIVE-to-DELETING fencing; cleanup is idempotent.
- Crash after Phase A: a later scan retries Phase B.

## Validation
Not compiled or tested against live Redis or Redis Cluster. Validate concurrent lease acquisition, crash/takeover, stale-token operations, slow scans and overlap, heartbeat/fencing races, Phase A/Phase B recovery, counter integrity, atomic reads during cleanup, and Cluster slot behavior before deployment.
