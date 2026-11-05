# START HERE — Redis Job Status work (updated handoff)

This file is the corrected handoff for future ChatGPT sessions working in this repository. Read it before making changes.

## Repository and working rules

- Repository: `mikegithub99/test2`
- Active branch: `main`
- Primary working directory for this work: `SUMMARY.7/`
- `SUMMARY.7/` is the requested working copy. Make requested changes there unless the user explicitly directs otherwise.
- `.7/` is a separate directory in the repository. It is **not an automatic mirror** of `SUMMARY.7/`, and changes made in `SUMMARY.7/` do **not** have to be copied to `.7/`.
- Do not treat historical notes, matching filenames, or identical current file contents as a standing synchronization requirement. Some corresponding files may happen to be identical at a given commit; that does not create an obligation to keep them synchronized.
- Only inspect or change files under `.7/` when the user explicitly asks for that directory, or when a concrete dependency or requested cross-directory task makes it necessary. If that happens, explain the reason and scope rather than assuming mirroring.
- Do not make unrelated changes or refactor outside the requested scope.
- Changes are pushed directly to GitHub `main` using the connected GitHub integration. For updates, fetch the current file first and use its current blob SHA as required by the update operation. For new files, use the create-file operation.
- **Always report the full commit SHA after every push.** If multiple file commits are created, identify the last commit SHA and list the others when useful. Do not say a change was tested unless it actually was.

## Repository entry points and layout

- The repository-level handoff is `START_HERE.md` at the repository root.
- The root-level `README.md` was not found when checked on `main`. The project README is `SUMMARY.7/README.md`.
- The main implementation and documentation for this task are under `SUMMARY.7/`.
- `.7/` is a separate repository directory, not a required mirror target.

Important files in `SUMMARY.7/` include:

- `RedisClient.h/.cc` — Redis client selection/configuration and singleton access.
- `RedisJobStatus.h/.cc` — public job lifecycle and dashboard APIs and their C++ implementation.
- `RedisJobScripts.inc` — embedded Lua scripts, included by `RedisJobStatus.cc` as C++ raw string literals. Edit the embedded script here; it is not a standalone Lua file.
- `redis_browser.py` — interactive Redis browser, including metadata information popup.
- `redis_job_correctness.py` — correctness/consistency checks for Redis job data.
- `README.md` — design, usage, key layout, and build/validation notes.
- `examples/` — worker, CLI, and lifecycle examples.

The project README describes the feature as requiring C++17, redis-plus-plus and hiredis, with the host application providing `get_reg_val(const std::string&)`.

## Current public API and behavior

`RedisJobStatus::Config` contains only `enabled` (defaults to true). The constructor's optional `ConfigProvider` reads only `redisJobStatusEnabled` by default. Server type and server ID are passed explicitly to `registerServer(serverType, serverId, ...)`; neither is read from the environment or host registry.

Enablement accepts true/false, 1/0, and yes/no (case-insensitive); empty/unset defaults to enabled. Configuration is captured when the object is constructed. When disabled, Redis is not initialized/accessed: result-returning mutators return `Result::Disabled`, void mutators do nothing, and read APIs return empty collections.

The main API includes:
- `createPendingJob(jobId, metadata)`
- `transitionJob(jobId, State)`
- `completeJob(jobId)`
- `registerServer(serverType, serverId, serverTypeMetadata, serverMetadata)`
- `refreshHeartbeat(ttlSeconds)`
- `getServerTypes()`
- `getTypeSummary(serverType)`
- `getServerSummaries(serverType)`
- `getServerJobs(serverType, serverId)`

Valid lifecycle transitions are PENDING → ACTIVE, ACTIVE → SUSPENDED, and SUSPENDED → ACTIVE. Repeating the current state returns NoOp. The result enum values are part of the C++/Lua contract and must remain aligned:
- Success=0
- NoOp=1
- JobNotFound=2
- InvalidTransition=3
- InconsistentData=4
- JobAlreadyExists=5
- Disabled=6

Worker identity is set explicitly by `registerServer(serverType, serverId, ...)` and retained by the `RedisJobStatus` object. Job storage uses a length-prefixed composite reference derived from server ID and original job ID, while the hash's `jobId` field preserves the original application job ID for display.

