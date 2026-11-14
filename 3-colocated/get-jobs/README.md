# Best-effort job retrieval

This note describes the job-read changes in the parent `3-colocated` implementation and the intended use in a live job monitor.

## APIs

```cpp
std::vector<Job> getServerJobs(
    const std::string& serverType,
    const std::string& serverId);

std::vector<Job> getServerTypeJobs(const std::string& serverType);
```

- `getServerJobs()` reads jobs indexed by `jobs:server:{<serverType>}:<serverId>`.
- `getServerTypeJobs()` reads jobs indexed by `jobs:type:{<serverType>}`.

Both return job hash contents as `RedisJobStatus::Job` objects, not just references. The references are internal lookup identifiers used to construct the job hash keys.

## Intended use: live job monitoring

These APIs are designed to support a display of transient jobs as they progress through the system. A client can refresh either view periodically:

- A server view displays the jobs currently associated with one server.
- A type view displays jobs across all servers of that type.
- Each refresh provides the latest observed status and metadata for the jobs returned by that read.

Treat each result as the latest observed view, not as an event log or an audit record. If a job moves through multiple states between refreshes, the client may observe only its later state. For example, a quick `PENDING` → `ACTIVE` → completed progression may happen between two polls, so the UI is not guaranteed to display every intermediate state. That is expected for a live, best-effort view.

Best-effort retrieval does not weaken the job lifecycle writes. Creation, transitions, and completion continue to use their Lua scripts to update the job hash, indexes, and related counters atomically.

## Best-effort semantics under concurrent job churn

The C++ caller first reads the relevant index with `SMEMBERS`. It then constructs an explicit key list containing the index key and one job hash key for each discovered reference. The references are passed in `ARGV`.

The Lua snapshot script processes that candidate list:

1. It verifies the index key is either absent or a Set.
2. For each candidate reference, it checks current membership in the index.
3. If the reference is no longer indexed, it skips it.
4. If the reference remains indexed but its job hash has disappeared, it skips it.
5. If the hash exists, it reads and returns its fields.
6. A wrong-type index or wrong-type job key remains an integrity error.

Consequently, normal concurrent completion/removal does not make the read fail merely because a candidate disappeared. A job created after the C++ `SMEMBERS` is not in the candidate list and is not returned.

The result is deliberately **best effort**, not a complete point-in-time view of all jobs present at Lua execution time. It contains the initially enumerated candidates that are still indexed and have a hash when the Lua script processes them. The entire filtering and hash-reading phase is one atomic Lua execution, so no Redis command interleaves with that phase.

## Ownership and integrity checks

C++ validates the returned hash fields:

- `getServerJobs()` requires both `serverType` and `serverId` to match the requested owner.
- `getServerTypeJobs()` requires `serverType` to match. It intentionally accepts jobs from any `serverId` of that type.

A missing or mismatched ownership field is treated as inconsistent data, not normal churn. Wrong Redis key types are also errors rather than silently ignored.

## Redis Cluster key discipline

The index and every job hash key are explicitly supplied to Lua through `KEYS`. The script does not derive or discover job hash key names dynamically. All keys use the `{serverType}` hash tag, so the index and job hashes are colocated in one Redis Cluster slot.

## Operational considerations

- Both methods currently enumerate the entire relevant Set and pass all candidate job keys to one Lua invocation. Large indexes can create large client requests, Lua work, and replies; bounded pagination may be appropriate as job counts grow.
- A job added after the initial enumeration is omitted until a subsequent refresh.
- A candidate removed before Lua checks it is omitted.
- A candidate still indexed with a valid hash is returned from the Lua read.
- The API does not retry on normal membership changes; those changes are filtered rather than treated as snapshot failures.
- Refresh frequency is a client/UI decision. Polling more frequently can make the display more responsive, at the cost of more Redis read traffic. The client should tolerate jobs appearing or disappearing between refreshes.

## Files changed

- `../RedisJobStatus.h`: documents best-effort semantics and declares `getServerTypeJobs()`.
- `../RedisJobStatus.cc`: shares snapshot parsing and implements both APIs using the common Lua script.
- `../RedisJobScripts.inc`: replaces strict cardinality/membership validation with stale-candidate filtering.

No unrelated source files are changed by this job-retrieval update.
