# Redis Job Design — Continuation Notes

Purpose: capture the invariants, assumptions, implementation boundaries, and open questions for continued work on the `.7` Redis Job Status subsystem. This is an engineering handoff, not a claim that every behavior has passed live Redis integration testing. When this note conflicts with current source, inspect the C++ and Lua implementation and correct this document.

## 1. Code map and boundaries

- `.7/RedisJobStatus.h/.cc`: public lifecycle and dashboard API, identity capture, key construction, validation, and result codes.
- `.7/RedisJobScripts.inc`: Lua scripts implementing multi-key lifecycle mutations and server registration.
- `.7/RedisClient.h/.cc`: process-wide Redis client, standalone/Cluster selection, and configuration.
- `.7/ServerId.h/.cc`: standalone server-ID generation utility.
- `.7/examples/RedisWorkerExample.cc`, `.7/examples/RedisWorkerLifecycleExample.cc`: usage examples.
- `.7/tools/redis_browser/`: underscored Python/Textual browser.
- `.7/tools/redis-browser-2/`: separate hyphenated browser implementation. These are not interchangeable paths.

Keep C++ key arrays, Lua `KEYS[n]` order, `ARGV` order, result codes, field names, and documentation synchronized.

## 2. Domain identity and ownership

- A **server type** identifies a logical worker/service category (for example `query`).
- A **server ID** identifies one running instance within that type. Multiple instances may share a type.
- A **job ID** is application-supplied and is not required to be globally unique.
- A `RedisJobStatus` instance captures its enabled flag, server type, and server ID at construction. Lifecycle calls use this captured identity; they do not accept identity again on every call.
- Dashboard/read methods can query a supplied type and server ID independently of the object's worker identity.
- Job ownership is the pair `(serverType, serverId)`; the job hash records both.

### Composite reference

Indexes store a composite reference, not the plain application job ID:

```text
<serverId-byte-length>:<serverId>:<jobId>
```

Example: `7:query-01:job-100`.

The decimal length prefix disambiguates the server-ID boundary if the ID contains a colon. The length is based on C++ `std::string::size()` (bytes), not Unicode code points. A parser in another language must use compatible byte semantics or constrain identifiers to ASCII. Treat references as opaque unless parsing the length prefix correctly.

The job key is `job:{<serverType>}:<composite-reference>`. This permits the same application job ID on different server IDs without collisions. The hash also stores original `jobId`, `serverType`, and `serverId`.

## 3. Redis key schema

Braces below are literal Redis Cluster hash-tag delimiters; angle-bracket values are placeholders.

| Type | Key | Meaning |
|---|---|---|
| SET | `servers:{registry}:types` | Global set of known server types |
| SET | `servers:{registry}:type:<serverType>` | Registered server IDs for a type |
| HASH | `summary:type:{<serverType>}` | Materialized counters for a type |
| HASH | `summary:server:{<serverType>}:<serverId>` | Materialized counters for one server |
| HASH | `job:{<serverType>}:<composite-reference>` | Job state, ownership, and metadata |
| SET | `jobs:type:{<serverType>}` | Composite references for jobs of a type |
| SET | `jobs:server:{<serverType>}:<serverId>` | Composite references for jobs of a server |
| STRING + TTL | `heartbeat:server:{<serverType>}:<serverId>` | Expiring heartbeat marker |

Summary hash counters are `pending`, `active`, `suspended`, and `total`. They are integer materialized counters, not computed on every read.

## 4. Redis Cluster slot invariants

Redis Cluster uses the first non-empty `{...}` substring as the hash tag. All keys passed to one multi-key Lua script must map to the same slot.

