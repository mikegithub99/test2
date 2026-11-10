"""Interactive browser for RedisJobStatus counters and metadata."""

from __future__ import annotations

import asyncio
import os
from datetime import datetime, timezone
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

def format_metadata(metadata: dict[str, str]) -> dict[str, str]:
    """Render Redis-millisecond registeredAt as a readable UTC timestamp."""
    formatted = dict(metadata)
    value = formatted.get("registeredAt")
    if value is not None:
        try:
            milliseconds = int(value)
            formatted["registeredAt"] = (
                datetime.fromtimestamp(milliseconds / 1000, tz=timezone.utc)
                .isoformat(timespec="milliseconds")
                .replace("+00:00", "Z")
            )
        except (ValueError, OverflowError, OSError):
            pass
    return formatted


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
        details = self.title_text + "\n\n"
        details += "\n".join(
            f"{name}: {value}" for name, value in sorted(self.metadata.items())
        ) if self.metadata else "(no metadata)"
        yield Static(details + "\n\nPress Enter to close.", id="metadata-info")
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
            type_metadata = format_metadata({k[5:]: v for k, v in type_hash.items() if k.startswith("meta:")})
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
                server_metadata = format_metadata({k[5:]: v for k, v in server_hash.items() if k.startswith("meta:")})
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

    @staticmethod
    def _eval_checked(client, script: str, keys: list[str], args: list[str], operation: str) -> int:
        """Run one atomic Lua operation and turn Redis errors into clear failures."""
        result = client.eval(script, len(keys), *(keys + args))
        if isinstance(result, bytes):
            result = result.decode()
        if isinstance(result, str) and result.startswith("ERR"):
            raise RuntimeError(f"{operation}: {result}")
        return int(result)

    def _delete_job(self, job_key: str, metadata: dict[str, str]) -> None:
        # The row may be stale. The script reads the current status and owner,
        # validates both indexes and all four counters, then mutates atomically.
        server_type = metadata.get("serverType")
        server_id = metadata.get("serverId")
        job_id = metadata.get("jobId")
        if not server_type or not server_id or job_id is None:
            raise ValueError("Job metadata incomplete; refusing deletion.")
        reference = f"{len(server_id.encode('utf-8'))}:{server_id}:{job_id}"
        expected_key = f"job:{{{server_type}}}:{reference}"
        if job_key != expected_key:
            raise ValueError("Job key does not match its stored identity.")

        script = r"""
local job, typeJobs, serverJobs = KEYS[1], KEYS[2], KEYS[3]
local typeSummary, serverSummary, state = KEYS[4], KEYS[5], KEYS[6]
local function typ(k) return redis.call('TYPE', k).ok end
local function valid(k, t) local v=typ(k); return v=='none' or v==t end
if typ(state) ~= 'hash' then return 4 end
if redis.call('HGET', state, ARGV[3]) ~= 'ACTIVE' then return 7 end
if typ(job) ~= 'hash' or not valid(typeJobs,'set') or
   not valid(serverJobs,'set') or typ(typeSummary) ~= 'hash' or
   typ(serverSummary) ~= 'hash' then return 4 end
local status=redis.call('HGET',job,'status')
if redis.call('HGET',job,'serverType') ~= ARGV[2] or
   redis.call('HGET',job,'serverId') ~= ARGV[3] or
   redis.call('HGET',job,'jobId') ~= ARGV[1] then return 4 end
if status ~= 'PENDING' and status ~= 'ACTIVE' and status ~= 'SUSPENDED' then return 4 end
if redis.call('SISMEMBER',typeJobs,ARGV[4]) ~= 1 or
   redis.call('SISMEMBER',serverJobs,ARGV[4]) ~= 1 then return 4 end
local field=string.lower(status)
local max='9223372036854775807'
local function positive(k,f)
  local raw=redis.call('HGET',k,f) or '0'
  if not string.match(raw,'^%d+$') then return false end
  local n=string.gsub(raw,'^0+',''); if n=='' then n='0' end
  return n~='0' and (#n < #max or (#n == #max and n <= max))
end
if not positive(typeSummary,field) or not positive(typeSummary,'total') or
   not positive(serverSummary,field) or not positive(serverSummary,'total') then return 4 end
redis.call('HINCRBY',typeSummary,field,-1)
redis.call('HINCRBY',typeSummary,'total',-1)
redis.call('HINCRBY',serverSummary,field,-1)
redis.call('HINCRBY',serverSummary,'total',-1)
redis.call('SREM',typeJobs,ARGV[4])
redis.call('SREM',serverJobs,ARGV[4])
redis.call('DEL',job)
return 0
"""
        keys = [
            job_key,
            f"jobs:type:{{{server_type}}}",
            f"jobs:server:{{{server_type}}}:{server_id}",
            f"summary:type:{{{server_type}}}",
            f"summary:server:{{{server_type}}}:{server_id}",
            f"servers:state:{{{server_type}}}",
        ]
        result = self._eval_checked(
            self.client, script, keys,
            [job_id, server_type, server_id, reference], "Delete job"
        )
        if result == 2:
            return
        if result == 7:
            raise RuntimeError("Server is not ACTIVE; its cleanup lifecycle owns this job.")
        if result != 0:
            raise RuntimeError(f"Delete job rejected: inconsistent Redis data (code {result}).")

    def _delete_server(self, summary_key: str) -> None:
        """Force-fence and clean one server atomically in its per-type slot."""
        prefix = "summary:server:{"
        if not summary_key.startswith(prefix) or "}:" not in summary_key:
            raise ValueError("Invalid server summary key.")
        server_type, server_id = summary_key[len(prefix):].split("}:", 1)
        state = f"servers:state:{{{server_type}}}"
        heartbeat = f"heartbeat:server:{{{server_type}}}:{server_id}"
        server_jobs = f"jobs:server:{{{server_type}}}:{server_id}"
        type_jobs = f"jobs:type:{{{server_type}}}"
        server_summary = summary_key
        type_summary = f"summary:type:{{{server_type}}}"
        live = f"servers:live:{{{server_type}}}"
        retired = f"servers:retired:{{{server_type}}}"

        # This is the administrative equivalent of Phase A. Unlike expiry
        # cleanup it deliberately bypasses heartbeat eligibility, but it uses
        # the same per-type-slot fence, complete preflight, accounting and
        # tombstone semantics. All validation happens before the first write.
        script = r"""
local live,state,hb,sjobs,tjobs,ssum,tsum,retired =
  KEYS[1],KEYS[2],KEYS[3],KEYS[4],KEYS[5],KEYS[6],KEYS[7],KEYS[8]
local id,serverType,count=ARGV[1],ARGV[2],tonumber(ARGV[3])
if not count or count<0 or count~=math.floor(count) or #KEYS~=8+count then return redis.error_reply('invalid admin cleanup arguments') end
local function typ(k) return redis.call('TYPE',k).ok end
local function valid(k,t) local v=typ(k); return v=='none' or v==t end
if not valid(live,'set') or not valid(state,'hash') or not valid(hb,'string') or
   not valid(sjobs,'set') or not valid(tjobs,'set') or not valid(ssum,'hash') or
   not valid(tsum,'hash') or not valid(retired,'zset') then return redis.error_reply('wrong key type during admin cleanup') end
local current=redis.call('HGET',state,id)
if not current then
  if typ(sjobs)=='none' and typ(ssum)=='none' and redis.call('SISMEMBER',live,id)==0 then return 2 end
  return redis.error_reply('server artifacts exist without a lifecycle state')
end
if current~='ACTIVE' and current~='DELETING' then return redis.error_reply('server has unexpected lifecycle state: '..current) end
local refs=redis.call('SMEMBERS',sjobs)
if #refs~=count then return redis.error_reply('server job index changed; retry deletion') end
local supplied={}
for i=1,count do supplied[ARGV[3+i]]=true end
for _,ref in ipairs(refs) do if not supplied[ref] then return redis.error_reply('job reference missing from cleanup arguments') end end
local amounts={pending=0,active=0,suspended=0,total=0}
for i=1,count do
  local ref=ARGV[3+i]
  local jk=KEYS[8+i]
  if typ(jk)~='hash' then return redis.error_reply('missing or wrong-type job hash') end
  if redis.call('HGET',jk,'serverId')~=id or redis.call('HGET',jk,'serverType')~=serverType then return redis.error_reply('job ownership mismatch') end
  if redis.call('SISMEMBER',sjobs,ref)~=1 or redis.call('SISMEMBER',tjobs,ref)~=1 then return redis.error_reply('job index membership missing') end
  local status=redis.call('HGET',jk,'status')
  if status=='PENDING' then amounts.pending=amounts.pending+1
  elseif status=='ACTIVE' then amounts.active=amounts.active+1
  elseif status=='SUSPENDED' then amounts.suspended=amounts.suspended+1
  else return redis.error_reply('unsupported job status') end
  amounts.total=amounts.total+1
end
local max='9223372036854775807'
local function enough(field,dec)
  if dec==0 then return true end
  local raw=redis.call('HGET',tsum,field) or '0'
  if not string.match(raw,'^%d+$') then return false end
  local n=string.gsub(raw,'^0+',''); if n=='' then n='0' end
  local d=tostring(dec)
  if #n>#max or (#n==#max and n>max) then return false end
  if #n<#d or (#n==#d and n<d) then return false end
  return true
end
for field,dec in pairs(amounts) do if not enough(field,dec) then return redis.error_reply('invalid or insufficient type counter: '..field) end end
-- Commit: fence and clean in this one atomic per-type-slot script.
local now=redis.call('TIME')
local nowMs=tonumber(now[1])*1000+math.floor(tonumber(now[2])/1000)
redis.call('HSET',state,id,'DELETING')
redis.call('ZADD',retired,'NX',nowMs,id)
for field,dec in pairs(amounts) do if dec>0 then redis.call('HINCRBY',tsum,field,-dec) end end
for i=1,count do redis.call('SREM',tjobs,ARGV[3+i]); redis.call('DEL',KEYS[8+i]) end
redis.call('DEL',sjobs,ssum,hb)
redis.call('SREM',live,id)
return 1
"""
        refs = sorted(self.client.smembers(server_jobs))
        job_keys = [f"job:{{{server_type}}}:{ref}" for ref in refs]
        result = self._eval_checked(
            self.client, script,
            [live, state, heartbeat, server_jobs, type_jobs, server_summary,
             type_summary, retired] + job_keys,
            [server_id, server_type, str(len(refs))] + refs,
            "Delete server",
        )
        if result not in (1, 2):
            raise RuntimeError(f"Delete server rejected (code {result}).")

        # Registry is a different Cluster slot: Phase B must be separate.
        self.client.srem(f"servers:{{registry}}:type:{server_type}", server_id)

    def _delete_server_type(self, server_type: str) -> None:
        """Delete a type only after all discoverable instances are fenced/cleaned."""
        registry_set = f"servers:{{registry}}:type:{server_type}"
        state_key = f"servers:state:{{{server_type}}}"
        type_jobs = f"jobs:type:{{{server_type}}}"
        # Union registry members with lifecycle hash fields. This also catches
        # state records whose registry membership was lost.
        server_ids = set(self.client.smembers(registry_set))
        server_ids.update(
            server_id for server_id in self.client.hkeys(state_key)
            if not server_id.startswith("registeredAt:")
        )
        # Include orphaned per-server keys even if registry membership and
        # lifecycle state were lost. RedisCluster.scan_iter visits primaries.
        for pattern in (
            f"summary:server:{{{server_type}}}:*",
            f"jobs:server:{{{server_type}}}:*",
        ):
            prefix = (
                f"summary:server:{{{server_type}}}:"
                if pattern.startswith("summary:") else
                f"jobs:server:{{{server_type}}}:"
            )
            for key in self.client.scan_iter(match=pattern, count=500):
                server_ids.add(key[len(prefix):])
        for server_id in sorted(server_ids):
            self._delete_server(f"summary:server:{{{server_type}}}:{server_id}")

        # Do not erase the aggregate summary if orphan job references remain:
        # doing so would conceal inconsistent indexes/counters.
        leftovers = self.client.smembers(type_jobs)
        if leftovers:
            raise RuntimeError(
                f"Type deletion stopped: {len(leftovers)} orphan job reference(s) "
                "remain in the type index; inspect/reconcile before retrying."
            )

        # All per-type keys share the {serverType} slot. Registry keys are
        # intentionally removed separately because they use {registry}.
        self.client.delete(
            type_jobs,
            f"summary:type:{{{server_type}}}",
            state_key,
            f"servers:live:{{{server_type}}}",
            f"servers:retired:{{{server_type}}}",
        )
        self.client.delete(registry_set)
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
