# 3-colocated (test2) vs. 4-single-hash (test3)

## Executive summary

**Recommendation: retain 3-colocated as the preferred architecture unless the cross-slot registration edge case becomes an unacceptable operational or correctness risk.** The 4-single-hash design improves per-server-type atomicity by placing all keys for one server type in the same Redis Cluster hash slot. Its main cost is type discovery: without a global registry, discovering all server types requires a cluster-wide scan of primaries and is therefore slower and eventually consistent.

The 3-colocated registration weakness is real but narrow. A failure between its registry-slot and server-type-slot registration steps can leave a global registry membership without a corresponding per-type server record. It does **not** leave partial per-type server metadata/state, alter server or type counters, or corrupt existing job records. It is straightforward to reconcile by checking registry members against per-type state/metadata and removing orphan registry memberships.

## Designs at a glance

### 3-colocated — test2

The key layout separates the global registry from per-server-type data:

- Global type registry and per-type server membership live in the {registry} slot.
- A server type's summaries, lifecycle state, heartbeat, live/retired sets, job indexes, and job hashes use that type's hash tag, {serverType}.
- Job lifecycle scripts update the job record, server-level counters, type-level counters, and relevant indexes atomically within the server type's slot.
- Type discovery is efficient through the global registry; it does not require scanning every Cluster primary.

**Important registration boundary:** registration uses operations in two slots. It first adds the type/server ID to the {registry} slot, then creates/activates the server in the {serverType} slot. If the second operation fails before writing, the only residue is the global registry entry. There are no per-type artifacts for that failed registration. A retry can encounter the existing registry member, so reconciliation or retry-aware repair is needed.

Cleanup is also multi-phase, but its failure modes are different:
- Phase A fences and cleans server jobs/counters in the per-type slot. This protects job and counter integrity.
- Phase B removes registry membership in the registry slot. If it fails, discovery may show stale membership; the removal is idempotent and can be retried.
- Phase C collects retained tombstones after validating lifecycle and index state. It is not a cross-slot transaction, but the protocol ordering protects the intended registration/collection race under the documented assumptions.

### 4-single-hash — test3

All keys for a server type share one hash tag, {serverType}. The proposed layout includes:

- servers:{orders}:manifest
- servers:{orders}:summary
- servers:{orders}:state
- servers:{orders}:ids
- servers:{orders}:server:server-123
- servers:{orders}:heartbeat:server-123
- servers:{orders}:live
- servers:{orders}:retired
- servers:{orders}:jobs
- servers:{orders}:server:server-123:jobs
- servers:{orders}:job:<jobRef>

A manifest identifies the type and records lifecycle/schema metadata. Job lifecycle mutations can update the job record, both indexes, server counters, and type counters in one Lua script in that type's slot. This removes the separate registry/type-slot registration boundary and makes the per-type consistency boundary simpler.

Type discovery no longer relies on a global registry. The browser/maintenance process must scan every current primary for servers:*:manifest, validate discovered manifests, and tolerate the fact that Redis SCAN is node-local, incremental, and not a point-in-time snapshot. Cross-type dashboards are consequently best-effort/eventually consistent. A full cluster scan is for **type discovery**, not for every counter read or job operation; those remain per-type.

## Comparative grades

These are architecture/design assessments, not benchmark results or production certification. They assume the documented scripts and lifecycle protocols are followed.

| Criterion | 3-colocated (test2) | 4-single-hash (test3) | Why |
|---|---:|---:|---|
| Job/counter consistency within a type | A- | A | Both keep job/counter mutations in one type slot; 4 also consolidates server registration into that slot. |
| Registration failure recovery | B | A | 3 can leave a registry-only orphan after the first slot commits; 4 avoids that split registration. |
| Type discovery efficiency | A | C | 3 has a direct registry; 4 requires scanning Cluster primaries. |
| Operational simplicity | A- | B | 3 has a registry and multi-slot lifecycle bookkeeping; 4 has simpler per-type ownership but more complex discovery/operations. |
| Redis Cluster discovery scalability | A | C | A registry lookup is cheap; scanning every primary becomes more expensive as the cluster grows. |
| Ease of reconciliation | A- | B+ | 3's orphan is a registry membership only; 4 avoids that specific orphan but scan-based discovery and manifest consistency need operational care. |
| **Overall** | **A-** | **B+** | 3 is the better overall balance for the current requirements. |

## Impact and recovery of the 3-colocated registration edge case

The failure scenario is:

1. The registry-slot operation adds the server type and server ID to the global registry.
2. The process fails before the per-type-slot create/activate operation writes anything.
3. The registry can now advertise a server ID for which no per-type server state/metadata exists.

The impact is limited:
- A registry-driven listing/discovery view may temporarily show a phantom server or type.
- The failed registration does not create per-type server artifacts.
- It does not change server/type counters or existing job records.
- Normal job counter updates for other servers are unaffected.