- Job data uses the exact `{serverType}` tag: job hash, both job indexes, type/server summaries, and heartbeat. For example, `job:{query}:...`, `jobs:type:{query}`, `jobs:server:{query}:query-01`, and `summary:type:{query}` share a slot.
- Discovery uses a separate shared `{registry}` tag: `servers:{registry}:types` and `servers:{registry}:type:<serverType>`. This permits one registration script to update both registry sets atomically.
- The registry tag cannot also be the per-type tag for every type. The trade-off is a shared registry slot that may become a hotspot at high registration volume.

Invariant: every new multi-key script must be checked for exact same-slot hash tags. Having braces in every key is not sufficient. Never combine registry keys and server-type keys in one Cluster Lua script.

## 5. Lifecycle state machine

Persistent states are uppercase `PENDING`, `ACTIVE`, and `SUSPENDED`.

```text
create -> PENDING
PENDING   -> ACTIVE
ACTIVE    -> SUSPENDED
SUSPENDED -> ACTIVE
complete  -> delete job
```

- Creation starts in PENDING.
- Direct PENDING to SUSPENDED is unsupported.
- Repeating the current state is a NoOp and must not alter counters.
- A transition request to PENDING is invalid.
- Completion deletes the job; it is not a persistent fourth state.
- Normal completion expects the job to be present in both type and server indexes.
- Stored owner and status must agree with the requested operation.
- Missing/unknown status or inconsistent index/counter data must not be silently treated as success.

The public `Result` enum numeric values are a cross-language contract and should remain stable: 0 Success, 1 NoOp, 2 JobNotFound, 3 InvalidTransition, 4 InconsistentData, 5 JobAlreadyExists, 6 Disabled. Change C++ and Lua together if this contract is intentionally revised.

## 6. Mutation invariants and Lua behavior

Lifecycle create, transition, and complete are Lua scripts. C++ supplies explicit key arrays and arguments; preserve exact ordering between C++ and Lua.

### Create success effects

1. Create the job hash with reserved fields `jobId`, `status=PENDING`, `serverType`, and `serverId`.
2. SADD the composite reference to the type and server job indexes.
3. Increment `pending` and `total` in both type and server summaries.

An existing job key returns JobAlreadyExists. Extra metadata is supplied as field/value arguments.

### Transition success effects

1. Validate job existence, ownership, status, both index memberships, key types, and counter preconditions.
2. Change the job hash status.
3. Decrement the old status and increment the new status in both summaries.

Indexes and total counters do not change.

### Complete success effects

1. Validate existence, owner, status, both index memberships, key types, and relevant counters.
2. Decrement current status and total in both summaries.
3. SREM the reference from both indexes.
4. DEL the job hash.

### Atomicity caveat

Redis runs a Lua script without interleaving other commands on that node, but Lua is not a rollback transaction. A runtime error after writes can leave partial effects. Validate all expected key types, ownership, state, memberships, and counter preconditions before the first write. Keep scripts short. Do not replace script-backed lifecycle operations with independent client commands; that introduces race and partial-update windows.

## 7. Server registration and heartbeat

- Registration SADDs the type to `servers:{registry}:types` and the server ID to `servers:{registry}:type:<serverType>`.
- Both registry keys must be SET-or-absent before writes; both share the registry slot.
- SADD makes registration idempotent at the membership level.
- Registration does not itself create a job or summary.
- Heartbeat uses `heartbeat:server:{serverType}:<serverId>` with a positive TTL. Non-positive TTL is invalid.
- The example uses a 30-second TTL refreshed every 10 seconds; these are example values, not universal constants.
- Heartbeat is an expiring liveness signal, not proof of registration or counter correctness.
- There is no explicit unregister/shutdown API in the described design. Heartbeats expire, but registry membership can remain stale.

## 8. Identifier and metadata assumptions

Current validation requires non-empty identifiers and rejects `{` or `}` in server type because it is a Cluster hash tag. Server ID and job ID are not currently described as having the same brace restriction. The design assumes ordinary application strings, not arbitrary binary identifiers. Consider defining maximum lengths, encoding, whitespace/control-character rules, and whether braces should be rejected in all IDs.

