#!/usr/bin/env bash
#
# Run this script from inside the Redis container. It calls the redis-cli
# binary directly and reads the 3-colocated RedisJobStatus layout.
#
# Optional:
#   REDIS_HOST=127.0.0.1 REDIS_PORT=6379 REDIS_DB=0 ./show-job-status-in-container.sh
#   REDIS_USERNAME=default REDISCLI_AUTH=... ./show-job-status-in-container.sh
#
set -Eeuo pipefail

REDIS_HOST=${REDIS_HOST:-127.0.0.1}
REDIS_PORT=${REDIS_PORT:-6379}
REDIS_DB=${REDIS_DB:-0}
REDIS_USERNAME=${REDIS_USERNAME:-}

command -v redis-cli >/dev/null 2>&1 || {
  echo "ERROR: redis-cli is not installed or not on PATH in this container." >&2
  exit 1
}

REDIS=(redis-cli --raw --no-auth-warning -h "$REDIS_HOST" -p "$REDIS_PORT" -n "$REDIS_DB")
if [[ -n "$REDIS_USERNAME" ]]; then
  REDIS+=(--user "$REDIS_USERNAME")
fi

r() { "${REDIS[@]}" "$@"; }

if ! ping=$(r PING 2>&1) || [[ "$ping" != "PONG" ]]; then
  echo "ERROR: Redis PING failed at $REDIS_HOST:$REDIS_PORT database $REDIS_DB: $ping" >&2
  exit 1
fi

# HMGET returns one line per requested field when --raw is enabled.
read_counters() {
  local key=$1 output
  output=$(r HMGET "$key" pending active suspended total)
  local -a values=()
  mapfile -t values <<< "$output"
  printf '%s\t%s\t%s\t%s' "${values[0]:-0}" "${values[1]:-0}" "${values[2]:-0}" "${values[3]:-0}"
}

printf 'Redis Job Status (3-colocated)\n'
printf 'Redis: %s:%s  DB: %s\n\n' "$REDIS_HOST" "$REDIS_PORT" "$REDIS_DB"

types=$(r SMEMBERS 'servers:{registry}:types')
if [[ -z "$types" ]]; then
  printf 'No members found in servers:{registry}:types.\n'
  printf 'Checking for type summary hashes to help diagnose the key layout...\n'
  summary_keys=$(r --scan --pattern 'summary:type:*' 2>/dev/null || true)
  if [[ -n "$summary_keys" ]]; then
    printf 'Found type summary keys, but the registry set is empty or missing:\n'
    while IFS= read -r key; do
      [[ -n "$key" ]] && printf '  %s\n' "$key"
    done <<< "$summary_keys"
    printf '\nThe registry set must contain the matching type names for this viewer to enumerate servers.\n'
  else
    printf 'No summary:type:* keys found in database %s. Check REDIS_DB and whether this is the application Redis instance.\n' "$REDIS_DB"
  fi
  exit 0
fi

printf '%-42s %8s %8s %10s %8s  %s\n' 'SERVER TYPE / SERVER ID / JOB' 'PENDING' 'ACTIVE' 'SUSPENDED' 'TOTAL' 'STATE / HEARTBEAT'
printf '%s\n' '--------------------------------------------------------------------------------------------------------------------------------'

while IFS= read -r server_type; do
  [[ -z "$server_type" ]] && continue
  type_key="summary:type:{${server_type}}"
  IFS=$'\t' read -r pending active suspended total < <(read_counters "$type_key")
  printf '%-42s %8s %8s %10s %8s  %s\n' "$server_type" "$pending" "$active" "$suspended" "$total" ""

  server_ids=$(r SMEMBERS "servers:{registry}:type:${server_type}")
  if [[ -z "$server_ids" ]]; then
    printf '  (no server IDs in servers:{registry}:type:%s)\n' "$server_type"
    continue
  fi

  while IFS= read -r server_id; do
    [[ -z "$server_id" ]] && continue
    server_key="summary:server:{${server_type}}:${server_id}"
    IFS=$'\t' read -r sp sa ss st < <(read_counters "$server_key")
    state=$(r HGET "servers:state:{${server_type}}" "$server_id" || true)
    ttl=$(r TTL "heartbeat:server:{${server_type}}:${server_id}" || true)
    case "$ttl" in
      -2) heartbeat='EXPIRED' ;;
      -1) heartbeat='NO EXPIRY' ;;
      '') heartbeat='UNKNOWN' ;;
      *) heartbeat="${ttl}s" ;;
    esac
    printf '  %-40s %8s %8s %10s %8s  %s / %s\n' "$server_id" "$sp" "$sa" "$ss" "$st" "${state:-UNKNOWN}" "$heartbeat"

    references=$(r SMEMBERS "jobs:server:{${server_type}}:${server_id}")
    if [[ -n "$references" ]]; then
      while IFS= read -r reference; do
        [[ -z "$reference" ]] && continue
        job_key="job:{${server_type}}:${reference}"
        job_data=$(r HMGET "$job_key" jobId status)
        mapfile -t job_fields <<< "$job_data"
        job_id=${job_fields[0]:-$reference}
        job_status=${job_fields[1]:-UNKNOWN}
        printf '    JOB: %s [%s]\n' "$job_id" "${job_status^^}"
      done <<< "$references"
    fi
  done <<< "$server_ids"
done <<< "$types"

printf '\nRead-only view; no Redis keys are modified.\n'
