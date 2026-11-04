# Redis Job Status Cleanup

Standalone utility for finding and removing only the Redis keys used by the
`.7` Redis Job Status design. It does not call `RedisClient` or
`RedisJobStatus`.

## Build

Requires C++17, redis++, and hiredis.

```sh
make
```

## Configure

Set `REDIS_URL` to the target Redis connection URL. If unset, the utility
connects to `redis://127.0.0.1:6379`.

## Preview

Run without arguments to scan and list matching keys without deleting them:

```sh
./RedisJobStatusCleanup
```

## Delete

Run with `--delete`, review the listed keys, and type `DELETE` to confirm:

```sh
./RedisJobStatusCleanup --delete
```

The utility only matches the key families defined by the `.7` design:

- `servers:{registry}:types`
- `servers:{registry}:type:*`
- `summary:type:{*}`
- `summary:server:{*}:*`
- `job:{*}:*`
- `jobs:type:{*}`
- `jobs:server:{*}:*`
- `heartbeat:server:{*}:*`

It uses `SCAN`, not `KEYS`, and deletes matching keys individually. It
does not use `FLUSHDB` or `FLUSHALL`. Run it against the intended Redis
database. The scan and deletion are not an atomic snapshot; stop writers while
cleaning if you need to ensure no new job-status keys are created mid-run.
