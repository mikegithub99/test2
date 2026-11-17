# Timestamp-and-Token Reconciliation for Partial Registration

## Status and scope

This document proposes the source changes needed to recover partial registration using a Redis-generated registration timestamp and a unique per-attempt token. It is a design proposal, not an implementation or test result.

The proposal deliberately keeps the existing registry-first ordering and the existing three-phase cleanup architecture. It does **not** move registry publication to the end, and it does **not** require adding `REGISTERING` as a value in the server-state hash. Instead, it uses a per-server registration-token field in that hash as a conditional claim/fencing marker while the public lifecycle state remains absent until activation.

The type slot remains authoritative for lifecycle state and fencing. The registry slot remains authoritative for discovery and pending-registration age. Because the slots differ, no cross-slot operation is atomic; the workflow must converge through retries.

## Existing key layout and proposed additions

All key patterns below follow the conventions in `README.md`.

| Redis key pattern | Slot | Existing/proposed role | Change |
|---|---|---|---|
| `servers:{registry}:types` | `{registry}` | Global server-type discovery SET | No semantic change; continue adding the type during registration. |
| `servers:{registry}:type:<serverType>` | `{registry}` | Per-type registered-server IDs SET | Continue adding the ID at registration start and removing it in Phase B. |
| `servers:{registry}:registration:<serverType>` | `{registry}` | **New HASH**; field is <serverId>, value contains <token> and <registeredAtMs> | Add token and Redis-time timestamp atomically with registry membership. Remove the field conditionally in Phase B. |
| `servers:state:{<serverType>}` | `{serverType}` | Per-server lifecycle state HASH | Add a per-server token field `registrationToken:<serverId>`; write it with an atomic conditional claim before metadata initialization. Continue storing `ACTIVE` / `DELETING` under the server ID field. |
| `summary:type:{<serverType>}` | `{serverType}` | Shared type counters and metadata HASH | No new registration identity fields. Make initialization conditional on the matching per-server token claim. |
| `summary:server:{<serverType>}:<serverId>` | `{serverType}` | Per-server counters and metadata HASH | Keep `meta:registeredAt` as the activation/first-heartbeat grace timestamp. Initialization must verify the matching token claim. |
| `servers:retired:{<serverType>}` | `{serverType}` | Retired-server ZSET used by Phase C | Use the existing ZSET for both normal retirement and an orphan-registration cleanup tombstone. |
| `heartbeat:server:{<serverType>}:<serverId>` | `{serverType}` | Worker heartbeat STRING with TTL | No key-layout change. Orphan reconciliation must validate that no live heartbeat contradicts the orphan decision. |
| `servers:live:{<serverType>}` | `{serverType}` | First-heartbeat/live SET | No key-layout change. |
| `jobs:server:{<serverType>}:<serverId>` | `{serverType}` | Per-server job-reference SET | No key-layout change. Must be checked/cleaned by the existing Phase A cleanup path. |
| `jobs:type:{<serverType>}` and `job:{<serverType>}:...` | `{serverType}` | Type job index and job hashes | No key-layout change. Verify lifecycle scripts prevent jobs before ACTIVE. |

The new registry hash value should have an unambiguous encoding. Prefer two explicitly delimited components, for example `<token>|<registeredAtMs>`, with a token format that cannot contain the delimiter. Alternatively, use two new hashes if the implementation prefers not to parse a compound value. Do not store the timestamp in an application-server clock format.

The token must be unique per registration attempt, not merely per server ID. Generate it before the registry operation and pass the same value through every subsequent per-type operation. Redis `TIME` inside the registry Lua script supplies `registeredAtMs`.

## Registration workflow: exact steps and keys

### Step 1 — check for an existing per-type lifecycle record

**Slot:** `{serverType}`

**Key modified:** none; read only.

Read `servers:state:{<serverType>}` for the server ID. Reject registration if the server already has an `ACTIVE` or `DELETING` state, or if the registration-token marker indicates another attempt still owns the ID.

This remains a preliminary check only. It is not a cross-slot lock and cannot guarantee that a later registry operation will still be valid. The subsequent per-type claim is the authoritative check.

