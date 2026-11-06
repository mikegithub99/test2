# 2-colocated — latest colocated Redis heartbeat implementation

This folder contains the latest colocated worker design. Each worker publishes its own heartbeat and starts one process-local cleanup manager. A Redis lease normally elects one manager to scan the global registry.

## Files
- RedisClient.h/.cc — process-wide Redis / Redis Cluster client.
- RedisJobStatus.h/.cc — registration, lifecycle, heartbeat and dashboard APIs.
- RedisJobScripts.inc — lifecycle, fencing, cleanup, lease and read-snapshot Lua.
- RedisHeartbeatManager.h/.cc — per-process cleanup thread coordinated by lease.
- heartbeat.cc — worker startup and heartbeat example.
- HEARTBEAT_DESIGN.md — detailed design and failure analysis.
- NEWIDEA.md — implementation status and validation plan.

## Startup and ownership
Initialize configuration, construct one RedisJobStatus, call registerServer(), then RedisHeartbeatManager::Start(). Publish immediately and refresh every 10 seconds with a 30-second TTL. The sample heartbeat loop runs for process lifetime and has no signal/shutdown handling; integrate an application-owned stop condition before production. RedisJobStatus is not a singleton and owns no scheduling; RedisClient::Instance() is process-wide.

Each manager attempts the shared `servers:{registry}:cleanup-lease` lease (30-second TTL, unique owner token). Only the holder normally scans; renewal and release compare the token. The owner scans all registered types and IDs and can clean a different worker. Lease expiry may allow overlapping scans; the lease is a load optimization, not a correctness lock.

## Fencing and cleanup
Heartbeat refresh and fencing serialize in the per-server-type Redis Cluster slot. Fencing verifies heartbeat absence, ACTIVE state and live-set membership, then marks DELETING. Lifecycle Lua mutators check ACTIVE: if the lifecycle script wins, cleanup accounts for its result; if fencing wins, later mutation returns ServerNotActive.

**TTL caveat:** heartbeat expiry alone does not change ACTIVE state. Until a manager fences the worker, lifecycle operations may still succeed after TTL expiry. Immediate rejection at TTL expiry would require lifecycle scripts to check heartbeat existence too; that policy is not implemented.

Cleanup Phase A validates and updates per-type job data, counters and live membership, retaining a DELETING tombstone. Phase B removes the ID from the `{registry}` slot. The phases cannot be one Redis Cluster Lua transaction because the hash slots differ. A later scan retries Phase B after a crash. Redis Lua is atomic but does not roll back prior writes after runtime errors; scripts preflight before writing.

## Dashboard read consistency
`getServerJobs()` first discovers the server's references, then runs a single Lua snapshot script with the index and all referenced hashes declared as keys. The script revalidates index membership and reads all hashes atomically; if cleanup changed the index before the script, the read fails with a retryable error instead of returning a partial list.

`getServerSummaries()` discovers IDs in the `{registry}` slot, then reads all extant summary hashes in one Lua execution in the `{serverType}` slot. Summary hashes are mutually consistent at that script's execution point. The two steps cannot be a single global point-in-time snapshot across Redis Cluster slots; a summary removed after ID discovery is omitted.

## Build
Requires C++17, redis-plus-plus, hiredis, and host-provided `get_reg_val(const std::string&)` and `initializeConfiguration()`. Compile RedisClient.cc, RedisJobStatus.cc, RedisHeartbeatManager.cc and heartbeat.cc into the worker. Do not compile heartbeat.cc as a second main when integrating its startup sequence into an existing worker main.

## Validation status
Not compiled or tested against live Redis/Redis Cluster. Verify installed redis-plus-plus supports `eval<std::vector<std::string>>`. Test concurrent lease acquisition, crash/takeover, stale tokens, slow scans/overlap, heartbeat/fencing races, Phase A/Phase B recovery, concurrent dashboard reads and cleanup, malformed key types, counter integrity, and Cluster slot routing.
