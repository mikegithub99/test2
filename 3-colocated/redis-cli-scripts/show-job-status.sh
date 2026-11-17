#!/usr/bin/env bash
#
# Read-only hierarchy viewer for the 3-colocated RedisJobStatus layout.
#
# Local redis-cli:
#   REDIS_HOST=127.0.0.1 REDIS_PORT=6379 ./show-job-status.sh
#
# redis-cli inside a Podman container:
#   REDIS_CONTAINER=my-redis ./show-job-status.sh
#
# Optional authentication:
#   REDIS_PASSWORD=... REDIS_USERNAME=default ./show-job-status.sh
#
set -Eeuo pipefail

REDIS_HOST=${REDIS_HOST:-127.0.0.1}
REDIS_PORT=${REDIS_PORT:-6379}
REDIS_DB=${REDIS_DB:-0}
REDIS_CONTAINER=${REDIS_CONTAINER:-}
REDIS_USERNAME=${REDIS_USERNAME:-}
REDIS_PASSWORD=${REDIS_PASSWORD:-}

if [[ -n "$REDIS_CONTAINER" ]]; then
  command -v podman >/dev/null 2>&1 || {
    echo "ERROR: podman was not found on PATH." >&2
    exit 1
  }
  REDIS=(podman exec -i)
  if [[ -n "$REDIS_PASSWORD" ]]; then
    REDIS+=(-e "REDISCLI_AUTH=$REDIS_PASSWORD")
  fi
  REDIS+=("$REDIS_CONTAINER" redis-cli --raw --no-auth-warning)
else
  command -v redis-cli >/dev/null 2>&1 || {
    echo "ERROR: redis-cli was not found. Set REDIS_CONTAINER to use Podman." >&2
    exit 1
  }
  REDIS=(redis-cli --raw --no-auth-warning -h "$REDIS_HOST" -p "$REDIS_PORT")
  if [[ -n "$REDIS_PASSWORD" ]]; then
    export REDISCLI_AUTH="$REDIS_PASSWORD"
  fi
fi

if [[ -n "$REDIS_USERNAME" ]]; then
  REDIS+=(--user "$REDIS_USERNAME")
fi
REDIS+=( -n "$REDIS_DB" )

r() {
  "${REDIS[@]}" "$@"
}

if ! r PING >/dev/null 2>&1; then
  echo "ERROR: could not connect to Redis. Check container name, host/port, and authentication." >&2
  exit 1
fi

counter_values() {
  local key=$1
  local values
  values=$(r HMGET "$key" pending active suspended total)
  local -a fields=()
  mapfile -t fields <<< "$values"
  printf '%s\t%s\t%s\t%s' "${fields[0]:-0}" "${fields[1]:-0}" "${fields[2]:-0}" "${fields[3]:-0}"
}

printf 'Redis Job Status (3-colocated)\n'
if [[ -n "$REDIS_CONTAINER" ]]; then
  printf 'Connection: Podman container %s\n' "$REDIS_CONTAINER"
else
  printf 'Connection: %s:%s (database %s)\n' "$REDIS_HOST" "$REDIS_PORT" "$REDIS_DB"
fi
printf '\n'

types=$(r SMEMBERS 'servers:{registry}:types')
if [[ -z "$types" ]]; then
  echo "No server types found in servers:{registry}:types."
  exit 0
fi

printf '%-54s %8s %8s %10s %8s  %s\n' 'SERVER TYPE / SERVER ID / JOB' 'PENDING' 'ACTIVE' 'SUSPENDED' 'TOTAL' 'STATE / HEARTBEAT'
printf '%s\n' '--------------------------------------------------------------------------------------------------------------------------------'

while IFS= read -r server_type; do
  [[ -z "$server_type" ]] && continue
  type_key="summary:type:{${server_type}}"
  IFS=$'\t' read -r pending active suspended total < <(counter_values "$type_key")
  server_set_key="servers:{registry}:type:${server_type}"
  server_ids=$(r SMEMBERS "$server_set_key")

  printf '%-54s %8s %8s %10s %8s  %s\n' "$server_type" "$pending" "$active" "$suspended" "$total" ""

  if [[ -z "$server_ids" ]]; then
    printf '  (no registered server IDs)\n'
    continue
  fi

  while IFS= read -r server_id; do
    [[ -z "$server_id" ]] && continue
    server_key="summary:server:{${server_type}}:${server_id}"
    IFS=$'\t' read -r sp sa ss st < <(counter_values "$server_key")
    state=$(r HGET "servers:state:{${server_type}}" "$server_id" || true)
    heartbeat_key="heartbeat:server:{${server_type}}:${server_id}"
    ttl=$(r TTL "$heartbeat_key" || true)
    case "$ttl" in
      -2) heartbeat='EXPIRED' ;;
      -1) heartbeat='NO EXPIRY' ;;
      *) heartbeat="${ttl}s" ;;
    esac
    printf '  %-52s %8s %8s %10s %8s  %s / %s\n' "$server_id" "$sp" "$sa" "$ss" "$st" "${state:-UNKNOWN}" "$heartbeat"

    jobs_key="jobs:server:{${server_type}}:${server_id}"
    references=$(r SMEMBERS "$jobs_key")
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
