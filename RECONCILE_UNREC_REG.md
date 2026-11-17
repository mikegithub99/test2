# Reconcile Unregistered Servers

## Purpose

Document a recovery design for partial server registration across the Redis Cluster registry slot and a server type's data slot.

The current registration workflow spans multiple Redis Cluster slots, so it cannot be made atomic with one Lua script or one transaction. A failure partway through registration can leave a server ID in the global registry even though the server type's state hash has no corresponding entry. The cleanup scanner discovers the ID through the registry, but the current cleanup fence only accepts `ACTIVE` servers. If the state lookup returns nil, cleanup does nothing and the registry member can remain indefinitely.

This document proposes making incomplete registration an explicit lifecycle state and making cleanup able to fence an absent or abandoned registration safely.

This is a design proposal, not an implementation specification verified against every current script. Before coding, inspect the current registration and cleanup scripts and reconcile the exact key names, arguments, and write ordering with this design.

## Current failure mode

The registration workflow currently performs operations across two slots:

1. Check the server's per-type state and any existing tombstone.
2. Add the server ID to the registry slot.
3. Initialize type summary metadata in the server type slot.
4. Initialize per-server summary metadata in the server type slot.
5. Activate the server by writing `ACTIVE` into the per-type state hash.

If steps 1–4 succeed but step 5 fails, the registry contains the server ID but the per-type state hash has no corresponding state entry.

The cleanup scanner enumerates server IDs from the registry and invokes the per-type fencing script. Its current effective condition is:

```lua
local current = redis.call('HGET', state, ARGV[1])
if current ~= 'ACTIVE' then
    return 0
end
```

For a missing field, `HGET` returns nil. The script therefore returns without fencing the server. The registry entry is not reconciled and can be rediscovered on every scan.

Simply removing the registry member whenever the state is missing is unsafe: a delayed registration attempt could subsequently create or activate the server, leaving an active server that is no longer discoverable through the registry.

## Design goals

- Make an in-progress registration visible in the per-type slot.
- Make activation conditional on the same registration attempt still owning that state.
- Let cleanup fence abandoned registrations and registry entries whose per-type state is absent.
- Ensure a delayed registration cannot reactivate a server after cleanup has fenced it.
- Keep all correctness-critical state transitions within the per-type slot, where Lua can update them atomically.
- Keep registry reconciliation idempotent; registry operations remain separate-slot operations.
- Preserve the existing cleanup phases and tombstone-retention model where practical.

## Proposed lifecycle states

Add an explicit `REGISTERING` state. Treat `ABORTED` as a tombstone/fenced outcome for a registration that did not become active.

Conceptual state transitions:

```text
             registration claims state
    ABSENT ------------------------------> REGISTERING(token)
      |                                         |
      | cleanup fences orphan                   | successful initialization
      v                                         | and conditional activation
  ABORTED(token)                                v
      ^                                      ACTIVE
      |
      | cleanup fences abandoned registration
      |
  REGISTERING(token)

  ACTIVE -> DELETING -> tombstone/retention -> eventual collection
```

The token identifies one registration attempt, not merely one server ID. It must be unique across retries and server incarnations. A delayed attempt with an old token must never be able to activate a later attempt.

The implementation may represent state and token in separate fields of the existing per-type state hash, or use an equivalent representation. The essential property is that checking the state/token and changing the state are performed atomically in the per-type slot.

## Proposed registration protocol

The following ordering is intended to preserve discoverability through the existing registry while ensuring that the per-type slot is the authority for fencing.

1. **Check existing state/tombstone.** Preserve the current protection against duplicate registration and against reusing a server ID that is still tombstoned.
2. **Publish a registry registration record.** Add the server ID to the registry and record enough information to identify this registration attempt (at minimum its token and a registration-start time). This is a cross-slot operation and is not itself the correctness fence.
3. **Claim per-type registration state.** In one per-type Lua script, create `REGISTERING` with the same token only if no state/tombstone already exists. This must be conditional (for example, an atomic absent-state check followed by writes); it must not overwrite `ABORTED`, `DELETING`, or another attempt's state.
4. **Initialize metadata.** Initialize type and per-server summary metadata using scripts that verify the expected `REGISTERING` state and token. Make these operations idempotent for the same token.
5. **Activate conditionally.** In one per-type Lua script, transition `REGISTERING(token)` to `ACTIVE` only if the stored token still equals the registration attempt's token. If cleanup has changed the state to `ABORTED`, activation must fail.
6. **Finalize registry bookkeeping.** Optionally clear or mark the registry-side pending-registration metadata after successful activation. This is housekeeping; it must not be required for correctness because a crash can occur after activation but before this step.

### Important ordering constraint

