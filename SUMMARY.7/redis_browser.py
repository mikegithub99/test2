"""Interactive browser for RedisJobStatus counters and metadata."""

from __future__ import annotations

import asyncio
import os
from datetime import datetime
from typing import Any

import redis
from redis.cluster import RedisCluster
from textual import events
from textual.app import App, ComposeResult
from textual.screen import ModalScreen
from textual.widgets import DataTable, Footer, Header, Static
from rich.text import Text

REFRESH_OPTIONS = (1, 2, 5, 10, 15, 30)


def make_client():
    url = os.getenv("REDIS_URL", "redis://127.0.0.1:6379")
    if os.getenv("REDIS_CLUSTER", "").lower() in {"1", "true", "yes"}:
        return RedisCluster.from_url(url, decode_responses=True)
    return redis.Redis.from_url(url, decode_responses=True)


def counter_values(summary: dict[str, str]) -> tuple[str, str, str, str]:
    """Return the four materialized lifecycle counters in display order."""
    return tuple(
        str(int(summary.get(name, "0")))
        for name in ("pending", "active", "suspended", "total")
    )


class JobDetailsScreen(ModalScreen[None]):
    """Modal view of every field stored in a selected job hash."""

    BINDINGS = [("enter", "dismiss", "Close")]

    def on_click(self, event: events.Click) -> None:
        """Dismiss the details popup when the user clicks it."""
        self.dismiss()

    def __init__(self, job_key: str, metadata: dict[str, str]) -> None:
        super().__init__()
        self.job_key = job_key
        self.metadata = metadata

    def compose(self) -> ComposeResult:
        details = "JOB METADATA\nKey: " + self.job_key + "\n\n"
        details += "\n".join(
            f"{name}: {value}" for name, value in sorted(self.metadata.items())
        )
        yield Static(details, id="job-details")
        yield Footer()


class MetadataInfoScreen(ModalScreen[None]):
    """Modal view of metadata attached to a server type or server ID."""

    BINDINGS = [("enter", "dismiss", "Close")]
    CSS = """MetadataInfoScreen { align: center middle; } #metadata-info { width: 80%; height: auto; max-height: 80%; border: round $accent; padding: 1 2; background: $surface; overflow-y: auto; }"""

    def __init__(self, title: str, metadata: dict[str, str]) -> None:
        super().__init__()
        self.title_text = title
        self.metadata = metadata

    def compose(self) -> ComposeResult:
        details = self.title_text + "\\n\\n"
        details += "\\n".join(
            f"{name}: {value}" for name, value in sorted(self.metadata.items())
        ) if self.metadata else "(no metadata)"
        yield Static(details + "\\n\\nPress Enter to close.", id="metadata-info")
        yield Footer()


class DeleteConfirmScreen(ModalScreen[bool]):
    BINDINGS = [("y", "confirm", "Delete"), ("n", "cancel", "Cancel"), ("escape", "cancel", "Cancel")]
    CSS = """DeleteConfirmScreen { align: center middle; } #delete-confirm { width: 80%; height: auto; border: round $error; padding: 1 2; background: $surface; }"""
    def __init__(self, title: str, description: str) -> None:
        super().__init__()
        self.title_text, self.description = title, description
    def compose(self) -> ComposeResult:
        yield Static(f"DELETE CONFIRMATION\n{self.title_text}\n{self.description}\nPress Y to delete, N or Esc to cancel.", id="delete-confirm")
    def action_confirm(self) -> None:
        self.dismiss(True)
    def action_cancel(self) -> None:
        self.dismiss(False)