### Step 2 — publish registry membership, token, and timestamp atomically

**Slot:** `{registry}`

**Keys modified:**
- `servers:{registry}:types` — SADD the server type.
- `servers:{registry}:type:<serverType>` — SADD the server ID.
- `servers:{registry}:registration:<serverType>` — HSET the server ID field to the attempt token plus Redis-generated registration-start timestamp.

Extend `kRegisterServerScript` so all three keys are passed explicitly in `KEYS`. They share the literal `{registry}` hash tag. The script must validate all key types and reject an existing registry membership/registration record rather than silently replacing a different attempt's token.

The script obtains the timestamp using Redis `TIME` and writes the registry membership and registration record in the same Lua execution. Return the timestamp (or the stored registration record) to C++ if later code needs it; do not obtain a second application-clock timestamp.

**Crash outcome:** if the process stops immediately after this step, the cleanup scanner can still discover the ID and determine how old this specific registration attempt is.

### Step 3 — claim the attempt in the per-type slot

**Slot:** `{serverType}`

**Key modified:** `servers:state:{<serverType>}`.

Add a per-type Lua claim script. It must:
- Verify that the server ID has no `ACTIVE` or `DELETING` state.
- Verify that no conflicting `registrationToken:<serverId>` exists.
- Set `registrationToken:<serverId>` to the token published in Step 2.
- Perform the check and write atomically.

Do not set the server ID's lifecycle state to `ACTIVE` here. The normal state field remains absent until Step 5. The token field is a private in-progress claim used to fence delayed registration and to authorize metadata writes.

If cleanup has already installed a `DELETING` tombstone, or if a different token owns the claim, registration must fail. A C++ HGET followed by an unconditional HSET is not sufficient.

### Step 4 — initialize metadata, guarded by the claim

**Slot:** `{serverType}`.

**Keys modified:**
- `summary:type:{<serverType>}` — initialize shared type metadata as today.
- `summary:server:{<serverType>}:<serverId>` — initialize per-server metadata as today.
- `servers:state:{<serverType>}` — supplied as an additional Lua key for claim validation; no state transition is made.

Modify the type-summary and per-server-summary initialization scripts so each receives the state hash and checks that `registrationToken:<serverId>` still equals the attempt token before writing. All keys supplied to each script must share the `{serverType}` slot.

Keep type-level metadata shared: aborting one registration must not remove `summary:type:{<serverType>}` or metadata created by another server. The per-server summary may contain partial metadata if registration fails; Phase A must remove it after fencing the attempt.

### Step 5 — activate conditionally

**Slot:** `{serverType}`.

**Keys modified:**
- `servers:state:{<serverType>}` — set the server ID field to `ACTIVE`.
- `summary:server:{<serverType>}:<serverId>` — set `meta:registeredAt` using Redis `TIME`, preserving the current first-heartbeat timeout behavior.

Change `kActivateServerScript` to require the matching `registrationToken:<serverId>` and an absent lifecycle-state field. It must reject `DELETING`, any other state, or a mismatched/missing token. It must not treat an arbitrary existing `ACTIVE` state as success for a different token.

On success, write `ACTIVE` and the activation timestamp atomically. Preserve the token marker while the server is active so cleanup can compare the registry attempt token with the per-type incarnation. Remove the token marker only as part of final tombstone collection in Phase C.

**Crash outcome:** if activation succeeds but the process stops before doing any further work, the registry entry already exists and cleanup can discover the active server normally.

## Cleanup changes

The current cleanup phases are:

- **Phase A:** per-type fencing and cleanup, including `ACTIVE -> DELETING`, job/counter cleanup, heartbeat/live removal, and per-server summary removal.
- **Phase B:** registry-slot removal of the server ID after Phase A completes.
- **Phase C:** after retention and a separate registry absence check, collection of the per-type tombstone.

Keep these phase boundaries. Extend the existing scripts and make the new orphan path converge through the same tombstone and Phase B/C machinery.

### Phase A1 — read the registry registration record

**Slot:** `{registry}`.

