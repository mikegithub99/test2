# Redis Job Status Browser (.7)

A small, read-only terminal UI for inspecting the Redis keyspace used by the
.7 RedisJobStatus implementation. It is separate from the C++ library and
does not modify Redis data.

## Requirements

- Python 3.10+
- Redis reachable from the machine running the browser
- Python packages in requirements.txt

## Install and run

\`\`\`sh
cd .7/tools/redis_browser
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
REDIS_URL=redis://127.0.0.1:6379 python redis_browser.py
\`\`\`

For Redis Cluster, set REDIS_CLUSTER=1:

\`\`\`sh
REDIS_URL=redis://127.0.0.1:6379 REDIS_CLUSTER=1 python redis_browser.py
\`\`\`

Environment variables:

- REDIS_URL: connection URL; defaults to redis://127.0.0.1:6379.
- REDIS_CLUSTER: set to 1, true, or yes for Redis Cluster.

## Navigation

- Overview shows registered server types, their server rows, summary
  counters, and heartbeat TTLs.
- Jobs lists jobs across all registered server types. It displays the
  original jobId unless that ID is duplicated in the current result set;
  duplicate IDs display the stored composite reference instead.
- Raw keys lists the .7 key patterns and shows Redis type and value size.
- Select any row to inspect its complete value in the details panel.
- 1, 2, 3 switch views; r refreshes; q quits.
- Auto-refresh defaults to 2 seconds. Choose 1, 2, 5, 10, 15, or 30 seconds.
  Refresh is skipped if the preceding refresh is still running.
- Refresh preserves the current view and selected key when it remains present.
- Expand All and Collapse All control the server rows in Overview.

The raw inspector supports hashes, sets, strings, lists, sorted sets, and streams.
Only the known .7 key patterns are scanned; unrelated Redis keys are not shown.

## Safety and limitations

This tool is intentionally read-only. It does not create, update, or delete keys.
Large keyspaces or very short refresh intervals can increase Redis load. The raw
key view scans the .7 patterns on refresh; use a longer interval on busy systems.
The browser uses Redis SCAN rather than KEYS.

Redis Cluster support depends on redis-py's RedisCluster client and the
cluster's permissions/network configuration. This initial version has not been
tested against a live Redis instance.
