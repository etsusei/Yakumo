# Bounded source-authority module (ASSET-013)

This implementation follows [the recovered contract](TEXTURE_SOURCE_AUTHORITY.md).
The module consumes values describing observed allocation, request, transfer
and invalidation events. It does not read guest memory, run original code,
perform I/O, schedule workers or install a production hook. A successful
metadata permit is only one prerequisite for the existing adapter; actual
code identity, complete observation and exclusion of concurrent mutation remain
integration obligations.

## Offline acceptance matrix

Tests must establish behavior at the contract boundary rather than mirror
internal data structures. The following cases are required before completion:

| Group | Evidence required |
| --- | --- |
| Valid flow | A live constructed owner, separate command allocation, complete ordered read/copy/transform/postprocess receipts and exact final extent yield a current permit. |
| Partial and failed work | Short/zero/error reads cannot advance transfer; retirement alone or a missing phase cannot publish. |
| Fragment consistency | Initial offset is zero, each following fragment starts at the preceding end, and final end equals the declared total. Reject gaps, overlap, premature final flags and changing identities. |
| Extents | Distinguish transferred bytes from rounded transform footprints; reject wrap, unbounded counts, unavailable RAM, slot overrun and command slack. |
| Generations | Reset, cancel, reconstruction, free/reuse, descriptor reuse, code changes and replacement authority instances cannot reuse old receipts or permits. |
| Pending writers | An old pending writer survives cancellation and owner release. A newer overlapping allocation/source remains ineligible until quiescence. |
| Foreign writes | Overlapping writes invalidate published sources and poison pending candidates even between copy and transform. Stale or unknown writers receive the same treatment. |
| Aliases and isolation | Cached/uncached addresses resolving to the same bytes cannot evade conflict checks; unrelated owners and nonoverlapping ranges retain independent state. |
| Integrity | DATA.BIN digest generation remains computed-only; no reference-comparison pass is invented. |
| Budgets and loss | Storage exhaustion, counter exhaustion and observation loss fail closed without silently discarding hazards. |

## Implemented API and storage

`resources/source_authority.hpp/.cpp` implements this state machine using
standard-library values and preallocated, bounded tables. Configure the
supported code identity, allocator identities, RAM size and storage/counter
ceilings before use. Owner and command allocations are distinct leases; they
may belong to the same allocator when their physical ranges do not overlap.
The caller attests code/factory provenance; numeric identity fields do not
independently verify executable bytes.

All calls on one instance must be serialized, including queries for unrelated
owners. The process-local atomic instance-ID counter does not make the state
tables thread-safe. The module supplies no synchronization primitive.

`construct_owner` binds the selected owner and its fixed slot. `begin_load`
revokes previous source data and creates a new load identity. Every reuse of a
32-byte descriptor gets a fresh `begin_descriptor` token; partial overlap and
cached aliases retire old generations. `begin_fragment` can queue multiple
ordered fragments before processing. Its token remains tracked even when a
known receipt becomes ineligible, so pending writers are not forgotten.

Observe a fresh full descriptor before every read attempt and before successful
terminal completion. The adapter should also recheck it at intermediate phases.
Record read, copy, transform when enabled, postprocessing and terminal facts in
order. The read cannot precede completion of the prior fragment; transform and
hash flags must remain coherent across the load. Short reads consume their
snapshot and require a fresh observation before retry.

Cancellation and failed/incomplete retirement leave a writer pending. Only a
complete eligible terminal receipt or the separately attested
`prove_request_quiescent` removes that hazard. External writers use explicit
begin/write/quiescent events. `observe_unknown_write` means an already completed
one-off write, not an untracked continuing writer. An unknown writer-token
callback, unsupported/unaccounted load start, lost observation or exhausted
budget latches failure. Known unsupported routes with a proven footprint must
be tracked as external writers rather than silently skipped.

Foreign writes poison both ready sources and pending candidates. A write to
an owner's vptr revokes its construction binding; descriptor writes revoke
bound receipts. Changed actual destinations are retained as hazards, or latch
failure if they exceed bounded tracking. Free/reuse of a physical owner address
does not erase an old request's pending destination.

`permit` checks a complete source, live command capacity, code identity, source
and output subranges, and pending writers on source, output and the owner vptr.
It stores one exact snapshot internally. `revalidate` rejects changed fields,
stale generations and revisions, and permits from another instance. A newer
permit request supersedes the previous permit, even for an unrelated owner;
this API is intended for one immediate checked operation at a time. The caller
must keep guest execution and writers excluded through revalidation and commit.
The module does not supply that lock or validate the indexed/TMH structures.

## Validation and limits

The independent suite passes **50 cases, 101 checks**, under CTest and under
AddressSanitizer/UndefinedBehaviorSanitizer on Apple Silicon macOS. Strict
C++20 warnings include conversion and signed-conversion checks. Read-only review
led to regressions for shifted descriptor overlap, unsupported copy modes,
retry snapshots, unknown writer callbacks and failed terminal hazards. Root
also verified bounded RAM geometry, configuration limits, queued fragments,
command hazards, stale construction and nonmonotonic code changes.

```sh
cmake --build out/mhp3rd --target mhp3rd_source_authority_tests -j2
ctest --test-dir out/mhp3rd -R '^mhp3rd_source_authority_tests$' --output-on-failure
```

The module has no dynamic table growth after construction. Invalid configuration
is clamped to hard storage ceilings before allocation and then rejected; even
invalid settings may allocate bounded tables. Constructor allocation failure
may throw before a usable authority exists. Process-local instance IDs and
per-instance revisions never wrap into reusable tokens.

This version supports positive source/output permits only; the original
adapter's no-command branch is not authorized by it yet. The supported code
identity includes a fixed epoch. Code changes invalidate authority; repeated
or regressive epochs are lost evidence. Recovery after loss or a new supported
epoch requires externally proven quiescence, a new authority instance and
recaptured lifecycle state. Merely constructing a new instance cannot dismiss
writers left by the old one.

These tests validate metadata behavior, not actual observation completeness,
resource parsing, guest scheduling, real races or gameplay. ASSET-014 must
connect factual original lifecycle/transfer observations and guarded dispatch,
then validate the production paths offline before a related user test batch.
The delivered applications and existing user records are unchanged.
