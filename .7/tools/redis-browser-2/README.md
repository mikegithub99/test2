# Redis Job Counters Browser (.7)

A counter-first, read-only Textual terminal UI for the RedisJobStatus .7 keyspace.

## Display

The screen starts collapsed, showing one row per **server type** with its aggregate counters. Expand a server type to see its **server** rows; expand a server to see its **job** rows. Each level shows its own counters. This keeps the initial screen compact and lets you drill into only the server or job detail you need.

The counter columns are always in this order:

1. Jobs Pending
2. Jobs Active
3. Jobs Suspended
4. Total Jobs

Each job row contributes 1 to its current status column and 1 to Total Jobs, so the columns remain meaningful at every level. Up/Down arrows move through the entire hierarchy. There are no separate Overview, Jobs, or Raw Keys tabs.

## Keyboard controls

| Key | Action |
|---|---|
| 1 | Refresh immediately |
| 2 | Pause / resume automatic refresh |
| 3 | Slow automatic refresh |
| 4 | Speed up automatic refresh |
| Up / Down | Move through the visible hierarchy |
| Space | Expand or collapse the selected server type or server |
| q | Quit |

Automatic refresh starts at 2 seconds. The interval can be adjusted through 1, 2, 5, 10, 15, and 30 seconds. Manual refresh works while paused. Refresh retains the selected row when that Redis key still exists. Expanded/collapsed state is retained across refreshes.

## Install and run

Requires Python 3.10+, Redis access, and the packages in requirements.txt.

```sh
cd .7/tools/redis-browser-2
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
REDIS_URL=redis://127.0.0.1:6379 python redis_browser.py
```

For Redis Cluster:

```sh
REDIS_URL=redis://127.0.0.1:6379 REDIS_CLUSTER=1 python redis_browser.py
```

Environment variables:

- REDIS_URL: Redis connection URL; defaults to redis://127.0.0.1:6379.
- REDIS_CLUSTER: set to 1, true, or yes to use redis-py's RedisCluster client.

## Notes

- This browser only reads Redis; it never modifies keys.
- Counters are read from the materialized `summary:type:{serverType}` and
  `summary:server:{serverType}:{serverId}` hashes.
- Jobs are read through each server's `jobs:server:{serverType}:{serverId}`
  set and corresponding `job:{serverType}:{reference}` hashes.
- The browser uses Redis SCAN nowhere; it follows the explicit .7 indexes.
- The browser has not yet been verified against a live Redis instance.
