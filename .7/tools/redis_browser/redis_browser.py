"""Read-only Textual browser for the RedisJobStatus .7 keyspace."""

from __future__ import annotations

import asyncio
import json
import os
from collections import Counter
from datetime import datetime
from typing import Any

import redis
from redis.cluster import RedisCluster
from textual import on
from textual.app import App, ComposeResult
from textual.containers import Horizontal
from textual.widgets import Button, DataTable, Footer, Header, Select, Static

REFRESH_OPTIONS = [
    ("1 second", 1), ("2 seconds", 2), ("5 seconds", 5),
    ("10 seconds", 10), ("15 seconds", 15), ("30 seconds", 30),
]
KEY_PATTERNS = [
    "servers:{registry}:types",
    "servers:{registry}:type:*",
    "summary:type:{*}",
    "summary:server:{*}:*",
    "job:{*}:*",
    "jobs:type:{*}",
    "jobs:server:{*}:*",
    "heartbeat:server:{*}:*",
]


def make_client():
    url = os.getenv("REDIS_URL", "redis://127.0.0.1:6379")
    if os.getenv("REDIS_CLUSTER", "").lower() in {"1", "true", "yes"}:
        return RedisCluster.from_url(url, decode_responses=True)
    return redis.Redis.from_url(url, decode_responses=True)


def redis_text(value: Any) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