**Keys:** read `servers:{registry}:type:<serverType>` membership and `servers:{registry}:registration:<serverType>` field for the server ID.

The scanner already obtains candidate IDs from the per-type registry SET. For each ID, it must also obtain the matching token and registration-start timestamp. A registry member without its registration record is a legacy/inconsistent record; handle it through an explicit compatibility policy rather than inventing a timestamp.

Pass the token and timestamp as arguments to the per-type fencing script. Do not pass registry keys to that script: they are in another slot.

### Phase A2 — extend the per-type fence script for an absent state

**Slot:** `{serverType}`.

**Keys:** retain the current fence keys:
- `heartbeat:server:{<serverType>}:<serverId>`
- `servers:state:{<serverType>}`
- `jobs:server:{<serverType>}:<serverId>`
- `summary:server:{<serverType>}:<serverId>`
- `servers:live:{<serverType>}`
- `servers:retired:{<serverType>}`

Add the registry token and registration timestamp as arguments. Extend `kFenceExpiredServerScript` with these cases:

| Per-type state | Token condition | Action |
|---|---|---|
| `ACTIVE` | Stored per-type token matches registry token | Existing heartbeat/first-heartbeat expiration logic; normal `ACTIVE -> DELETING`. |
| `ACTIVE` | Token mismatch | Do not fence based on this registry record; surface an invariant/identity mismatch. |
| `DELETING` | Matching token (or recognized legacy tombstone) | Resume existing Phase A cleanup. |
| State absent, token marker absent | Registry record is older than orphan grace | Atomically install the per-type token marker, set state to `DELETING`, and add the ID to `servers:retired:{<serverType>}` using Redis `TIME`. |
| State absent, token marker matches | Registry record is older than orphan grace | Atomically set `DELETING` and add the retired-index entry. |
| State absent, token marker differs | Any age | Do not overwrite; this is a stale registry record or newer attempt. |
| State absent, within grace | Any | Leave it alone. |
| Unexpected state | Any | Do not silently delete; report an invariant violation. |

The absent-state check, token comparison/claim, `DELETING` write, and retired-index insertion must occur in one Lua script in the type slot. This is the critical race fence. The grace period only determines when cleanup may attempt the operation; it is not itself the safety mechanism.

Before installing the tombstone, validate the relevant key types and ensure there is no live heartbeat. If a heartbeat exists, do not classify the candidate as an absent-state orphan. The script must not overwrite a different token marker or an existing active incarnation.

Use a distinct return code for “orphan fenced” if useful, but ensure the caller routes it into the existing Phase A cleanup script. The current return code 2 means Phase A is already complete; do not overload that meaning.

### Phase A3 — reuse cleanup for partial registration artifacts

After the orphan has been fenced as `DELETING`, invoke the existing `kCleanupExpiredServerScript` path. It already validates job ownership/indexes/counters before writes and removes per-server artifacts.

For a registration that never activated, normal job lifecycle scripts should have prevented jobs from being created because they require `ACTIVE`. Still, verify this in every lifecycle script. If no jobs exist, cleanup should accept absent job indexes and absent per-server summary/heartbeat/live keys as an already-clean state, while retaining the state tombstone and retired ZSET entry.

Do not remove shared type summary metadata. If unexpected jobs or inconsistent indexes are found, fail safely and leave the tombstone/registry entry for investigation and retry; do not discard the registry record.

### Phase B — conditionally remove registry membership and registration metadata

**Slot:** `{registry}`.

**Keys modified:**
- `servers:{registry}:type:<serverType>` — SREM the server ID.
- `servers:{registry}:registration:<serverType>` — HDEL the server ID's token/timestamp record.

Replace the unconditional C++ `SREM` with a small registry-slot Lua script. It must compare the currently stored token with the token that Phase A fenced. Only when they match should it remove the set member and registration hash field in the same script.

This token comparison prevents a delayed Phase B operation from removing a newer registration record for the same ID. If the token does not match, leave the registry data untouched and report/retry the reconciliation decision.

The global `servers:{registry}:types` set remains unchanged. It is a type-discovery set, not a per-server membership list.

### Phase C — retain the existing tombstone collection protocol