class RedisBrowser(App):
    TITLE = "Redis Job Counters (.7)"
    SUB_TITLE = "Live hierarchy | D deletes | I shows metadata"

    BINDINGS = [
        ("1", "refresh_now", "Refresh"),
        ("2", "toggle_pause", "Pause / Resume"),
        ("3", "slower", "Slower"),
        ("4", "faster", "Faster"),
        ("h", "toggle_help", "Help"),
        ("d", "delete_selected", "Delete"),
        ("i", "show_info", "Info"),
        ("space", "toggle_expand", "Expand / Collapse"),
        ("escape", "collapse", "Collapse"),
        ("q", "quit", "Quit"),
    ]

    CSS = """
    Screen { layout: vertical; }
    #table { height: 1fr; }
    #status { height: 1; padding: 0 1; color: $text-muted; }
    JobDetailsScreen { align: center middle; }
    #job-details { width: 90%; height: 85%; border: round $accent; padding: 1 2; background: $surface; overflow-y: auto; }
    #controls {
        height: 1;
        padding: 0 1;
        background: $boost;
        color: $text;
    }
    """

    def __init__(self) -> None:
        super().__init__()
        self.client = make_client()
        self.interval_index = REFRESH_OPTIONS.index(2)
        self.paused = False
        self.help_visible = True
        self._refreshing = False
        self._timer = None
        self.last_rows = []
        self.expanded_types: set[str] = set()
        self.expanded_servers: set[str] = set()
        self.table: DataTable | None = None
        self.status: Static | None = None

    @property
    def interval(self) -> int:
        return REFRESH_OPTIONS[self.interval_index]

    def compose(self) -> ComposeResult:
        yield Header()
        yield DataTable(id="table", cursor_type="row", zebra_stripes=True)
        yield Static("Starting…", id="status")
        yield Static(
            "Click/Enter Select   Space Expand   I Info   Esc Collapse   D Delete   1 Refresh   2 Pause   3 Slower   4 Faster   Q Quit",
            id="controls",
        )

    async def on_mount(self) -> None:
        table = self.query_one("#table", DataTable)
        self.table = table
        self.status = self.query_one("#status", Static)
        # Keep the hierarchy and counters in one column. Separate DataTable
        # columns can horizontally scroll the hierarchy label out of view on
        # narrower terminals.
        table.add_column(
            f"{'Hierarchy':<62}{'Pending':>7} {'Active':>7} {'Suspended':>10} {'Total':>6}",
            key="display",
        )
        self._timer = self.set_interval(self.interval, self.refresh_view)
        table.focus()
        await self.refresh_view()

    async def on_unmount(self) -> None:
        if self._timer:
            self._timer.stop()
        try:
            await asyncio.to_thread(self.client.close)
        except Exception:
            pass

    def load_hierarchy(self):
        """Read the complete hierarchy, including parent and depth metadata."""
        rows = []
        server_types = sorted(self.client.smembers("servers:{registry}:types"))

        for server_type in server_types:
            type_key = f"summary:type:{{{server_type}}}"
            type_hash = self.client.hgetall(type_key)
            type_counters = counter_values(type_hash)
            type_metadata = {k[5:]: v for k, v in type_hash.items() if k.startswith("meta:")}
            server_set_key = f"servers:{{registry}}:type:{server_type}"
            server_ids = sorted(self.client.smembers(server_set_key))
            rows.append((
                type_key, server_type, *type_counters, 0, None,
                bool(server_ids), type_metadata,
            ))

            for server_id in server_ids:
                server_key = f"summary:server:{{{server_type}}}:{server_id}"
                server_hash = self.client.hgetall(server_key)
                server_counters = counter_values(server_hash)
                server_metadata = {k[5:]: v for k, v in server_hash.items() if k.startswith("meta:")}
                jobs_set_key = f"jobs:server:{{{server_type}}}:{server_id}"
                references = sorted(self.client.smembers(jobs_set_key))
                rows.append((
                    server_key, f"SERVER: {server_id}", *server_counters,
                    1, type_key, bool(references), server_metadata,
                ))

                for reference in references:
                    job_key = f"job:{{{server_type}}}:{reference}"
                    job = self.client.hgetall(job_key)
                    if not job:
                        continue
                    job_id = job.get("jobId", reference)
                    status = job.get("status", "UNKNOWN").upper()
                    rows.append((
                        job_key, f"JOB: {job_id} [{status}]",
                        "1" if status == "PENDING" else "0",
                        "1" if status == "ACTIVE" else "0",
                        "1" if status == "SUSPENDED" else "0",
                        "1", 2, server_key, False, dict(job),
                    ))
        return rows

    def visible_hierarchy(self, all_rows):
        """Return only rows whose parent levels are expanded."""
        by_key = {row[0]: row for row in all_rows}
        visible = []
        for row in all_rows:
            key, label, pending, active, suspended, total, level, parent, has_children = row[:9]
            if level == 0:
                visible.append(row)
            elif level == 1 and parent in self.expanded_types:
                visible.append(row)
            elif level == 2 and parent in self.expanded_servers:
                parent_server = by_key.get(parent)
                if parent_server and parent_server[7] in self.expanded_types:
                    visible.append(row)
        return visible

    async def refresh_view(self) -> None:
        if self._refreshing:
            return
        self._refreshing = True
        try:
            all_rows = await asyncio.to_thread(self.load_hierarchy)
            rows = self.visible_hierarchy(all_rows)
            table = self.table
            status_widget = self.status
            if table is None or status_widget is None:
                return
            old_cursor_row = table.cursor_row
            old_cursor_key = None
            if 0 <= old_cursor_row < len(self.last_rows):
                old_cursor_key = self.last_rows[old_cursor_row][0]
            had_focus = table.has_focus

            self.last_rows = rows
            table.clear()
            for row in rows:
                key, label, pending, active, suspended, total = row[:6]
                level, parent, has_children = row[6:9]
                is_expanded = (
                    (level == 0 and key in self.expanded_types)
                    or (level == 1 and key in self.expanded_servers)
                )
                marker = "[-]" if has_children and is_expanded else (
                    "[+]" if has_children else "   "
                )
                # Indentation belongs inside a fixed-width label field;
                # counters therefore start at the same screen column at
                # every hierarchy depth.
                item_label = f"{'  ' * level}{marker} {label}"
                display = (
                    f"{item_label:<62.62}"
                    f"{pending:>7} {active:>7} {suspended:>10} {total:>6}"
                )
                table.add_row(Text(display), key=key)

            restored_index = None
            if old_cursor_key:
                restored_index = next(
                    (i for i, row in enumerate(rows) if row[0] == old_cursor_key),
                    None,
                )
            if restored_index is None and rows:
                restored_index = min(max(old_cursor_row, 0), len(rows) - 1)
            if restored_index is not None:
                table.move_cursor(row=restored_index, column=0, animate=False)
            if had_focus:
                table.focus()

            state = "PAUSED" if self.paused else f"refresh every {self.interval}s"
            status_widget.update(
                f"{len(rows)} visible rows | {state} | Updated "
                f"{datetime.now().astimezone().strftime('%H:%M:%S %Z')}"
            )
        except Exception as exc:
            if self.status is not None:
                self.status.update(f"Redis error: {exc}")
        finally:
            self._refreshing = False

    async def on_data_table_row_selected(
        self, event: DataTable.RowSelected
    ) -> None:
        """Enter and mouse selection share one handler for hierarchy and jobs."""
        if not 0 <= event.cursor_row < len(self.last_rows):
            return
        self.query_one("#table", DataTable).move_cursor(
            row=event.cursor_row, column=0, animate=False
        )
        row = self.last_rows[event.cursor_row]
        if row[6] == 2:
            metadata = row[9] if len(row) > 9 else {}
            self.push_screen(JobDetailsScreen(row[0], metadata))
        else:
            await self.action_toggle_expand()

    async def action_collapse(self) -> None:
        """Collapse the selected hierarchy level, or its parent when on a job."""
        table = self.table
        if table is None:
            return
        row_index = table.cursor_row
        if not 0 <= row_index < len(self.last_rows):
            return

        row = self.last_rows[row_index]
        key, _label, _p, _a, _s, _t, level, parent, _has_children = row[:9]

        if level == 2:
            # A job row collapses its containing server, avoiding the need to
            # navigate back through a long list of jobs.
            collapse_key = parent
            collapse_level = 1
        elif level == 1:
            # Escape on any server row returns to the server-type level.
            collapse_key = parent
            collapse_level = 0
        else:
            collapse_key = key
            collapse_level = 0

        if collapse_level == 1:
            if collapse_key not in self.expanded_servers:
                return
            self.expanded_servers.discard(collapse_key)
        else:
            if collapse_key not in self.expanded_types:
                return
            self.expanded_types.discard(collapse_key)
            # Also clear expansion state for servers under this type.
            for child in self.last_rows:
                if child[6] == 1 and child[7] == collapse_key:
                    self.expanded_servers.discard(child[0])

        # Move the cursor to the row that will remain visible after collapse.
        parent_index = next(
            (i for i, item in enumerate(self.last_rows) if item[0] == collapse_key),
            None,
        )
        if parent_index is not None:
            table.move_cursor(row=parent_index, column=0, animate=False)
        await self.refresh_view()

    async def action_show_info(self) -> None:
        """Show metadata for the selected server type or server ID."""
        table = self.table
        if table is None or not 0 <= table.cursor_row < len(self.last_rows):
            return
        row = self.last_rows[table.cursor_row]
        if row[6] not in (0, 1):
            return
        metadata = row[9] if len(row) > 9 else {}
        title = (f"SERVER TYPE: {row[1]}" if row[6] == 0
                 else f"SERVER ID: {row[1].removeprefix('SERVER: ')}")
        self.push_screen(MetadataInfoScreen(title, metadata))

    async def action_delete_selected(self) -> None:
        table = self.table
        if table is None or not 0 <= table.cursor_row < len(self.last_rows):
            return
        row = self.last_rows[table.cursor_row]
        key, label, level = row[0], row[1], row[6]
        metadata = dict(row[9]) if level == 2 and len(row) > 9 else {}
        if level == 0:
            title, description = f"Server type: {label}", "Delete all servers, jobs, summaries, indexes, heartbeats, and registry entries for this type?"
        elif level == 1:
            title = f"Server ID: {label.removeprefix('SERVER: ')}"
            description = "Delete this server's jobs, summary, heartbeat, job index, and registry membership?"
        elif level == 2:
            title = f"Job: {metadata.get('jobId', label)}"
            description = "Delete this job hash and both index references, and decrement server and type counters?"
        else:
            return
        self.push_screen(DeleteConfirmScreen(title, description), callback=lambda confirmed: self._finish_delete(confirmed, key, level, metadata))

    async def _finish_delete(self, confirmed: bool, key: str, level: int, metadata: dict[str, str]) -> None:
        if not confirmed:
            return
        try:
            if level == 2:
                await asyncio.to_thread(self._delete_job, key, metadata)
            elif level == 1:
                await asyncio.to_thread(self._delete_server, key)
            else:
                await asyncio.to_thread(self._delete_server_type, key[len("summary:type:{"):-1])
            await self.refresh_view()
        except Exception as exc:
            if self.status is not None:
                self.status.update(f"Delete failed: {exc}")

    def _delete_job(self, job_key: str, metadata: dict[str, str]) -> None:
        server_type, server_id = metadata.get("serverType"), metadata.get("serverId")
        state = metadata.get("status", "").lower()
        if not server_type or not server_id or state not in {"pending", "active", "suspended"}:
            raise ValueError("Job metadata incomplete; refusing deletion.")
        prefix = f"job:{{{server_type}}}:"
        if not job_key.startswith(prefix):
            raise ValueError("Job key does not match its metadata.")
        reference = job_key[len(prefix):]
        if not self.client.exists(job_key):
            return
        for summary in (f"summary:type:{{{server_type}}}", f"summary:server:{{{server_type}}}:{server_id}"):
            if self.client.exists(summary):
                self.client.hincrby(summary, state, -1)
                self.client.hincrby(summary, "total", -1)
        self.client.srem(f"jobs:type:{{{server_type}}}", reference)
        self.client.srem(f"jobs:server:{{{server_type}}}:{server_id}", reference)
        self.client.delete(job_key)

    def _delete_server(self, summary_key: str) -> None:
        prefix = "summary:server:{"
        if not summary_key.startswith(prefix) or "}:" not in summary_key:
            raise ValueError("Invalid server summary key.")
        server_type, server_id = summary_key[len(prefix):].split("}:", 1)
        server_jobs, type_jobs = f"jobs:server:{{{server_type}}}:{server_id}", f"jobs:type:{{{server_type}}}"
        type_summary = f"summary:type:{{{server_type}}}"
        counts = {"pending": 0, "active": 0, "suspended": 0, "total": 0}
        for reference in list(self.client.smembers(server_jobs)):
            job_key = f"job:{{{server_type}}}:{reference}"
            state = self.client.hget(job_key, "status")
            if state and state.lower() in counts:
                counts[state.lower()] += 1
                counts["total"] += 1
            self.client.delete(job_key)
            self.client.srem(type_jobs, reference)
        if self.client.exists(type_summary):
            for field, amount in counts.items():
                if amount:
                    self.client.hincrby(type_summary, field, -amount)
        self.client.delete(server_jobs, summary_key, f"heartbeat:server:{{{server_type}}}:{server_id}")
        self.client.srem(f"servers:{{registry}}:type:{server_type}", server_id)

    def _delete_server_type(self, server_type: str) -> None:
        server_set = f"servers:{{registry}}:type:{server_type}"
        for server_id in list(self.client.smembers(server_set)):
            self._delete_server(f"summary:server:{{{server_type}}}:{server_id}")
        type_jobs = f"jobs:type:{{{server_type}}}"
        for reference in list(self.client.smembers(type_jobs)):
            self.client.delete(f"job:{{{server_type}}}:{reference}")
        self.client.delete(type_jobs, f"summary:type:{{{server_type}}}", server_set)
        self.client.srem("servers:{registry}:types", server_type)

    async def action_toggle_expand(self) -> None:
        table = self.table
        if table is None:
            return
        row_index = table.cursor_row
        if not 0 <= row_index < len(self.last_rows):
            return
        row = self.last_rows[row_index]
        key, _label, _p, _a, _s, _t, level, _parent, has_children = row[:9]
        if not has_children:
            return
        if level == 0:
            if key in self.expanded_types:
                self.expanded_types.remove(key)
                # Clear child expansion state when collapsing a server type.
                for child in self.last_rows:
                    if child[6] == 1 and child[7] == key:
                        self.expanded_servers.discard(child[0])
            else:
                self.expanded_types.add(key)
        elif level == 1:
            if key in self.expanded_servers:
                self.expanded_servers.remove(key)
            else:
                self.expanded_servers.add(key)
        await self.refresh_view()

    def restart_timer(self) -> None:
        if self._timer:
            self._timer.stop()
        self._timer = None
        if not self.paused:
            self._timer = self.set_interval(self.interval, self.refresh_view)

    async def action_toggle_help(self) -> None:
        """Hide or show the bottom status and keyboard-help lines."""
        self.help_visible = not self.help_visible
        if self.status is not None:
            self.status.display = self.help_visible
        controls = self.query_one("#controls", Static)
        controls.display = self.help_visible

    async def action_refresh_now(self) -> None:
        await self.refresh_view()

    async def action_toggle_pause(self) -> None:
        self.paused = not self.paused
        self.restart_timer()
        await self.refresh_view()

    async def action_slower(self) -> None:
        self.interval_index = min(
            self.interval_index + 1, len(REFRESH_OPTIONS) - 1
        )
        self.restart_timer()
        await self.refresh_view()

    async def action_faster(self) -> None:
        self.interval_index = max(self.interval_index - 1, 0)
        self.restart_timer()
        await self.refresh_view()


if __name__ == "__main__":
    RedisBrowser().run()
