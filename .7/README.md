# Redis Job Status API (.7)

## Optional RedisJobStatus configuration

Set the host registry value `redisJobStatusEnabled` to `true` or `false` (also accepts `1/0` and `yes/no`, case-insensitively). An unset or empty value defaults to `true` for compatibility with existing deployments.

When enabled, `RedisJobStatus` obtains the configured `RedisClient` and runs the normal Redis operations. When disabled, it does not initialize or access `RedisClient`: result-returning mutators return `RedisJobStatus::Result::Disabled`, void mutators do nothing, and read methods return empty collections. This makes status reporting optional without requiring Redis configuration or a reachable Redis server when the feature is off.

The configuration is read when the `RedisJobStatus` object is constructed; changing the registry later does not change an existing object. Tests/examples can inject a provider:

```cpp
RedisJobStatus status([]
{
  return RedisJobStatus::Config{false, "query", "query-01"};
});
```

## RedisClient configuration injection

Pass an optional provider to `RedisClient::Instance()` on its first call. This keeps `RedisClient` small: it accepts a callable returning `RedisClient::Config`, while the default provider calls the host application's `get_reg_val("redisUrl")` and `get_reg_val("isRedisCluster")`.

Production usage:

```cpp
// Initialize the application registry first, including redisJobStatusEnabled.
// Set redisJobStatusEnabled=false to disable status reporting without Redis.
RedisJobStatus status; // reads enablement, fixed identity, then RedisClient settings

auto client = RedisClient::Instance(); // optional direct access
```

Tests and examples can pass a provider returning deterministic mock values. The first `Instance()` call initializes the singleton; providers passed on later calls are ignored. The default provider accepts `true`/`false`, `1`/`0`, or `yes`/`no` for `isRedisCluster`.

## Files

- `RedisClient.h/.cc`: client selection and injectable configuration.
- `RedisJobStatus.h/.cc`: optional job lifecycle and dashboard APIs; controlled by `redisJobStatusEnabled`.
- `RedisJobScripts.inc`: Lua scripts stored as C++ raw string literals and included by `RedisJobStatus.cc` to keep the main implementation readable without a generator or runtime script files.
- `examples/RedisWorkerExample.cc`: worker registration and heartbeat.
- `examples/RedisStatusCli.cc`: interactive type → instance → jobs drill-down.
- `examples/RedisWorkerLifecycleExample.cc`: end-to-end demonstration of every public RedisJobStatus API.

## Lifecycle example

Run with `--statusEnabled=false` to demonstrate the disabled path without connecting to Redis. The default is enabled.

```sh
./RedisWorkerLifecycleExample --serverType=query-server-type1
./RedisWorkerLifecycleExample --serverType=query-server-type1 --redisUrl=redis://127.0.0.1:6379 --cluster=false
./RedisWorkerLifecycleExample --serverType=query-server-type1 --statusEnabled=false
```

When enabled, the example injects mock values for `redisUrl` and `isRedisCluster` and calls `registerServer`, `refreshHeartbeat`, `createPendingJob`, `getServerTypes`, `getTypeSummary`, `getServerSummaries`, `getServerJobs`, all valid `transitionJob` transitions, and `completeJob`. With `--statusEnabled=false`, it skips Redis client initialization, shows the `Disabled` result, and verifies that reads are empty and void operations are no-ops. The enabled path uses a deterministic demo job ID, so rerunning against the same Redis data may report `JobAlreadyExists`; use a fresh server type or remove the prior demo record.

## Redis data structures

| Type | Key pattern | Purpose | Sample data held |
|---|---|---|---|
| SET | `servers:{registry}:types` | Global server-type discovery. | `query`, `order` |
| SET | `servers:{registry}:type:serverType` | Registered instance IDs for one type. | For `query`: `query-01`, `query-02` |
| HASH | `summary:type:{serverType}` | Type-level status counters. | `pending=2`, `active=3`, `suspended=1`, `total=6` |
| HASH | `summary:server:{serverType}:serverId` | Per-instance counters. | For `query-01`: `pending=1`, `active=2`, `total=3` |
| HASH | `job:{serverType}:<serverId-length>:<serverId>:<jobId>` | Job state, ownership and metadata. | `jobId=job-100`, `status=ACTIVE`, `serverType=query`, `serverId=query-01`, plus custom fields |
| SET | `jobs:type:{serverType}` | Composite references for current jobs of a type. | `7:query-01:job-100`, `7:query-01:job-101`, `7:query-02:job-100` |
| SET | `jobs:server:{serverType}:serverId` | Composite references for jobs owned by an instance. | For `query-01`: `7:query-01:job-100`, `7:query-01:job-102` |
| String + TTL | `heartbeat:server:{serverType}:serverId` | Heartbeat health signal. | `query-01` with a 30-second TTL |