**Slot check:** `{registry}`, read only.

**Per-type keys modified:** `servers:state:{<serverType>}` and `servers:retired:{<serverType>}`; the existing script also checks heartbeat, server-job index, server summary, and live-set keys.

Keep the current order:
1. Discover expired IDs from `servers:retired:{<serverType>}`.
2. Check separately that the ID is absent from `servers:{registry}:type:<serverType>`.
3. Run `kCollectRetiredServerTombstoneScript` in the type slot.
4. Extend the collection script to remove both `<serverId>` and `registrationToken:<serverId>` from `servers:state:{<serverType>}`, and remove the retired ZSET member, after all existing retention and absence checks pass.

The registry registration hash field should already have been removed in Phase B. Phase C must not attempt to access it from the type-slot script.

## Does this introduce races or inconsistencies in the three-phase cleanup?

It does not require changing the three phase boundaries, but it introduces additional cross-slot metadata that must be reconciled carefully. The important cases are:

### Registration crashes after registry publication, before the type claim

The registry member and timestamp/token exist; the type state and token marker are absent. After the grace period, Phase A atomically installs the matching token marker and `DELETING` tombstone. The delayed registration's Step 3 claim then fails because the tombstone exists. Phase A cleans any partial artifacts (normally none), Phase B removes the registry member and matching registration record, and Phase C later collects the tombstone.

### Registration crashes after the type claim or metadata initialization

The registry record and per-type token marker match. The lifecycle state is still absent. After grace, Phase A changes the state to `DELETING` only if the token matches. Delayed metadata scripts and activation reject the now-`DELETING` state. Partial per-server metadata is removed by Phase A cleanup.

### Registration races with orphan fencing

Both the registration claim/activation and orphan fencing run in the same per-type slot. If activation wins first, cleanup sees `ACTIVE` and applies normal heartbeat rules. If orphan fencing wins first, it writes `DELETING`; claim/metadata/activation scripts reject the attempt. This is the desired serialization point.

### Phase A succeeds, then the process crashes before Phase B

The `DELETING` tombstone and retired ZSET member remain. The registry membership and registration record remain. A later scan recognizes the already-fenced state and retries cleanup, then Phase B removes the matching registry record. This is the same recoverability property as current cleanup.

### Phase B succeeds, then the process crashes before Phase C

The registry member and registration record are gone, while the tombstone and retired ZSET member remain. Phase C continues to discover the tombstone from the retired ZSET, verifies registry absence, waits for retention, and collects it. This is the existing design.

### Phase C races with a new registration

The current duplicate-ID rule is important: registration must reject an ID while its per-type `DELETING` tombstone exists. Thus a new attempt cannot publish a registry record while Phase C is about to collect that tombstone. If registration checks after Phase C has removed the tombstone, it may proceed as a new attempt. Preserve this ordering and do not weaken the state/tombstone check.

### Stale Phase B versus a newer registration

The new token-conditional Phase B script is required. An unconditional `SREM` is not sufficient once registration metadata can be replaced or retried. Phase B must remove only the registry record whose token Phase A fenced.

### Overlapping cleanup scans / expired cleanup lease

The cleanup lease remains best-effort coordination, not a lock. Two scans may process the same candidate. Phase A must remain idempotent and token-checked; Phase B must be token-conditional; Phase C must retain its existing state/retired/registry checks. These properties, not the lease, make overlap safe.

### Remaining cross-slot race: registry snapshot versus type-slot decision

The scanner reads token/timestamp in the registry slot and later acts in the type slot. A newer registry record could theoretically appear between those operations. The type-slot token marker and state/tombstone rules must reject mismatches; Phase B must compare the token again before removal. Never treat the timestamp alone as authority to delete.

## Risks and trade-offs