Metadata is a `std::map<std::string,std::string>`: field names are unique and iteration order deterministic. Reserved fields are `jobId`, `status`, `serverType`, and `serverId`; caller metadata must not override them. Metadata is stored as string fields in the job hash. Renaming reserved fields requires coordinated reader/script/docs updates.

The example `makeServerId()` combines host IPv4 with five random lowercase letters. This is intended to make IDs distinct but is not a mathematical uniqueness guarantee. Applications must avoid reusing a server ID while stale records for that identity remain.

## 9. Configuration and process lifecycle

### RedisJobStatus

Default configuration keys: `redisJobStatusEnabled`, `redisJobStatusServerType`, `redisJobStatusServerId`.

Enablement accepts case-insensitive true/false, 1/0, yes/no. Empty/unset defaults to enabled; other non-empty values are invalid. When disabled, construction should not initialize/access RedisClient; result-returning mutators return Disabled, void mutators do nothing, and reads return empty results. Disabled use should not require Redis configuration or connectivity.

Configuration is captured at object construction; changing the host registry later does not mutate an existing object.

### RedisClient

Default keys: `redisUrl`, `isRedisCluster`. URL must be non-empty. Cluster flag accepts case-insensitive true/false, 1/0, yes/no; empty defaults to standalone.

`RedisClient::Instance()` is a lazy process-wide singleton. The provider on the first call determines configuration; later providers are ignored. Injected test providers must be installed before any other code initializes the singleton. Different Redis endpoints may require process isolation or a future injectable non-singleton seam.

The host application must provide `get_reg_val(const std::string&)` and initialize its configuration registry before the first default-provider call.

## 10. Read API and dashboard consistency

- `getServerTypes()` reads the global registry type set.
- `getTypeSummary(type)` reads the materialized type summary.
- `getServerSummaries(type)` enumerates registered server IDs and reads their summaries.
- `getServerJobs(type,id)` enumerates composite references in that server's job set and reads each hash.
- These multi-key reads are not a snapshot; concurrent mutations can occur between commands.
- Reads trust indexes and materialized counters maintained by lifecycle scripts; they are not a reconciliation/repair subsystem.
- Registry membership can outlive heartbeat and job summary keys.
- `getServerJobs` has no pagination and may load all jobs for a server; large result sets can consume memory and time.
- Counters are not recomputed by scanning all job hashes.

## 11. Python browser tools and destructive-operation status

The underscored browser at `.7/tools/redis_browser/` is a separate implementation from `.7/tools/redis-browser-2/`. The underscored implementation's README describes it as read-only. It scans known `.7` key patterns with SCAN, not KEYS; supports standalone/Cluster via redis-py; reads `REDIS_URL` (default `redis://127.0.0.1:6379`) and `REDIS_CLUSTER` (1/true/yes enables cluster mode). Auto-refresh defaults to two seconds and may add load. Raw-key view supports common Redis data types.

A D-key cascading delete/confirmation implementation was previously committed only to the hyphenated browser path in commits `d5dcb38277c7139d0cb41be98d47d492b398f647` and `e2ebe249fa0f7ff74464c2a7140fb48de3c11c6e`. This does not mean the feature exists in `.7/tools/redis_browser/redis_browser.py`. The user explicitly identified the underscored path; keep the distinction clear.

### Requested cascade behavior — intent, not verified in underscored browser

Press `d` on a selected hierarchy item; show confirmation; Y confirms, N/Esc cancels.

- Job: delete its hash, remove its composite reference from both indexes, decrement the matching status and total in both summaries.
- Server: cascade through its jobs, then delete its job index, server summary, heartbeat, and registry membership; decrement type-level counters for removed jobs.
- Type: cascade through its servers, clean remaining type job references, then remove type summary, type job index, per-type registry set, and global type membership.

