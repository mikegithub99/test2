# 3-refactor-complete

This directory contains the completed source-organization pass for the
`3-colocated` Redis Cluster architecture.

## Design constraints

The refactor preserves the Redis key schema, hash tags, data structures,
Lua executable statements, Lua return codes, KEYS/ARGV ordering, and the
existing cross-slot registration and cleanup protocol. It does not make the
registry-slot and per-type-slot registration sequence globally atomic.

## Source layout

- `RedisJobStatus.h/.cc`: public job lifecycle API, registration phases,
  checked script-result conversion, input validation, and snapshot parsing.
- `RedisJobScripts.inc`: embedded Lua scripts with concise contract comments.
- `RedisHeartbeatManager.h/.cc`: heartbeat manager and cleanup orchestration;
  lease handling, per-server cleanup, and retired-tombstone collection are
  separated into focused helpers.
- `HeartbeatRunner.h/.cc`: periodic heartbeat publication and thread lifecycle.
- `heartbeat.cc`: worker integration example.
- `RedisClient.h/.cc`: Redis client wrapper used by the implementation.

## Result contracts

The numeric values of the public `RedisJobStatus::Result` enum are unchanged.
Lifecycle, registration, activation, and heartbeat results use checked
conversions. The cleanup manager uses separate result enums for lease
acquisition/renewal/release, fencing, cleanup, and tombstone collection.
Unexpected script result values throw instead of being silently accepted.

## Compatibility

- No Redis key names, prefixes, hash tags, fields, or data structures changed.
- No Lua executable bodies or return codes changed.
- No KEYS/ARGV positions changed.
- No cross-slot protocol or cleanup phase ordering changed.
- Public job result numeric values remain stable.

## Validation status

The source was structurally refactored. The embedded Lua script bodies were
compared against `3-colocated/RedisJobScripts.inc`. Compilation and live
Redis/Cluster tests have not been run in this environment; run the project's
normal build and integration suite before deployment.
