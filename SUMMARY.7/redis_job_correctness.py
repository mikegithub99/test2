#!/usr/bin/env python3
"""Read-only consistency audit for RedisJobStatus keys.

Uses REDIS_URL and REDIS_CLUSTER (same convention as redis-browser-2).
Run while workers are idle for the strongest result: SCAN/read sequences are
not a point-in-time snapshot. Exit: 0 clean, 1 inconsistencies, 2 runtime error.
"""
import argparse
import json
import os
import sys
from collections import Counter, defaultdict

import redis
from redis.cluster import RedisCluster

STATES = ("PENDING", "ACTIVE", "SUSPENDED")
FIELDS = ("pending", "active", "suspended", "total")


def text(v):
    return v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)


class Audit:
    def __init__(self, client, selected_type=None):
        self.r = client
        self.selected_type = selected_type
        self.issues = []
        self.stats = Counter()

    def add(self, code, message, severity="ERROR", st=None, sid=None, ref=None, key=None):
        self.issues.append(dict(severity=severity, code=code, message=message,
                                server_type=st, server_id=sid, reference=ref, key=key))

    def scan(self, pattern):
        for key in self.r.scan_iter(match=pattern, count=500):
            yield text(key)

    def kind(self, key):
        return text(self.r.type(key)).lower()

    @staticmethod
    def tagged(key, prefix):
        if not key.startswith(prefix):
            return None
        tail = key[len(prefix):]
        if not tail.startswith("{") or "}" not in tail:
            return None
        st, rest = tail[1:].split("}", 1)
        if rest and not rest.startswith(":"):
            return None
        return st, rest[1:] if rest else ""

    @staticmethod
    def decode_ref(ref):
        # Reference format: <serverId UTF-8 byte length>:<serverId>:<jobId>
        p = ref.find(":")
        if p <= 0 or not ref[:p].isascii() or not ref[:p].isdigit():
            return None
        n = int(ref[:p])
        raw = ref[p + 1:].encode("utf-8")
        if len(raw) <= n or raw[n:n + 1] != b":":
            return None
        try:
            return raw[:n].decode("utf-8"), raw[n + 1:].decode("utf-8")
        except UnicodeDecodeError:
            return None

    def get_set(self, key, code, st=None):
        kind = self.kind(key)
        if kind == "none":
            return set()
        if kind != "set":
            self.add(code, f"Expected SET, found {kind}", st=st, key=key)
            return set()
        return {text(x) for x in self.r.smembers(key)}

    def get_hash(self, key, code, st=None):
        kind = self.kind(key)
        if kind == "none":
            return {}
        if kind != "hash":
            self.add(code, f"Expected HASH, found {kind}", st=st, key=key)
            return {}
        return {text(k): text(v) for k, v in self.r.hgetall(key).items()}

    def read_counters(self, key, st, sid=None):
        raw = self.get_hash(key, "SUMMARY_KEY_TYPE", st)
        actual = {}
        for field in FIELDS:
            value = raw.get(field, "0")
            try:
                n = int(value, 10)
                if str(n) != value or n < 0:
                    raise ValueError
                actual[field] = n
            except ValueError:
                self.add("INVALID_COUNTER", f"{field} is not a nonnegative canonical integer: {value!r}",
                         st=st, sid=sid, key=key)
                actual[field] = 0
        if actual["total"] != sum(actual[x] for x in FIELDS[:3]):
            self.add("SUMMARY_ARITHMETIC", "total != pending + active + suspended",
                     st=st, sid=sid, key=key)
        extras = sorted(set(raw) - set(FIELDS))
        if extras:
            self.add("UNKNOWN_SUMMARY_FIELDS", f"Unexpected summary fields: {extras}",
                     "WARNING", st, sid, key=key)
        return actual

    def compare_summary(self, key, derived, st, sid=None):
        actual = self.read_counters(key, st, sid)
        expected = dict(pending=derived["PENDING"], active=derived["ACTIVE"],
                        suspended=derived["SUSPENDED"],
                        total=sum(derived[x] for x in STATES))
        for field in FIELDS:
            if actual[field] != expected[field]:
                self.add("COUNTER_MISMATCH",
                         f"{field}: summary={actual[field]}, job hashes={expected[field]}",
                         st=st, sid=sid, key=key)

    def discover_types(self):
        found = self.get_set("servers:{registry}:types", "TYPE_REGISTRY_KEY_TYPE")
        for pattern, prefix in (
            ("job:{*}:*", "job:"), ("jobs:type:{*}", "jobs:type:"),
            ("jobs:server:{*}:*", "jobs:server:"),
            ("summary:type:{*}", "summary:type:"),
            ("summary:server:{*}:*", "summary:server:"),
        ):
            for key in self.scan(pattern):
                item = self.tagged(key, prefix)
                if item:
                    found.add(item[0])
        return {self.selected_type} if self.selected_type else found

    def audit_type(self, st):
        self.stats["server_types"] += 1
        type_index_key = f"jobs:type:{{{st}}}"
        type_summary_key = f"summary:type:{{{st}}}"
        registry_key = f"servers:{{registry}}:type:{st}"
        type_index = self.get_set(type_index_key, "TYPE_INDEX_KEY_TYPE", st)
        registered = self.get_set(registry_key, "SERVER_REGISTRY_KEY_TYPE", st)

        server_ids = set(registered)
        server_indexes = {}
        for key in self.scan("jobs:server:{*}:*"):
            parsed = self.tagged(key, "jobs:server:")
            if parsed and parsed[0] == st:
                server_ids.add(parsed[1])
                server_indexes[parsed[1]] = self.get_set(key, "SERVER_INDEX_KEY_TYPE", st)
        for pattern, prefix in (("summary:server:{*}:*", "summary:server:"),
                                ("heartbeat:server:{*}:*", "heartbeat:server:")):
            for key in self.scan(pattern):
                parsed = self.tagged(key, prefix)
                if parsed and parsed[0] == st:
                    server_ids.add(parsed[1])

        job_hashes = {}
        jobs_per_server = defaultdict(set)
        server_counts = defaultdict(Counter)
        type_counts = Counter()
        for key in self.scan("job:{*}:*"):
            parsed = self.tagged(key, "job:")
            if not parsed or parsed[0] != st:
                continue
            ref = parsed[1]
            self.stats["job_hashes"] += 1
            record = self.get_hash(key, "JOB_KEY_TYPE", st)
            if not record:
                self.add("EMPTY_JOB_HASH", "Job key has no readable hash fields", st=st, ref=ref, key=key)
                continue
            job_hashes[ref] = record
            decoded = self.decode_ref(ref)
            if decoded is None:
                self.add("INVALID_REFERENCE", "Cannot decode composite serverId/jobId reference",
                         st=st, ref=ref, key=key)
                continue
            ref_sid, ref_jid = decoded
            sid, jid, status = record.get("serverId"), record.get("jobId"), record.get("status")
            if record.get("serverType") != st:
                self.add("JOB_TYPE_MISMATCH", f"Hash serverType={record.get('serverType')!r}",
                         st=st, sid=sid, ref=ref, key=key)
            if sid != ref_sid:
                self.add("REFERENCE_SERVER_MISMATCH", f"Reference={ref_sid!r}; hash={sid!r}",
                         st=st, sid=sid, ref=ref, key=key)
            if jid != ref_jid:
                self.add("REFERENCE_JOB_MISMATCH", f"Reference={ref_jid!r}; hash={jid!r}",
                         st=st, sid=sid, ref=ref, key=key)
            if status not in STATES:
                self.add("INVALID_JOB_STATUS", f"Unexpected status {status!r}",
                         st=st, sid=sid, ref=ref, key=key)
                continue
            if sid:
                server_ids.add(sid)
                jobs_per_server[sid].add(ref)
                server_counts[sid][status] += 1
            type_counts[status] += 1

        for ref in sorted(set(job_hashes) - type_index):
            self.add("MISSING_TYPE_INDEX_MEMBER", "Job hash absent from type index",
                     st=st, ref=ref, key=type_index_key)
        for ref in sorted(type_index - set(job_hashes)):
            self.add("ORPHAN_TYPE_INDEX_MEMBER", "Type index entry has no job hash",
                     st=st, ref=ref, key=type_index_key)

        for sid in sorted(server_ids):
            index_key = f"jobs:server:{{{st}}}:{sid}"
            summary_key = f"summary:server:{{{st}}}:{sid}"
            indexed = server_indexes.get(sid)
            if indexed is None:
                indexed = self.get_set(index_key, "SERVER_INDEX_KEY_TYPE", st)
            expected = jobs_per_server.get(sid, set())
            for ref in sorted(expected - indexed):
                self.add("MISSING_SERVER_INDEX_MEMBER", "Owned job missing from server index",
                         st=st, sid=sid, ref=ref, key=index_key)
            for ref in sorted(indexed - expected):
                self.add("ORPHAN_SERVER_INDEX_MEMBER", "Server index entry has no matching owned job",
                         st=st, sid=sid, ref=ref, key=index_key)
            if sid not in registered:
                self.add("SERVER_NOT_REGISTERED", "Server data exists but server ID is not registered",
                         "WARNING", st, sid, key=registry_key)
            self.compare_summary(summary_key, server_counts[sid], st, sid)
            self.stats["servers"] += 1

        self.compare_summary(type_summary_key, type_counts, st)
        self.stats["valid_status_jobs"] += sum(type_counts.values())
        self.stats["type_index_members"] += len(type_index)
        self.stats["registered_servers"] += len(registered)

    def run(self):
        types = sorted(self.discover_types())
        if not types:
            self.add("NO_SERVER_TYPES", "No types found in registry or job/summary keys", "WARNING")
        for st in types:
            self.audit_type(st)
        errors = sum(x["severity"] == "ERROR" for x in self.issues)
        return dict(result="PASS" if not errors else "FAIL", read_only=True,
                    snapshot_consistent=False,
                    note="SCAN plus subsequent reads is not a point-in-time snapshot; run with workers idle.",
                    server_types_audited=types, counts=dict(self.stats),
                    error_count=errors,
                    warning_count=sum(x["severity"] == "WARNING" for x in self.issues),
                    issues=self.issues)