This is destructive and the earlier browser implementation used multiple client commands, not an atomic script. Registry keys use `{registry}`, while job/summary keys use `{serverType}`; one Lua script cannot atomically cover both slots in Cluster. A production cascade therefore needs a multi-phase, retryable design with explicit partial-failure handling. Define behavior for stale indexes, orphan hashes, malformed metadata, unknown status, ownership mismatch, missing keys, and already-inconsistent counters. Consider dry-run counts and confirmation text that names the exact scope. Never claim this feature exists in the underscored browser until that exact file is changed and tested.

## 12. Assumptions, limitations, and open questions

1. Server ID uniqueness is application-managed; the sample generator has a non-zero collision probability.
2. Job IDs need only be unique within a server identity; the composite reference supports reuse by another server.
3. Server type is a partition key; renaming it requires a data migration.
4. Counters are materialized; no production reconciliation/repair API is defined.
5. Dashboard reads are not snapshot-consistent.
6. There is no C++ unregister API or automated stale-registry cleanup.
7. Heartbeat TTL/cadence are application policy.
8. Completed jobs are deleted immediately; no history/archive is defined.
9. Large job lists are unpaginated.
10. Global registry writes share one Cluster slot.
11. Lua scripts do not roll back runtime errors after writes.
12. External writers can corrupt types, indexes, reserved fields, and counters; schema ownership is assumed.
13. Identifier character/length/encoding policy is incomplete.
14. Composite length prefix counts bytes, not Unicode code points.
15. Empty enablement means enabled; operators must explicitly disable.
16. Singleton first-initialization wins, which affects test ordering.
17. Browser delete behavior is path-specific and not yet verified in the underscored implementation.
18. Redis ACLs, cluster topology discovery, library versions, and node network access are deployment dependencies.
19. SCAN avoids KEYS blocking behavior but repeated scans/detail reads and auto-refresh still have cost.
20. Do not claim live standalone or Cluster integration correctness without actually running those tests.

## 13. Validation checklist for continued work

### C++/Lua lifecycle

- Create produces one hash, both index memberships, and correct type/server counters.
- Duplicate create returns JobAlreadyExists with no counter change.
- Same application job ID on two server IDs does not collide.
- Every allowed transition updates both summaries exactly once.
- Repeating the same state returns NoOp without mutation.
- Invalid transitions leave all data unchanged.
- Completion removes hash and both memberships and decrements status/total once.
- Missing jobs return JobNotFound.
- Wrong key types, mismatched owner, unknown status, missing index member, and invalid counters produce controlled inconsistent/error results without writes.
- Registration is idempotent and updates both registry sets.
- Heartbeat TTL is positive; zero/negative values are rejected.
- Disabled mode performs no Redis access and returns documented empty/no-op results.
- Test all accepted config spellings, defaults, and invalid values.
- Run on standalone and Redis Cluster; verify all KEYS of every script share one slot.

### Browser and destructive cascade

- Confirm exact directory and dependency versions before editing.
- Test hierarchy navigation, selection retention, refresh, empty/missing data.
- For delete: test Y/N/Esc, stale selection, concurrent worker changes, missing keys, inconsistent counters, duplicate job IDs, and server/type cascades.
- Use disposable Redis data for destructive tests.
- Explicitly test and document Cluster partial failure across registry and server-type slots.
- Do not describe a cascade as atomic unless the architecture truly provides that guarantee.

## 14. Working rules

- Preserve C++/Lua key and argument order, enum values, reserved fields, and state names.
- Keep every multi-key script within one exact Cluster hash tag.
- Do not bypass script consistency checks with client-side lifecycle mutations.
- Treat server type as the partition boundary.
- Treat composite references as opaque unless parsed using the byte-length prefix.
- Do not conflate the hyphenated and underscored browser directories.
- Distinguish implemented/verified behavior from proposed behavior.
- Update this handoff when assumptions or design decisions change.
- Never report tests as run unless the exact test and Redis topology were exercised.
