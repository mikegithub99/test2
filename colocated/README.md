# Colocated Redis heartbeat and cleanup

Self-contained C++ implementation: every worker process publishes its own heartbeat and starts one in-process cleanup manager. A Redis distributed lease normally elects one process to scan the registry, avoiding redundant full scans.

## Files
- `RedisClient.h/.cc`: process-wide Redis client singleton.
- `RedisJobStatus.h/.cc`: registration, job lifecycle, heartbeat refresh, and read APIs.
- `RedisJobScripts.inc`: all embedded Lua scripts, including lease acquire/renew/release.
- `RedisHeartbeatManager.h/.cc`: in-process cleanup manager coordinated by a Redis lease.
- `heartbeat.cc`: worker startup example.
- `HEARTBEAT_DESIGN.md`: detailed architecture.
- `NEWIDEA.md`: proposal and implementation status.

## Startup
Initialize configuration and Redis settings, construct one `RedisJobStatus`, call `registerServer()`, then call `RedisHeartbeatManager::Start()`. Publish immediately and refresh every 10 seconds using `refreshHeartbeat(30)`. The heartbeat loop has no explicit stop condition or signal handling; it runs for the worker process lifetime. Application work runs on other threads. Do not deploy the old standalone cleanup executable for this mode.

## Lease and correctness
The lease key is `servers:{registry}:cleanup-lease`. It uses a unique owner token and 30-second TTL. Renewal and release use token-checked Lua; stale owners cannot modify a successor's lease. The manager renews at work boundaries about every five seconds. The lease is only a scan-load optimization. If scans overlap after lease expiry, per-server Lua fencing and idempotent Phase A cleanup remain authoritative. Phase B registry removal is separate because registry and per-type keys occupy different Redis Cluster slots.

## Build
Requires C++17, redis-plus-plus, hiredis, and host-provided `get_reg_val(const std::string&)` and `initializeConfiguration()`. Compile `RedisClient.cc`, `RedisJobStatus.cc`, `RedisHeartbeatManager.cc`, and `heartbeat.cc` into the worker. Adapt include/link flags to the installed libraries, for example:

```sh
c++ -std=c++17 -pthread RedisClient.cc RedisJobStatus.cc \
  RedisHeartbeatManager.cc heartbeat.cc \
  -I/path/to/redis-plus-plus/include \
  -L/path/to/redis-plus-plus/lib -lredis++ -lhiredis -o worker
```

If integrating into an existing worker main, do not compile `heartbeat.cc` as a second main; integrate its startup sequence instead.

## Validation
Implemented but not compiled or tested against live Redis/Redis Cluster. Validate concurrent acquisition, crash/takeover, stale-token renew/release, lease expiry during slow scans, overlapping cleanup, heartbeat/fencing races, Phase A/Phase B recovery, counter integrity, and hash-slot separation before deployment.