The registry record is published before the per-type `REGISTERING` claim so that the existing registry-driven cleanup scanner can discover the attempt. This creates a short interval where the registry entry exists but the per-type state is absent. Cleanup must therefore handle missing state, but only after an eligibility/grace check and by atomically installing a per-type tombstone that fences delayed registration.

The per-type claim must be conditional on the state still being absent. If cleanup installs `ABORTED` first, the delayed registration claim must fail. If registration claims `REGISTERING` first, cleanup must observe that state and apply the abandoned-registration policy instead.

If the current registration implementation cannot safely make the per-type claim conditional, change that before adding orphan cleanup. A check in C++ followed by an unconditional write is not sufficient.

## Cleanup and reconciliation behavior

Extend the existing cleanup scan/fence path to distinguish these cases:

| Per-type state | Proposed behavior |
|---|---|
| `ACTIVE` | Apply the existing expiration check and normal `ACTIVE -> DELETING` fence. |
| `REGISTERING` with matching token | If the registration is still within its allowed window, leave it alone. If it is abandoned/expired, atomically change it to `ABORTED`. |
| State absent | If the registry registration record is old enough to be considered orphaned, atomically create an `ABORTED` tombstone only if the state is still absent. |
| `ABORTED` | Resume/idempotently complete partial-registration cleanup, then remove the registry record. |
| `DELETING` | Resume the existing cleanup workflow. |
| Other/unexpected state | Do not silently delete data. Report or surface an invariant violation for investigation. |

The grace period is an eligibility threshold, not the safety mechanism. A process can pause longer than the grace period and later resume. Safety comes from the atomic per-type state transition: once cleanup installs `ABORTED`, the delayed registration attempt cannot claim or activate that server ID.

### Atomic orphan fence

For the missing-state case, the per-type Lua operation should conceptually:

1. Read the current state and registration token.
2. If the state is still absent, write an `ABORTED` tombstone associated with the observed registration token.
3. If the state is now `REGISTERING`, apply the registering-state rules instead.
4. If the state is `ACTIVE`, `DELETING`, or another tombstone, return the corresponding outcome without overwriting it.

The check and tombstone creation must happen in the same Lua script and the same per-type slot. Do not implement this as a C++ read followed by a separate write.

For `REGISTERING`, the cleanup script must compare the stored token with the token it intends to fence. This prevents cleanup for an old registry record from aborting a newer registration attempt.

### Partial metadata cleanup

After the registration has been fenced as `ABORTED`, cleanup should remove or reconcile metadata that may have been created before activation, including per-server summary metadata and any other per-server registration artifacts.

Do not assume that no jobs exist merely because activation did not complete. The lifecycle scripts are expected to require `ACTIVE`, which should prevent normal job creation while `REGISTERING`; nevertheless, verify this invariant in the actual scripts. If jobs or indexes can exist for a non-active server, the abort cleanup must account for them using the same consistency rules as normal cleanup.

Type-level summary metadata may be shared by other servers of the same type. Do not delete shared type metadata just because one registration was aborted. Reconcile or remove it only when its own lifecycle rules prove it is safe.

Once per-type cleanup is complete, remove the server ID and its pending-registration metadata from the registry slot. Because this is a separate slot, it is a separate operation and must be retryable. If the process crashes after per-type cleanup but before registry removal, the next scan should see the tombstone and safely repeat the cleanup/removal steps.

## Race analysis

Correctness depends on the per-type state being the authority for registration and cleanup decisions.

### Registration claims first

1. Registration publishes its registry record.
2. Registration atomically creates `REGISTERING(token)`.
3. Cleanup examines the per-type state.
4. Cleanup either leaves a still-valid registration alone or fences it according to the abandonment policy.
5. Activation succeeds only if the state is still `REGISTERING` with the same token.

If cleanup fences it first, activation fails.

### Cleanup fences missing state first

1. Registration publishes its registry record.
2. Registration pauses before claiming per-type state.
3. The registration record becomes eligible for orphan reconciliation.
4. Cleanup atomically creates `ABORTED(token)` only if the per-type state is still absent.
5. Registration resumes and attempts to claim `REGISTERING(token)`.
6. The conditional claim fails because the tombstone exists.

### Cleanup races with activation

Both operations execute in the same per-type slot. If activation changes `REGISTERING(token)` to `ACTIVE` first, cleanup observes `ACTIVE` and follows normal active-server cleanup rules. If cleanup changes it to `ABORTED` first, activation's state/token comparison fails.

### Old cleanup record versus a new registration

A registry record for an old attempt must not be allowed to abort a newer attempt. Use a unique per-attempt token and compare it in the per-type scripts. Do not rely on server ID alone if IDs can be reused or registration can be retried.

## Redis Cluster and atomicity boundaries