class RedisBrowser(App):
    TITLE = "Redis Job Status Browser (.7)"
    SUB_TITLE = "Read-only Redis inspector"
    BINDINGS = [
        ("r", "refresh_now", "Refresh"),
        ("1", "show_overview", "Overview"),
        ("2", "show_jobs", "Jobs"),
        ("3", "show_keys", "Raw keys"),
        ("escape", "overview_back", "Back"),
        ("q", "quit", "Quit"),
    ]

    CSS = """
    Screen { layout: vertical; }
    #toolbar { height: 3; padding: 0 1; }
    #mode-label { width: 7; content-align: left middle; }
    #refresh-label { width: 9; content-align: center middle; }
    #interval { width: 18; }
    #table { height: 1fr; }
    #details { height: 12; border: round $accent; padding: 1; overflow-y: auto; }
    #status { height: 1; padding: 0 1; color: $text-muted; }
    Button { margin-right: 1; }
    """

    def __init__(self):
        super().__init__()
        self.client = make_client()
        self.mode = "overview"
        self.interval = 2
        self.selected_key: str | None = None
        self.last_rows: list[tuple[str, str, str, str]] = []
        self._refreshing = False
        self._timer = None
        self.overview_level = "types"
        self.selected_server_type = None
        self.selected_server_id = None

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal(id="toolbar"):
            yield Static("View:", id="mode-label")
            yield Button("Overview", id="overview", variant="primary")
            yield Button("Jobs", id="jobs")
            yield Button("Raw keys", id="keys")
            yield Button("Back", id="back", disabled=True)
            yield Static("Every:", id="refresh-label")
            yield Select(REFRESH_OPTIONS, value=2, allow_blank=False, id="interval")
            yield Button("Refresh", id="refresh")
        yield DataTable(id="table", cursor_type="row", zebra_stripes=True)
        yield Static("Select a row to inspect its Redis data.", id="details")
        yield Static("Starting…", id="status")
        yield Footer()

    async def on_mount(self) -> None:
        self.set_table_columns()
        self._timer = self.set_interval(self.interval, self.refresh_view)
        await self.refresh_view()

    async def on_unmount(self) -> None:
        try:
            await asyncio.to_thread(self.client.close)
        except Exception:
            pass

    def set_table_columns(self) -> None:
        table = self.query_one("#table", DataTable)
        table.clear(columns=True)
        if self.mode == "overview":
            if self.overview_level == "types":
                table.add_columns("Server Type", "Job Counters")
            elif self.overview_level == "servers":
                table.add_columns("Server ID", "Heartbeat", "Job Counters")
            else:
                table.add_columns("Job ID", "Status", "Server")
        elif self.mode == "jobs":
            table.add_columns("Job ID", "Server Type / Server ID", "Status")
        else:
            table.add_columns("Redis Key", "Type", "Details")
        self.query_one("#back", Button).disabled = not (
            self.mode == "overview" and self.overview_level != "types"
        )

    def set_mode(self, mode: str) -> None:
        self.mode = mode
        if mode == "overview":
            self.overview_level = "types"
            self.selected_server_type = None
            self.selected_server_id = None
        self.set_table_columns()
        for name in ("overview", "jobs", "keys"):
            self.query_one(f"#{name}", Button).variant = (
                "primary" if name == mode else "default"
            )
        self.selected_key = None
        self.query_one("#details", Static).update(
            "Select a row to inspect its Redis data."
        )

    async def overview_back(self) -> None:
        if self.mode != "overview" or self.overview_level == "types":
            return
        if self.overview_level == "jobs":
            self.overview_level = "servers"
            self.selected_server_id = None
        else:
            self.overview_level = "types"
            self.selected_server_type = None
        self.selected_key = None
        self.set_table_columns()
        await self.refresh_view()
        self.query_one("#table", DataTable).focus()

    @on(Button.Pressed)
    async def button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id in {"overview", "jobs", "keys"}:
            self.set_mode(event.button.id)
            await self.refresh_view()
            self.query_one("#table", DataTable).focus()
        elif event.button.id == "back":
            await self.overview_back()
        elif event.button.id == "refresh":
            await self.refresh_view()

    @on(Select.Changed, "#interval")
    def interval_changed(self, event: Select.Changed) -> None:
        if event.value is not Select.BLANK:
            self.interval = int(event.value)
            if self._timer:
                self._timer.stop()
            self._timer = self.set_interval(self.interval, self.refresh_view)
            self.update_status(f"Auto-refresh every {self.interval}s")

    @on(DataTable.RowHighlighted)
    async def row_highlighted(self, event: DataTable.RowHighlighted) -> None:
        key = str(event.row_key.value)
        self.selected_key = key or None
        if self.selected_key:
            await self.show_details(self.selected_key)

    @on(DataTable.RowSelected)
    async def row_selected(self, event: DataTable.RowSelected) -> None:
        key = str(event.row_key.value)
        self.selected_key = key or None
        if not self.selected_key:
            return
        if self.mode == "overview" and self.overview_level == "types":
            row = next((item for item in self.last_rows if item[0] == key), None)
            if row:
                self.selected_server_type = row[1]
                self.overview_level = "servers"
                self.selected_server_id = None
                self.set_table_columns()
                await self.refresh_view()
                self.query_one("#table", DataTable).focus()
            return
        if self.mode == "overview" and self.overview_level == "servers":
            row = next((item for item in self.last_rows if item[0] == key), None)
            if row:
                self.selected_server_id = row[1]
                self.overview_level = "jobs"
                self.set_table_columns()
                await self.refresh_view()
                self.query_one("#table", DataTable).focus()
            return
        await self.show_details(self.selected_key)
    async def refresh_view(self) -> None:
        if self._refreshing:
            return
        self._refreshing = True
        try:
            if self.mode == "overview":
                if self.overview_level == "types":
                    rows = await asyncio.to_thread(self.load_server_types)
                elif self.overview_level == "servers":
                    rows = await asyncio.to_thread(self.load_servers, self.selected_server_type)
                else:
                    rows = await asyncio.to_thread(self.load_jobs, self.selected_server_type, self.selected_server_id)
            elif self.mode == "jobs":
                rows = await asyncio.to_thread(self.load_jobs)
            else:
                rows = await asyncio.to_thread(self.load_keys)

            table = self.query_one("#table", DataTable)
            old_cursor_row = table.cursor_row
            old_cursor_key = None
            if 0 <= old_cursor_row < len(self.last_rows):
                old_cursor_key = self.last_rows[old_cursor_row][0]
            old_key = old_cursor_key or self.selected_key
            had_focus = table.has_focus

            self.last_rows = rows
            table.clear()
            for key, name, col2, col3 in rows:
                if self.mode == "overview" and self.overview_level == "types":
                    table.add_row(name, col2, key=key)
                elif self.mode == "jobs":
                    table.add_row(name, col3, col2, key=key)
                else:
                    table.add_row(name, col2, col3, key=key)

            restored_index = None
            if old_key:
                restored_index = next(
                    (index for index, row in enumerate(rows) if row[0] == old_key),
                    None,
                )
            if restored_index is None and rows:
                restored_index = min(old_cursor_row, len(rows) - 1)

            if restored_index is not None:
                table.move_cursor(row=restored_index, column=0, animate=False)
                self.selected_key = rows[restored_index][0]
                await self.show_details(self.selected_key)
            else:
                self.selected_key = None
                self.query_one("#details", Static).update(
                    "Select a row to inspect its Redis data."
                )

            if had_focus:
                table.focus()
            self.update_status(
                f"{len(rows)} rows | Updated "
                f"{datetime.now().astimezone().strftime('%H:%M:%S %Z')} "
                f"| Auto-refresh {self.interval}s"
            )
        except Exception as exc:
            self.update_status(f"Redis error: {exc}")
        finally:
            self._refreshing = False

    def load_server_types(self) -> list[tuple[str, str, str, str]]:
        rows = []
        for server_type in sorted(self.client.smembers("servers:{registry}:types")):
            summary_key = f"summary:type:{{{server_type}}}"
            summary = self.client.hgetall(summary_key)
            counters = " ".join(f"{k}={v}" for k, v in sorted(summary.items()))
            rows.append((summary_key, server_type, counters or "No counters", ""))
        return rows

    def load_servers(self, server_type: str | None) -> list[tuple[str, str, str, str]]:
        if not server_type:
            return []
        rows = []
        server_key = f"servers:{{registry}}:type:{server_type}"
        for server_id in sorted(self.client.smembers(server_key)):
            heartbeat_key = f"heartbeat:server:{{{server_type}}}:{server_id}"
            ttl = self.client.ttl(heartbeat_key)
            heartbeat = f"TTL={ttl}s" if ttl >= 0 else "Missing"
            summary_key = f"summary:server:{{{server_type}}}:{server_id}"
            summary = self.client.hgetall(summary_key)
            counters = " ".join(f"{k}={v}" for k, v in sorted(summary.items()))
            rows.append((summary_key, server_id, heartbeat, counters or "No counters"))
        return rows
    def load_jobs(self, filter_server_type: str | None = None, filter_server_id: str | None = None) -> list[tuple[str, str, str, str]]:
        jobs = []
        types = [filter_server_type] if filter_server_type else sorted(self.client.smembers("servers:{registry}:types"))
        for server_type in types:
            for reference in self.client.smembers(f"jobs:type:{{{server_type}}}"):
                job_key = f"job:{{{server_type}}}:{reference}"
                record = self.client.hgetall(job_key)
                if record and (filter_server_id is None or record.get("serverId") == filter_server_id):
                    jobs.append((job_key, server_type, reference, record))
        duplicate_counts = Counter(record.get("jobId", "") for _, _, _, record in jobs)
        rows = []
        for job_key, server_type, reference, record in sorted(
            jobs,
            key=lambda item: (
                item[1], item[3].get("serverId", ""), item[3].get("jobId", "")
            ),
        ):
            job_id = record.get("jobId", reference)
            display_id = reference if duplicate_counts[job_id] > 1 else job_id
            rows.append((
                job_key, display_id,
                record.get("status", "UNKNOWN"),
                f"{server_type}/{record.get('serverId', '?')}",
            ))
        return rows

    def load_keys(self) -> list[tuple[str, str, str, str]]:
        found = set()
        for pattern in KEY_PATTERNS:
            found.update(self.client.scan_iter(match=pattern, count=500))
        rows = []
        for key in sorted(found):
            kind = self.client.type(key)
            if kind == "hash":
                detail = f"{self.client.hlen(key)} fields"
            elif kind == "set":
                detail = f"{self.client.scard(key)} members"
            elif kind == "string":
                detail = f"TTL={self.client.ttl(key)}s"
            else:
                detail = kind
            rows.append((key, key, "", f"{kind}: {detail}"))
        return rows

    async def show_details(self, key: str) -> None:
        try:
            detail = await asyncio.to_thread(self.read_key, key)
            self.query_one("#details", Static).update(detail)
        except Exception as exc:
            self.query_one("#details", Static).update(f"{key}\nError: {exc}")

    def read_key(self, key: str) -> str:
        kind = self.client.type(key)
        ttl = self.client.ttl(key)
        lines = [f"Redis key: {key}", f"Type: {kind} | TTL: {ttl}s"]
        if kind == "hash":
            lines.extend(
                f"{field}: {value}"
                for field, value in sorted(self.client.hgetall(key).items())
            )
        elif kind == "set":
            lines.extend(sorted(self.client.smembers(key)))
        elif kind == "string":
            lines.append(redis_text(self.client.get(key)))
        elif kind == "list":
            lines.extend(self.client.lrange(key, 0, -1))
        elif kind == "zset":
            lines.extend(
                f"{member}: {score}"
                for member, score in self.client.zrange(key, 0, -1, withscores=True)
            )
        elif kind == "stream":
            lines.extend(
                json.dumps(item, ensure_ascii=False)
                for item in self.client.xrange(key)
            )
        else:
            lines.append("(empty or unsupported Redis value)")
        return "\n".join(lines)

    def update_status(self, message: str) -> None:
        self.query_one("#status", Static).update(message)

    async def action_refresh_now(self) -> None:
        await self.refresh_view()

    async def action_show_overview(self) -> None:
        self.set_mode("overview")
        await self.refresh_view()
        self.query_one("#table", DataTable).focus()

    async def action_show_jobs(self) -> None:
        self.set_mode("jobs")
        await self.refresh_view()
        self.query_one("#table", DataTable).focus()

    async def action_show_keys(self) -> None:
        self.set_mode("keys")
        await self.refresh_view()
        self.query_one("#table", DataTable).focus()

    async def action_overview_back(self) -> None:
        await self.overview_back()


if __name__ == "__main__":
    RedisBrowser().run()
