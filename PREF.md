# Architecture Assessment: 3-colocated vs Dual-role

## Overall assessment

**3-colocated is simpler and more conservative operationally. Dual-role provides stronger cleanup-manager coordination and better independence across server types, but it carries materially greater implementation and validation complexity.**

The choice depends on whether independent per-type cleanup progress is a real requirement. If one cleanup coordinator is sufficient, 3-colocated is the safer baseline. If different server types need independent cleanup leadership and concurrent management, dual-role is the more capable design.

## Complexity ratings

These are engineering estimates, not measured metrics.

| Design | Complexity | Assessment |
|---|---:|---|
| 3-colocated | **4/10** | Moderate; fewer coordination and concurrency mechanisms |
| Dual-role | **7/10** | High; per-type leases, generations, renewal, and concurrent authority bookkeeping |

## Side-by-side comparison

| Area | 3-colocated | Dual-role |
|---|---|---|
| Architecture | Simpler, centralized cleanup coordination | Independent manager per server type |
| Manager election | One global, best-effort lease | Per-type lease with generation fencing |
| Stale-manager protection | Primarily worker lifecycle fencing; global lease is best-effort coordination | Worker fencing plus manager-token fencing in cleanup mutations |
| Redis Cluster | Registry and type slots are separated | Same deliberate separation, with per-type authority and cleanup data colocated |
| Concurrency | Less manager concurrency | Different server types can be cleaned concurrently |
| Failure handling | Fewer moving parts | More lease, renewal, expiration, and thread interactions |
| Operational complexity | Lower | Higher |
| Testing burden | Moderate | High |

## Why dual-role is more complex

1. **Per-type leadership:** Each server type has an independent lease and generation. A process may hold several independent leadership roles.
2. **Concurrent loops:** Manager discovery/scanning and lease renewal run independently and must coordinate safely.
3. **Local authority synchronization:** Thread races must not allow stale operations to erase or release newer authority.
4. **More failure states:** Acquisition, renewal, expiration, release, Redis outages, ambiguous command outcomes, and shutdown all require careful handling.
5. **More demanding tests:** Correctness depends on timing and concurrency, not only ordinary cleanup scenarios.

The high-priority local-authority race fix in dual-role is necessary: without token-specific compare-and-erase/release behavior, a delayed operation associated with an older token could remove a newer token from the local authority map. That could leave a valid Redis lease unrenewed and delay failover. The fix addresses this defect, but it still needs deterministic concurrency tests.

## Which design should be chosen?

### Prefer 3-colocated when
- A single cleanup coordinator is sufficient.
- Simplicity and lower operational risk are priorities.
- Independent cleanup progress per server type is not a requirement.

### Choose dual-role when
- Different server types need independent cleanup progress.
- Cleanup work should proceed concurrently across types.
- Strong per-type manager fencing is worth the additional implementation and testing cost.

## Final verdict

**3-colocated is the safer baseline today.** It has fewer moving parts and is easier to reason about and maintain.

**Dual-role is a worthwhile architectural improvement only when per-type cleanup independence is a real requirement.** Its additional complexity is justified by that coordination model, not simply by being more sophisticated.

Neither design should be considered production-validated based on source review alone. Dual-role still needs compilation, deterministic concurrency tests, Redis integration testing, and Redis Cluster testing before replacing 3-colocated. The current assessment is architectural/source-review guidance, not a claim that either implementation has passed runtime validation.
