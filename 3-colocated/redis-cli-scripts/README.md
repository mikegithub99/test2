# redis-cli hierarchy scripts

Read-only command-line viewers for the **3-colocated** RedisJobStatus key
layout. Both scripts use Bash and `redis-cli`; neither requires Python or
redis-py.

## Run from the Linux host

Use the regular script with a host-installed Redis CLI:

```bash
chmod +x show-job-status.sh
REDIS_HOST=127.0.0.1 REDIS_PORT=6379 ./show-job-status.sh
```

## Run from inside the Redis Podman container

Use `show-job-status-in-container.sh`. Copy or mount this folder into the
container, then execute it from a shell inside that container:

```bash
podman cp 3-colocated/redis-cli-scripts my-redis:/tmp/redis-cli-scripts
podman exec -it my-redis sh
```

Inside the container:

```bash
cd /tmp/redis-cli-scripts
chmod +x show-job-status-in-container.sh
./show-job-status-in-container.sh
```

Replace `my-redis` with the container name or ID from `podman ps`.
The script invokes the container's local `redis-cli` directly, connecting to
`127.0.0.1:6379` by default. You can override the connection:

```bash
REDIS_HOST=127.0.0.1 REDIS_PORT=6379 REDIS_DB=0 ./show-job-status-in-container.sh
```

If authentication is enabled, set `REDISCLI_AUTH` in the container environment
and optionally `REDIS_USERNAME` for ACL authentication:

```bash
REDISCLI_AUTH='your-password' REDIS_USERNAME=default ./show-job-status-in-container.sh
```

Avoid putting passwords directly in shell history.

## Output

The viewer prints server type aggregate counters, each server ID and its
counters/state/heartbeat TTL, then the jobs found in that server's job index.
It reads:

- `servers:{registry}:types`
- `servers:{registry}:type:<serverType>`
- `summary:type:{<serverType>}`
- `summary:server:{<serverType>}:<serverId>`
- `servers:state:{<serverType>}`
- `heartbeat:server:{<serverType>}:<serverId>`
- `jobs:server:{<serverType>}:<serverId>`
- `job:{<serverType>}:<jobReference>`

If the registry set is empty, the in-container script checks for
`summary:type:*` keys and prints diagnostic guidance. This helps distinguish
an empty/wrong Redis database from a missing registry index.

## Notes

- Both scripts are read-only; they do not modify or delete Redis keys.
- Reads are separate Redis commands, not an atomic point-in-time snapshot.
- Jobs are listed through each server's per-server job index. This is a
  static listing, not the interactive browser; it does not implement the
  browser's expand/collapse or `j` type-wide-jobs toggle.