- Keep registration-state transitions, token comparisons, activation, and orphan fencing in per-type Lua scripts using keys in the same `{serverType}` hash slot.
- Registry membership and registry-side pending metadata remain in the `{registry}` slot.
- Do not attempt to pass registry keys into per-type Lua scripts; that would violate the Cluster slot boundary.
- Cross-slot operations remain a recoverable workflow, not a globally atomic transaction.
- Make each phase idempotent so retries can converge after a crash.
- The manager lease may reduce duplicate scan work, but it is not a correctness lock or fencing token.

## Implementation areas to inspect and change

Before implementation, inspect the current source and map the design to the actual functions/scripts:

- `registerServer()` and its C++ operation ordering.
- The registration Lua scripts (including the existing initial state/tombstone check and activation write).
- Type-summary and per-server-summary initialization scripts.
- The cleanup scan that enumerates registry members.
- `kFenceExpiredServerScript` and the subsequent cleanup phases.
- Registry removal and tombstone-retention scripts.
- Every job lifecycle script to confirm that job creation and transitions require `ACTIVE`.

Likely changes will include:

1. Add `REGISTERING` and `ABORTED` handling to the server lifecycle state model.
2. Add a unique registration-attempt token and registration timestamp to registry/per-type metadata as appropriate.
3. Replace any unconditional activation/state creation with conditional Lua transitions that compare state and token.
4. Make metadata initialization conditional on the expected `REGISTERING(token)` state.
5. Extend the cleanup fence script to handle expired `REGISTERING` and eligible missing-state orphans.
6. Add an idempotent partial-registration cleanup path for `ABORTED`.
7. Ensure registry removal occurs only after the per-type abort/cleanup decision is durable.
8. Update documentation and operational diagnostics for registration attempts that are pending, aborted, or inconsistent.

The exact storage representation and script signatures should be chosen after reviewing the current code. Avoid adding redundant registry metadata if an existing field can safely carry the token and timestamp.

## Test plan

Use a standalone Redis instance for Lua behavior and an actual Redis Cluster for cross-slot workflow and recovery tests.

### Registration failure injection

- Fail after registry insertion but before the per-type `REGISTERING` claim.
- Fail after the `REGISTERING` claim.
- Fail after type summary initialization.
- Fail after per-server summary initialization.
- Fail after activation but before registry pending metadata is finalized.

For each point, run reconciliation repeatedly and verify that the workflow converges without deleting a successfully active server or leaving a permanent registry orphan.

### Race tests

- Pause registration after registry insertion; let cleanup install `ABORTED`; resume registration and verify its claim/activation fails.
- Pause registration after creating `REGISTERING`; let cleanup fence it; resume and verify activation fails.
- Race activation against cleanup and verify exactly one terminal outcome wins.
- Run two registration attempts with different tokens for the same server ID and verify an old attempt cannot activate or abort the newer attempt.
- Verify an old registry record cannot cause cleanup to overwrite `ACTIVE` or a newer `REGISTERING` state.

### Cleanup idempotency tests

- Re-run cleanup after each individual phase.
- Simulate a crash after per-type abort cleanup but before registry removal.
- Simulate a crash after registry removal.
- Verify tombstone retention and eventual collection still work.
- Verify shared type summary metadata is preserved while other servers of that type remain.
- Verify unexpected Redis key types and inconsistent ownership are surfaced rather than silently discarded.

### Regression tests

- Normal registration reaches `ACTIVE`.
- Duplicate registration and tombstone/re-registration rules remain correct.
- Job lifecycle scripts continue to reject non-`ACTIVE` servers.
- Normal `ACTIVE -> DELETING` cleanup behavior is unchanged.
- Registry and per-type keys continue to use their intended Cluster hash slots.

## Operational considerations

- Choose the registration grace period based on observed registration duration and failure/retry behavior. It should avoid aborting healthy slow registrations, but it is not a substitute for token-checked fencing.
- Expose counts/ages for registry entries in pending registration, per-type `REGISTERING`, orphaned missing-state records, and `ABORTED` cleanup retries.
- Log the server ID and registration token (or a safe abbreviated form) when a registration is fenced.
- Alert on records that remain pending or aborted beyond the expected reconciliation window.
- Keep reconciliation retryable; transient Redis errors should not cause registry membership to be discarded prematurely.

## Summary

The key change is to stop treating a missing per-type state entry as an unresolvable no-op. Make registration explicit with `REGISTERING(token)`, make activation conditional on that exact token, and let cleanup atomically install an `ABORTED` tombstone when an eligible registration has no state or has been abandoned. Then clean partial per-server metadata and remove the registry record as a retryable cross-slot phase.

The grace period decides when cleanup may try; the per-type atomic state/token transition decides which operation wins. This prevents a delayed registration from resurrecting an orphan after cleanup has reconciled it.