1. **More protocol state.** There is a new registry hash and a per-server token field in the type state hash. All registration, cleanup, diagnostics, and migration code must agree on the encoding and lifecycle of these fields.
2. **Registration scripts become conditional.** Type metadata initialization and activation must all validate the token. Missing one guard reopens the delayed-registration race.
3. **Cleanup script complexity increases.** The fence script now handles ACTIVE expiration, DELETING retries, and absent-state orphan fencing. Return codes and caller behavior must remain explicit.
4. **Legacy records need a migration policy.** Existing registry members have no token/timestamp. Do not infer an age from application time or silently remove them. Options include a one-time backfill timestamp, a conservative legacy cleanup policy, or requiring a controlled deployment/migration.
5. **Rolling upgrades need coordination.** Old workers do not write registration tokens and old cleanup managers do not understand the new registry hash. A mixed-version deployment can misclassify records. Define deployment ordering or add a compatibility window before enabling orphan reclamation.
6. **ID reuse remains dangerous.** Server IDs should be unique per process incarnation. Token checks reduce stale-attempt interference but do not make reuse of a still-running process's ID safe.
7. **Type-summary metadata is shared.** Orphan cleanup must never delete shared type metadata merely because one server registration failed.
8. **Registry hash growth/cleanup.** Phase B must remove each per-server registration field; monitoring should detect orphaned hash fields and registry set/hash mismatches.
9. **Timestamp/grace selection.** A too-short grace can abort slow but healthy startup. Use Redis time and choose a grace longer than normal registration latency plus operational pauses. The token fence protects correctness after the threshold, but a false-positive cleanup still causes an avoidable failed startup.
10. **Partial data invariants must be verified.** Confirm all job lifecycle scripts require ACTIVE and that no other writer creates server jobs, counters, heartbeat, or live membership before activation.

## Implementation and test checklist

### Source changes

- [ ] Add `servers:{registry}:registration:<serverType>` key helper.
- [ ] Extend `kRegisterServerScript` to atomically add registry membership and token/Redis timestamp.
- [ ] Add a per-type conditional registration-token claim script.
- [ ] Guard type-summary and per-server-summary initialization with the matching token.
- [ ] Change `kActivateServerScript` to require the matching token and absent state; preserve activation-time `meta:registeredAt`.
- [ ] Extend `kFenceExpiredServerScript` to fence eligible absent-state orphans using registry timestamp and token.
- [ ] Route orphan-fenced results through the existing Phase A cleanup path.
- [ ] Make Phase B remove registry membership and registration metadata only when the token matches.
- [ ] Extend Phase C to remove the per-type token marker when collecting the tombstone.
- [ ] Update README key layout, registration ordering, script KEYS/ARGV contracts, and cleanup description.
- [ ] Add logs/metrics for orphan candidates, token mismatches, legacy records, and retry outcomes.

### Tests

- [ ] Crash/failure after registry publication but before type claim.
- [ ] Crash after type claim, after type-summary initialization, and after server-summary initialization.
- [ ] Pause registration beyond grace; let cleanup fence; resume and verify claim/activation fail.
- [ ] Race activation against orphan fencing; exactly one per-type transition wins.
- [ ] Verify an active server with matching token follows existing heartbeat expiry behavior.
- [ ] Verify token mismatch never fences or removes a different registration attempt.
- [ ] Crash after Phase A before Phase B; verify retry removes only the matching registry record.
- [ ] Crash after Phase B before Phase C; verify retention and token-marker collection.
- [ ] Run overlapping cleanup scans with the lease expiring mid-scan.
- [ ] Verify shared type summary metadata survives orphan cleanup.
- [ ] Verify no job can be created before ACTIVE.
- [ ] Test wrong Redis key types and missing/legacy registration metadata.
- [ ] Run Lua tests on standalone Redis and cross-slot workflow tests on an actual Redis Cluster.

## Recommendation

Proceed with timestamp-plus-token reconciliation while preserving registry-first publication and the existing three cleanup phases. Use the registry timestamp only to decide when an orphan is eligible for reconciliation. Use the unique token and atomic per-type state transition to decide whether cleanup or registration wins. Use token-conditional Phase B removal so cleanup cannot delete a different registration attempt.

The design is safe only when the token is checked by every per-type registration write and by Phase A, Phase B, and Phase C as described. Implement it as one coordinated protocol change, with explicit legacy/rolling-upgrade handling; do not deploy only the new orphan cleanup script against old registration writers.