A small idempotent reconciliation task can iterate registry type/server memberships, check for the corresponding per-type server state/metadata, and remove only memberships with no per-type server artifact. If a type has no remaining server IDs, it can remove that type from the global type registry. This repair should not touch job records or counters.

## Operational and correctness considerations

### Shared considerations

- Redis Lua scripts are atomic with respect to interleaving, but they do **not** provide rollback after a runtime error. Scripts should validate key types, arguments, index membership, ownership, and counter bounds before the first write.
- Per-type snapshots can be coherent when read in one Lua script with all required keys declared. Multi-page reads and cross-type dashboards are not a single global snapshot.
- Stale index members and missing hashes should be handled defensively by browsers and maintenance tools.
- Heartbeat cleanup should fence a server before reconciling its jobs and counters. Large cleanups should use bounded, retryable batches where appropriate.
- Tombstones/retired records should remain long enough to prevent accidental reuse and to support safe cleanup/re-registration rules.
- Server IDs should be unique per process incarnation within a server type, or otherwise include an incarnation/fencing token.

### Additional 3-colocated considerations

- Treat registration as a recoverable two-slot workflow. Make retry behavior explicit or run registry reconciliation.
- Treat Phase B registry removal as retryable eventual consistency, not as part of the per-type job/counter transaction.
- Keep the documented Phase C ordering assumptions; do not externally mutate managed lifecycle keys.

### Additional 4-single-hash considerations

- Cluster-wide discovery must scan **each current primary**; a scan against only one node is incomplete. Handle topology changes and duplicate/partial scan observations.
- SCAN is incremental and node-local. A discovered type list is an operational inventory, not a linearizable global snapshot.
- Persist manifests and define clear ACTIVE/RETIRING/RETIRED semantics. Do not silently delete manifests while readers or maintenance processes may still depend on them.
- Migration requires coordinated reader/writer rollout. Do not stop updating the old registry while any deployed reader still depends on it.
- Confirm the Redis client library's Cluster scan behavior rather than assuming a single scan_iter call fans out to all primaries.

## UI and access patterns

The user-facing hierarchy can remain the same in either design:

1. Server type summary
2. Servers for that type
3. Jobs for a selected server

A type-wide jobs view can use the existing type-level jobs index. In 3-colocated, the browser already has the jobs:type:{serverType} index; the j toggle added to test2/3-colocated/browser/redis_browser.py switches between the normal server hierarchy and all jobs for the selected type. This is a browser presentation feature and does not require a Redis schema change.

In 4-single-hash, the equivalent type-wide index is servers:{serverType}:jobs (per the proposed layout), with server-specific indexes retained for drill-down.

## Test coverage needed before production approval

Neither architecture should be considered production-verified from static source review alone. At minimum, test:

- Wrong-type keys for every index, summary, state, heartbeat, lease, and job key.
- Missing, stale, duplicate, or mismatched index memberships.
- Malformed, negative, fractional, out-of-range, and int64-boundary counter values.
- Registration failures injected between each operation, including process termination after the first write.
- Retry and reconciliation behavior for registry-only orphan memberships (3-colocated).
- Fence/cleanup retries, partial batches, repeated cleanup, and tombstone retention/collection.
- Concurrent job transitions against server cleanup and deletion.
- Redis Cluster slot placement for every script key.
- Full-primary discovery behavior, topology changes, and incomplete SCAN iterations (4-single-hash).
- Snapshot pagination while jobs are being added, transitioned, and removed.
- Browser behavior with missing hashes, stale index references, and refreshes during updates.

## Implementation status and confidence

- **3-colocated (test2):** the design, Lua audit, and browser implementation have been reviewed at source level. The j type-wide jobs toggle was pushed to 3-colocated/browser/redis_browser.py. This does not imply live Redis or interactive UI testing.
- **4-single-hash (test3):** design documentation and implementation files/utilities/browser were created from the proposed design. They have not been established as compiled, syntax-checked, or integration-tested in this comparison. In particular, Cluster-wide SCAN fan-out and Redis client API compatibility require verification.
- The grades above are therefore comparative engineering judgments, not measured performance scores or a claim that either implementation has passed production qualification.

## Final recommendation

Keep **3-colocated** as the current preferred architecture. Its cross-slot registration weakness is real, but its failure residue is a registry-only orphan that can be repaired without touching per-type counters or jobs. Its registry also gives a substantially more efficient and scalable type-discovery path.

Choose **4-single-hash** if eliminating the cross-slot registration boundary is a higher priority than efficient type discovery, and if the operational cost and eventual-consistency properties of scanning every Cluster primary are acceptable. Before switching, validate the actual Redis Cluster client behavior, compile/syntax-check the implementation, and run the failure-injection and concurrency tests listed above.