## Redis key layout and Cluster constraints

- Global type discovery: `servers:{registry}:types`
- Registered server IDs for a type: `servers:{registry}:type:<serverType>`
- Type summary: `summary:type:{<serverType>}`
- Server summary: `summary:server:{<serverType>}:<serverId>`
- Job hash: `job:{<serverType>}:<serverId-length>:<serverId>:<jobId>`
- Type job index: `jobs:type:{<serverType>}`
- Server job index: `jobs:server:{<serverType>}:<serverId>`
- Heartbeat: `heartbeat:server:{<serverType>}:<serverId>`

All lifecycle keys for a server type share the `{serverType}` hash tag, allowing their Lua scripts to operate in one Redis Cluster slot. Registry keys share the literal `{registry}` tag so global and per-type server registration can be updated atomically. When adding keys to a Lua script, include them explicitly in `KEYS` and ensure every key uses the same hash tag/slot. Lua scripts execute atomically with respect to other Redis commands but are not general rollback transactions; validate key types and state before writing.

## Metadata feature — current intended semantics

Server metadata is supplied to `registerServer()`; job metadata is supplied to `createPendingJob()`:

1. **Job fields** are stored on the job hash as before.
2. **Server-type metadata** is stored in `summary:type:{serverType}` during registration using hash fields prefixed with `meta:`. It is merged: supplied values replace matching metadata fields, while unspecified metadata fields remain.
3. **Server-ID metadata** is merged into that server summary during registration. An empty map adds no custom metadata fields.
4. When a type summary is first created, the Lua script automatically writes:
   - `meta:createdByServerId` — the server ID that caused initial creation.
   - `meta:createdAt` — UTC ISO-8601 timestamp.
   These are reserved system fields; caller-supplied type metadata must not overwrite them.
5. Metadata values are stored in Redis hashes as strings.

The browser supports pressing `i` on a server-type or server-ID row to open a metadata popup; Enter dismisses it. Metadata fields are read from `meta:` hash entries. The correctness checker should not interpret `meta:` fields as numeric counters.

## Working-copy scope and observed directory state

The directory policy above is intentional: make the requested change in `SUMMARY.7/` and do not propagate it to `.7/` by default.

At the time this handoff was prepared, these corresponding pairs on `main` had identical Git blob SHAs:
- `.7/RedisJobStatus.h` and `SUMMARY.7/RedisJobStatus.h`
- `.7/RedisJobStatus.cc` and `SUMMARY.7/RedisJobStatus.cc`
- `.7/RedisJobScripts.inc` and `SUMMARY.7/RedisJobScripts.inc`
- `.7/README.md` and `SUMMARY.7/README.md`

This is only a snapshot of those files at that time. It is not evidence that every file has a counterpart, nor a rule requiring future changes to be mirrored. For example, `.7/RedisClient.h` and `.7/RedisClient.cc` existed when checked, while the corresponding `SUMMARY.7/` paths returned not found. Always verify the specific paths relevant to a task; never infer a mirror relationship from directory names.

## Validation status and next-session workflow

- No C++ build, Python test suite, Redis integration test, or Redis Cluster test has been run for the metadata changes described above. Do not imply otherwise.
- The README contains lifecycle example commands and build prerequisites. The example's enabled path uses a deterministic demo job ID; reruns against the same Redis data may return JobAlreadyExists.
- Before changing code, fetch the current versions of the relevant files from `main`; do not assume this handoff file's details are still current.
- Work within `SUMMARY.7/` unless the user explicitly broadens the scope.
- Keep C++ and Lua argument order/counts synchronized. The create Lua script currently expects five keys and fixed arguments for job ID, server type, server ID, composite reference, job-field count, type-metadata count, server-metadata count, and creation timestamp, followed by the respective field/value pairs. Verify the C++ argument construction before modifying either side.
- Preserve Redis Cluster hash-tag correctness and the numeric C++/Lua result-code contract.
- After pushing, report the full commit SHA explicitly, and clearly distinguish pushed changes from tests actually run.
