# redis-cli hierarchy scripts

This folder contains a read-only command-line view of the **3-colocated**
RedisJobStatus key layout.

## Files

- `show-job-status.sh` — displays server types, their registered server IDs,
  lifecycle counters, server state, heartbeat TTL, and each server's indexed
  jobs. It uses only `redis-cli` commands and does not require Python,
  redis-py, or Textual.

## Requirements

- Bash 4 or newer
- `redis-cli`
- For container-only Redis CLI usage, `podman` on the host and the name of
  the running Redis container

## Run with redis-cli installed on the host

Make the script executable, then run it:

```bash
chmod +x show-job-status.sh
REDIS_HOST=127.0.0.1 REDIS_PORT=6379 ./show-job-status.sh
```

## Run when redis-cli exists only inside the Redis Podman container

Run the script on the Linux host. Set `REDIS_CONTAINER` to the container name
(or ID); the script invokes `podman exec` to run `redis-cli` inside it:

```bash
chmod +x show-job-status.sh
REDIS_CONTAINER=my-redis ./show-job-status.sh
```

Replace `my-redis` with the name shown by:

```bash
podman ps
```

The Redis CLI inside the container connects to its local Redis server. If your
container uses a non-default Redis port, add `REDIS_PORT` only when using a
host-installed CLI; for a container-internal CLI, configure the Redis server
and container networking accordingly.

## Authentication and database selection

Optional environment variables:

| Variable | Default | Purpose |
|---|---:|---|
| `REDIS_HOST` | `127.0.0.1` | Host for host-installed redis-cli |
| `REDIS_PORT` | `6379` | Port for host-installed redis-cli |
| `REDIS_DB` | `0` | Redis database number |
| `REDIS_CONTAINER` | empty | Podman container name/ID; use container's redis-cli |
| `REDIS_USERNAME` | empty | ACL username |
| `REDIS_PASSWORD` | empty | Password; passed through `REDISCLI_AUTH` |

Examples:

```bash
REDIS_CONTAINER=my-redis REDIS_USERNAME=default REDIS_PASSWORD='secret' ./show-job-status.sh
REDIS_HOST=10.0.0.20 REDIS_PORT=6380 REDIS_DB=2 ./show-job-status.sh
```

Avoid putting passwords directly into shell history. Prefer exporting
`REDIS_PASSWORD` from a protected environment or secret manager.

## Example output

```text
Redis Job Status (3-colocated)
Connection: Podman container my-redis

SERVER TYPE / SERVER ID / JOB                                PENDING   ACTIVE  SUSPENDED    TOTAL  STATE / HEARTBEAT
--------------------------------------------------------------------------------------------------------------------------------
orders                                                           12        5          2       19
  server-101                                                      3        2          1        6  ACTIVE / 18s
    JOB: job-001 [ACTIVE]
    JOB: job-002 [PENDING]
  server-102                                                      9        3          1       13  ACTIVE / 21s

Read-only view; no Redis keys are modified.
```

## Notes and limitations

- This reads the existing `servers:{registry}:types`,
  `servers:{registry}:type:<type>`, type/server summary hashes, server state,
  heartbeat keys, per-server job indexes, and job hashes used by
  `3-colocated`.
- The view is a point-in-time-ish sequence of ordinary read commands, not an
  atomic snapshot. Counters and indexes can change while it is printing.
- It displays jobs through each server's `jobs:server:{<type>}:<serverId>`
  index. It does not display a separate type-wide jobs listing or provide the
  browser's interactive expand/collapse and `j` view toggle.
- The script is read-only. It does not delete jobs, servers, or keys.