## Redis Cluster hash-tag scheme: why and how

Redis Cluster assigns each key to a hash slot. Normally the slot is calculated from the key name, but when a key contains a non-empty substring inside the first matching pair of curly braces, Redis hashes that substring instead. This substring is called the **hash tag**. Keys with the same tag are placed in the same slot, which is required for a Lua script or multi-key command to operate on them together in Redis Cluster.

### Job lifecycle keys use the server type

Job-related keys use `{serverType}` as their hash tag. For example, `job:{query}:job-100`, `jobs:type:{query}`, `jobs:server:{query}:query-01`, `summary:type:{query}`, and `summary:server:{query}:query-01` all hash using `query`. This co-locates the job record, its indexes and its counters so the create, transition and completion scripts can update those related keys together. Different server types, such as `query` and `order`, can be distributed across different slots.

### Server discovery uses a shared registry tag

Server registration needs to update two keys: the global type set `servers:{registry}:types` and a type-specific set such as `servers:{registry}:type:query`. Both use the exact literal tag `{registry}`, so they occupy the same Redis Cluster slot. `registerServer()` passes both keys explicitly to one Lua script, which checks that both are either absent or sets before adding the type and server ID. This avoids the previous two-operation gap, where one registry could be updated while the other failed, and avoids a Cluster cross-slot error.

The registry tag is intentionally different from the `{serverType}` tag: a single global type-discovery set cannot share a slot with every type-partitioned job key. The trade-off is that all server-registry keys share one slot, which could become a hotspot if registration traffic is very high. Job lifecycle data remains partitioned by server type. Lua scripts execute without interleaving from other Redis commands, but they are not general rollback transactions, so the script validates both key types before performing writes.

When adding keys to a script, pass them in the explicit `KEYS` list and ensure every key belongs to the same hash slot. Do not assume that merely having braces somewhere in each key is enough: the text inside the braces must match exactly.

## Lua scripts: keys, arguments and tables

The Lua scripts are kept in `RedisJobScripts.inc` as C++ raw string literals. This keeps them together and out of the main implementation file, while avoiding a generator and runtime file-path dependencies. Edit the script text in that include file; it is not a standalone `.lua` file.


redis-plus-plus `eval` passes the Lua script, a key range and an argument range. `KEYS[1]`/ `KEYS[2]` are supplied Redis keys; `ARGV[1]`/ `ARGV[2]` are non-key arguments. Lua tables are one-indexed arrays. For example, `local fields = {'status', 'PENDING'}` and `redis.call('HSET', KEYS[1], unpack(fields))` expands the table into the arguments to `HSET`. Other calls include `SADD` for set membership, `HGET` for a field read, and `HINCRBY` for counters. Keep scripts short: they execute atomically on the serving node but block other commands while running.

## Build and validation

Requires C++17, redis-plus-plus and hiredis. The host application must provide `std::string get_reg_val(const std::string&)` and initialize its configuration registry before the first `RedisClient::Instance()` call. Build flags depend on your installation. These sources have not been compiled or integration-tested as part of this repository update; test against the exact client version and both standalone and cluster deployments before production.


## Fixed server identity and duplicate application IDs

The worker identity is configured once using `RedisJobStatus::Config` or host registry values `redisJobStatusServerType` and `redisJobStatusServerId`. A worker then calls `createPendingJob(jobId, fields)`, `transitionJob(jobId, state)`, `completeJob(jobId)`, `registerServer()`, and `refreshHeartbeat(ttlSeconds)` without repeating identity arguments.

The storage key uses a length-prefixed composite reference derived from the configured `serverId` and the original `jobId`. Both indexes store that composite reference. The hash field `jobId` remains the original application-generated value, so UI display is unchanged. All related keys retain the `{serverType}` hash tag.
