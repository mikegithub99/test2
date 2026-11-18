# 3-refactored

A refactoring workspace based on the established `3-colocated` implementation.

## Scope and compatibility constraints

This directory preserves the existing Redis key schema, hash tags, Lua script
logic, Lua argument ordering, integer return values, and cross-slot lifecycle
protocol. It introduces checked C++ conversions for script results and
operation-specific result types where numeric meanings differ.

## Files

- `RedisJobStatus.h/.cc` — job lifecycle API and checked script-result conversion.
- `RedisJobScripts.inc` — embedded Lua scripts, retained from `3-colocated`.
- `RedisHeartbeatManager.h/.cc` — distributed cleanup orchestration with
  operation-specific checked result types.
- `HeartbeatRunner.h/.cc` — background heartbeat publisher.
- `heartbeat.cc` — worker startup example.
- `RedisClient.h/.cc` — Redis client wrapper required by these sources.

## Result handling

The public `RedisJobStatus::Result` numeric contract remains unchanged for
job lifecycle methods. Registration, activation, and heartbeat have separate
named result types because their integer meanings are operation-specific.
Heartbeat manager lease, fencing, cleanup, and tombstone operations likewise
use distinct internal result types. Unknown integer results raise an exception
instead of being silently accepted or ignored.

Redis error replies continue to propagate as exceptions.

## Deliberately unchanged

- Redis key names, prefixes, and hash tags.
- Redis data structures and field names.
- Lua executable statements and return codes.
- Lua KEYS/ARGV ordering.
- Redis Cluster slot boundaries and the three-phase cleanup protocol.
- Public job lifecycle result values.

The registry and per-type registration operations still occupy different
Cluster slots. This refactor does not make registration globally atomic and
does not add compensating cleanup.

## Validation status

This is an initial refactoring pass, not a verified release. No compilation,
shell checks, or live Redis/Redis Cluster tests have been run. Review the
changed sources and compare the Lua script bodies against
`3-colocated/RedisJobScripts.inc` before deployment.