def make_client():
    url = os.getenv("REDIS_URL", "redis://127.0.0.1:6379")
    if os.getenv("REDIS_CLUSTER", "").lower() in {"1", "true", "yes"}:
        return RedisCluster.from_url(url, decode_responses=True)
    return redis.Redis.from_url(url, decode_responses=True)


def main():
    parser = argparse.ArgumentParser(description="Read-only RedisJobStatus correctness analyzer")
    parser.add_argument("--server-type", help="Limit audit to one server type")
    parser.add_argument("--json", metavar="FILE", help="Write full report to JSON")
    args = parser.parse_args()
    client = make_client()
    try:
        client.ping()
        report = Audit(client, args.server_type).run()
    except Exception as exc:
        print(f"Audit failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    finally:
        try:
            client.close()
        except Exception:
            pass
    print(f"{report['result']}: {len(report['server_types_audited'])} type(s), "
          f"{report['counts'].get('servers', 0)} server(s), "
          f"{report['counts'].get('job_hashes', 0)} job hash(es), "
          f"{report['error_count']} error(s), {report['warning_count']} warning(s).")
    print(report["note"])
    for item in report["issues"]:
        where = [f"{k}={item[k]}" for k in ("server_type", "server_id", "reference", "key")
                 if item[k] is not None]
        print(f"{item['severity']} {item['code']}: {item['message']}"
              + (f" [{'; '.join(where)}]" if where else ""))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as out:
            json.dump(report, out, indent=2, ensure_ascii=False)
            out.write("\n")
        print(f"JSON report written to {args.json}")
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
