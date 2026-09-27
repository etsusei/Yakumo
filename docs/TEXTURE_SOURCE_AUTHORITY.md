# Texture source authority contract

This is the implementation contract recovered by ASSET-012 for one supported
lobby owner and the DATA.BIN read route. It joins the
[owner evidence](TEXTURE_OWNER_VALIDATION.md), [slot lifecycle](TEXTURE_SLOT_LIFECYCLE.md),
[worker audit](TEXTURE_ASYNC_COMPLETION.md), and [transfer extent](TEXTURE_TRANSFER_EXTENT.md).
It does not install hooks or claim gameplay coverage. Other owners, overlays,
read routes and uncertified mutations retain the original builder.

## Identities and bounds

An owner record must originate from the identified allocation/factory chain,
not from scanning for a vptr. Capture the exact allocation base, requested
extent, allocator identity and a monotonic allocation generation. The selected
lobby constructor establishes vptr `0x0896FBC8` on the same `0x2F470`-byte
allocation. Bind the lobby overlay identity and code epoch; the quest overlay
has different code at that address. Construction produces empty slots.

For selector 7 the root slot is `[owner+0x27C70, owner+0x2D470)`, capacity
`0x5800`. Validate raw arithmetic and canonical physical intervals. A command
allocation is a separate lease from the command allocator, retaining its
successful requested byte count. Rounding slack in either heap is not extra
authorized capacity. Unknown, freed, reconstructed, reset or reused allocations
cannot inherit old source or command authority.

Each request needs an independent identity beyond its ring-descriptor address:
owner/allocation generation, source-slot generation, descriptor-slot generation,
resource ID, group, exact destination, fragment offset/count, total request
length, first/last flags, transfer mode and hash flag. Snapshot and recheck the
fields that influence execution; recycled descriptors cannot complete an older
request. Capture the selected caller/vtable/provider and bounded code identities.

## What can publish a source

For each supported DATA.BIN fragment, observe all of these actual original
operations in order, without replacing them:

1. A positive `sceIoRead` return equals the requested fragment byte count.
   Short reads, zero and errors are retry evidence, not transferred bytes.
2. The original copy finishes at `destination + fragment_offset`, either
   inline or through the selected copy-worker acknowledgement. A worker signal
   without its matched operation is insufficient.
3. If deobfuscation is enabled, the original descriptor-bound transform and
   its worker acknowledgement finish. It is an in-place, length-preserving
   operation, not a decompressor returning a new length. For positive `n`, its
   write footprint is `round_up_4(n)`; the meaningful transferred extent is
   still `n`. Check the rounded footprint against the live allocation and slot.
   Do not promote the extra padding bytes into completed resource bytes.
4. Observe the request's terminal route and descriptor identity. Retirement
   alone is insufficient because an abort route also retires requests.

The first/last flags and total count must describe one coherent request.
Accumulate nonoverlapping, gap-free meaningful fragments in original order,
with checked offset arithmetic: the first offset is zero, each later offset
equals the preceding logical end, and the final offset plus count equals the
total requested transfer length. Publish only after that exact total and its
final-fragment evidence are complete. Unsupported sizes, overlaps,
out-of-order events, missing phases, changed identities and observer loss revoke
eligibility. Allocation capacity is an outer bound, not evidence that all of it
was loaded. A resource-ID cache hit can reuse only an already certified source
of the same live generation; a cached numeric ID is not a completion receipt.

The read route is part of the receipt. The bounded gate covers state-8 DATA.BIN
reads. The alternate memory-stick-file route has different error and integrity
control flow and is not automatically admitted by this contract.

## Integrity and readiness are separate observations

The descriptor hash flag is conditional, even when deobfuscation is enabled.
When set, the transform worker can compute SHA-1. The state-8 DATA.BIN path
does not visit the digest-comparison branch at `0x08865CD0`; that comparison
belongs to the alternate file path. Record `not_checked`, `computed_only` or
`reference_compared` distinctly. Never turn a computed digest or worker return
into an observed original integrity pass. A future reference-comparison receipt
requires the actual comparison and matching result on a certified route.

The source authority above establishes current bytes and their bounds, not
asset semantics or visual correctness. Validate the indexed root and child-2
pair inside the completed logical extent, then validate the selected TMH
metadata traversal. The resulting pointer must equal the builder's source
argument. A malformed resource or any required byte outside that extent stays
on the original path. State 3, post-build state 4, queue idle, vptr equality and
the optional status pointer are not substitutes for these facts.

## Invalidation and late writers

Start/reload, reset, cancel, teardown, command free, owner free/reuse and code
epoch changes revoke affected authority. Group cancellation does not guarantee
that an active selected request stops writing: its optional cancellation/status
pointer is normally absent, and the original gate demonstrates late writes.

Retain in-flight destination hazards until a real terminal boundary or a
separately proven cancellation/quiescence event. A new allocation or source
generation overlapping an old pending writer cannot become eligible merely by
getting a new ID. **Every unrelated overlapping write invalidates current publications and
poisons in-progress candidates, including writes from stale or unknown
requests.** Track a destination write epoch through read/copy/transform and
publication; the candidate's matched phase writes are expected, but a foreign
write between those phases destroys its receipt chain. Only a fresh, fully
correlated request after invalidation can publish again. Silently discarding
stale events, or keeping a current candidate alive because it has not published
yet, is unsafe.

At builder entry, require live owner/source/command leases, all relevant code
identities, no pending overlapping writer, a stable execution context and
exclusive access through snapshot and commit. The adapter then applies its
existing physical-disjointness and CPU/memory preflight. A lease, epoch or
descriptor change at any point rejects before native mutation. Original
fallback remains available; the authority module itself performs no I/O,
thread scheduling, guest stores or fallback execution.

## Implementation order and remaining coverage

ASSET-013 implements the bounded authority state machine and offline event
replay first. Exercise interval overflow/aliases, missing and duplicate events,
partial reads, multiple fragments, descriptor reuse, late writes after reset
or allocation reuse, observation loss, bounded storage and retirement without
transfer. No production registration is implied by those unit tests.

Subsequent integration must prove that actual original call paths emit these
events, including intra-unit AOT transfers and all invalidations; a hook visible
only on outer dispatch is insufficient. Keep Baseline on original execution,
retain explicit off/verify/native modes and counters, and batch a later user
case with related validated changes. Multi-fragment production traffic,
alternate file integrity failures, real PSP scheduling/cache behavior and live
selector-7 reachability remain explicit coverage obligations.
