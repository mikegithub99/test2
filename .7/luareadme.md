# Lua in Redis and how .7 uses it

## What Lua is

Lua is a small, lightweight, embeddable programming language. It is dynamically typed, uses `local` for local variables, and has familiar control-flow constructs such as `if`, `for`, and functions. Lua tables are its main compound data structure: they can act as arrays, maps, or records.

Lua is often embedded inside another application rather than run as a separate program. Redis embeds Lua scripting so a client can send a script to Redis and have Redis execute several data operations together on the server.

## What Lua means in Redis

A Redis Lua script runs on the Redis server that receives it. Commands inside the script are issued through Redis's scripting API, most commonly:

- `redis.call(...)` — execute a Redis command; command errors are raised to the script caller.
- `redis.pcall(...)` — execute a Redis command but return a Lua error object instead of raising the command error.
- `KEYS[n]` — the Redis keys explicitly supplied by the client.
- `ARGV[n]` — the non-key arguments explicitly supplied by the client.

Redis scripts are atomic with respect to other commands: Redis does not interleave another client's commands while the script is running. This makes scripts useful when a change involves multiple related keys, such as a job record, two indexes, and two summary counters. Atomic execution is not the same as a general database rollback mechanism: scripts should validate inputs and key types before making writes, and should be kept short because they block command processing while running.

Lua arrays/tables use **1-based** indexing. Thus `KEYS[1]` and `ARGV[1]` refer to the first supplied key and argument, not index zero.

## How our C++ code supplies scripts

In `.7/RedisJobStatus.cc`, scripts are stored as C++ raw string literals. For example:

```cpp
const char* const kRegisterServerScript = R"lua(
local function validtype(key)
  local t = redis.call('TYPE', key).ok
  return t == 'none' or t == 'set'
end

if not validtype(KEYS[1]) or not validtype(KEYS[2]) then
  return redis.error_reply('wrong key type')
end
redis.call('SADD', KEYS[1], ARGV[1])
return redis.call('SADD', KEYS[2], ARGV[2])
)lua";
```

The `R"lua(... )lua"` syntax is a **C++ raw string literal**. It lets us write Lua source without escaping every quote or backslash. The `lua` marker is just the C++ raw-string delimiter; it is not a Lua keyword.

The C++ helper `evalResult()` passes the script, a key range, and an argument range to redis-plus-plus:

```cpp
return redis.template eval<int64_t>(
    script, keys.begin(), keys.end(), args.begin(), args.end());
```

The redis-plus-plus client sends those pieces to Redis. The keys are supplied separately from arguments so the script can use `KEYS` and `ARGV` consistently and Redis Cluster can identify the keys a script operates on. In our code, key names are built in C++ and passed to the script; the scripts do not invent the application's key names.

## Where we use Lua in .7

The scripts are implementation details of `RedisJobStatus`. They keep multi-key lifecycle updates together on Redis rather than issuing each write as a separate C++ client command.

### 1. Create a pending job — `kCreatePendingScript`

This script:

1. Checks whether the job hash already exists.
2. Checks that the index keys are sets (or do not exist) and summary keys are hashes (or do not exist).
3. Builds a list of hash field/value pairs, including the initial `PENDING` status and server ownership.
4. Writes the job hash, adds the composite reference to the type and server sets, and increments pending and total counters for both summaries.

Optional job fields arrive in `ARGV` as alternating field/value pairs. Lua's `table.insert` appends them to the `fields` table, and `unpack(fields)` passes the table's entries as individual arguments to `HSET`.

### 2. Transition a job — `kTransitionScript`

This script verifies the job exists, its stored server type and server ID match the requested owner, and both indexes contain the job. It returns early for an unchanged state and rejects transitions other than:

- `PENDING → ACTIVE`
- `ACTIVE → SUSPENDED`
- `SUSPENDED → ACTIVE`

Before writing, it checks that the old-state and total counters are present and positive. It then updates the job's status and moves one count from the old state to the new state in both the type-level and server-level summaries.

### 3. Complete a job — `kCompleteScript`

This script checks the job, owner, indexes, state and counters. It decrements the current-state and total counters, removes the job ID from both indexes, and deletes the job hash.

### 4. Register a server — `kRegisterServerScript`

