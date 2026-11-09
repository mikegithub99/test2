# Distributed Heartbeat Cleanup Manager

## Status
Implemented as a colocated variant in this directory; not yet compiled or integration-tested.

## Implemented behavior
- Every worker publishes its own heartbeat immediately after registration, then every 10 seconds with a 30-second TTL.
- Every worker calls RedisHeartbeatManager::Start(); one cleanup thread is started per process.
- Managers coordinate registry scans with servers:{registry}:cleanup-lease, a 30-second lease with a unique owner token.
- Renewal and release use token-checked Lua; stale owners cannot modify a successor's lease.
- Existing per-server fencing, Phase A cleanup, and Phase B registry removal remain the correctness protocol.
- Scans may overlap if a lease expires during a slow operation; per-server Lua remains authoritative.
- No separate cleanup manager process is required.

## Dashboard read consistency
The latest implementation snapshots the server job index and all referenced job hashes in one per-type Lua execution after declaring the discovered keys; if the index changed between discovery and snapshot, it returns a retryable error. Summary hashes are read together in one per-type Lua script after registry ID discovery. Redis Cluster slot boundaries mean registry discovery and the summary snapshot are not one global atomic view.

## Validation still required
Compile and test against Redis: concurrent acquisition, crash/takeover, stale-token renewal/release, lease expiry during slow scans, overlapping cleanup, heartbeat/fencing races, Phase A/Phase B recovery, atomic dashboard reads concurrent with cleanup, counter integrity, and Redis Cluster slot separation.
