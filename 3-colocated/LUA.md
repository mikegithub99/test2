# Redis Lua Scripts

## Why Lua

Redis Lua scripts are a good fit when an operation must **read state, make decisions from those reads, validate data, and conditionally update multiple related keys as one operation**.

A Lua script runs inside Redis. It can:
- Read server state, counters, job records, and indexes.
- Branch on the values it just read.
- Loop over jobs and calculate actual counts.
- Validate ownership, status, key types, and counter bounds before writing.
- Apply the related updates in the same atomic execution.

For example, cleanup can verify that a server is `DELETING`, inspect its jobs, count jobs by status, check that type-level counters can safely be decremented, and then remove the jobs and update counters. The checks and writes cannot be interleaved with commands from other clients.

Redis executes a Lua script atomically with respect to other commands. This does **not** mean rollback: if a script encounters a runtime error after it has begun writing, earlier writes are not automatically undone. Scripts should validate likely failure conditions before the first mutation and remain reasonably short.

## Transactions (MULTI/EXEC) vs. Lua

A Redis transaction using `MULTI/EXEC` queues commands and executes the queued command sequence atomically with respect to other clients. For example:

```cpp
auto tx = redis.transaction(key);
tx.del(key);
tx.rpush(key, values);
tx.exec();
```

This is suitable for a straightforward fixed sequence, such as replacing a list's contents.

The limitation is that a transaction cannot use the result of one queued command to decide whether to queue or execute a later command within that same `EXEC`. The application receives command results after execution. `WATCH` can support optimistic check-and-set workflows, but the application must read and evaluate values, attempt the transaction, and retry if a watched key changed.

| Capability | MULTI/EXEC | Lua |
|---|---|---|
| Execute multiple commands atomically relative to other clients | Yes | Yes |
| Branch on values read during the operation | Not inside the queued transaction | Yes |
| Loop over job records inside Redis | No; application coordinates it | Yes |
| Validate several conditions before writing | Requires application-side coordination (often with WATCH/retries) | Natural fit |
| Fixed sequence of commands | Good fit | Also possible |

## Why Lua for this job lifecycle

The job lifecycle and cleanup operations are not just fixed command sequences. They need conditional logic across related keys: check that the server is `ACTIVE`, validate job ownership and index membership, inspect statuses, calculate actual job counts, verify counter balances, and only then update the records and summaries.

Lua keeps that decision-making and the resulting writes together in Redis. In this Redis Cluster design, all keys touched by a given script must also be in the same hash slot; the per-server-type keys use the `{serverType}` hash tag for that reason.

**In short:** use `MULTI/EXEC` for a known sequence of commands; use Lua when Redis must inspect data, make decisions based on it, and apply conditional multi-key changes atomically.