This script receives two keys: `servers:{registry}:types` and `servers:{registry}:type:<serverType>`. Both keys share the literal `{registry}` hash tag, so Redis Cluster routes them to the same slot. The script checks that both keys are either absent or sets before writing, then adds the server type to the global discovery set and the server ID to the type-specific set. One script keeps the two registry updates together without a cross-slot error.

## Redis Cluster hash tags: why and how

Redis Cluster distributes keys among hash slots. For a key containing a non-empty substring inside its first matching `{...}` pair, Redis calculates the slot from that substring rather than the entire key. This lets application key names share a slot when they must be used together in a multi-key operation or Lua script.

In `.7`, job lifecycle keys use the server type as their tag. For example, `job:{query}:job-100`, `jobs:type:{query}`, and `summary:type:{query}` share the `query` tag. This keeps a job's record, indexes, and summaries co-located for lifecycle scripts, while types such as `query` and `order` can be partitioned separately.

Server discovery has a different requirement: `registerServer()` must update both the global type set `servers:{registry}:types` and a type-specific set such as `servers:{registry}:type:query`. They use the same literal `{registry}` tag, which makes the two-key Lua script cluster-safe. The trade-off is that all registry keys share one slot; job data is still partitioned by `{serverType}`.

When writing a multi-key script, list every accessed Redis key in `KEYS` and make sure all of those keys use the same hash tag. Braces containing different text do not co-locate keys.

## Lua features used by these scripts

| Lua feature | How it is used |
|---|---|
| `local` | Defines script-local variables and helper functions. |
| `function` | Defines helpers such as `validtype` and `hasCounters`. |
| `if`, `and`, `or`, `not` | Validates key types, job ownership, indexes, states and counters. |
| `for` | Reads optional field/value pairs from `ARGV`. |
| Tables | Collects the fields passed to `HSET`. |
| `string.lower` | Converts status names such as `ACTIVE` to counter field names such as `active`. |
| `tonumber` | Converts counter strings to numbers for validation. |
| `redis.call` | Reads and updates Redis keys and data structures. |
| `return` | Returns a result code or a Redis error reply to the C++ caller. |

The scripts deliberately use a small subset of Lua. Most application logic, identifier validation, key construction, and C++ result interpretation remain in the C++ layer.

## Important implementation notes

- **Pass keys explicitly.** Redis keys accessed by a script should be provided in the key list and referenced through `KEYS`, not assembled from arbitrary data inside Lua.
- **Keep Cluster keys co-located.** Job lifecycle keys use the `{serverType}` hash tag, for example `job:{query}:job-100` and `summary:type:{query}`. The server-discovery keys use the shared literal `{registry}` tag, for example `servers:{registry}:types` and `servers:{registry}:type:query`. Redis Cluster hashes the text inside the first non-empty pair of braces; keys with the same tag map to the same slot. Different tags generally map to different slots, and a script touching keys in different slots will fail in Redis Cluster.
- **Validate before writing.** The scripts check expected key types and important state before changing data. Keep those checks ahead of the writes.
- **Keep scripts short.** Redis runs a script without interleaving other commands. Avoid long loops or expensive work.
- **Treat return values as part of the C++/Lua contract.** The C++ API maps the integer returned by `eval<int64_t>` to its `RedisJobStatus::Result` enum. If a script's return codes change, update and verify the C++ mapping together.
- **Test on the Redis versions you deploy.** Scripting behavior and available commands can depend on Redis version and configuration. Validate against the exact standalone and Cluster environments used by the application.

For the concrete script bodies and the matching C++ call sites, see `.7/RedisJobStatus.cc`.


## .7 configured identity and composite references

The `.7` API captures `serverType` and `serverId` in `RedisJobStatus::Config` at construction. Worker lifecycle calls no longer accept those values repeatedly. The C++ layer builds a composite reference as `<serverId-length>:<serverId>:<jobId>`; the job hash key is `job:{serverType}:<reference>`. Length-prefixing keeps the reference unambiguous even if identifiers contain colons. The hash field `jobId` continues to hold the original application ID.

The type and per-server sets both store the composite reference, not just `jobId`. This allows replicas of the same type to use the same application-generated ID without collisions. All lifecycle keys still use the `{serverType}` hash tag so one Lua script can atomically update the job, indexes, and summaries in the same Redis Cluster slot.
