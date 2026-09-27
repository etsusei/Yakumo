# Texture command admission and return correlation (ASSET-014)

This is the admission contract for a future `TextureCommandRuntime` around the
selected original builder call. It combines the observed caller, the owned
`IndexedBundle`, `SourceAuthority`, and the prepared command bridge. It does
not claim that the production owner, descriptor, worker, allocator, or code
epoch events already populate `SourceAuthority`. Those producers and a compiled
callback path need their own bounded original AOT proof before Native mode can
be admitted. All unrelated builder calls and unsupported source routes execute
the original code.

The supported main ELF is SHA-256
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
The original instruction/format evidence is in
[the caller audit](TEXTURE_CALLER_SOURCE_AUDIT.md),
[builder contract](TEXTURE_COMMAND_BUILDER_CONTRACT.md),
[allocation contract](TEXTURE_ALLOCATION_CONTRACT.md), and
[observation checkpoint map](TEXTURE_OBSERVATION_INTEGRATION.md). The
generated C++ is ignored local evidence, not a file to publish or edit. All
byte spans below are half-open.

## The one admitted call frame

`0x088B0114` starts the selected enclosing caller. Its prologue saves `r30`
in its 128-byte frame, then copies entry `r4` into `r30`. The selected vtable's
`+0xB4` method is this caller. At `0x088B0398..0x088B03B0`, it calls virtual
slot `+0x78` with `(r4=r30, r5=7)`. For vptr `0x0896FBC8`, this slot is
`0x088B7DE0`, which returns `r30+0x27C70`. That root occupies the fixed
selector-7 slot `[owner+0x27C70, owner+0x2D470)`, capacity `0x5800`, within
the observed `0x2F470`-byte owner allocation. At `0x088B03B4`, the caller
passes that root and index 2 to `0x088661C8`; a null result branches away.
The successful result is saved in `r16`, and the accessor's selected layout
returns `root+load32(root+0x14)`. Its sibling length field is `root+0x18`,
which this caller does not read.

At `0x088B03C4`, the caller reads `N=load32(child+8)`. It computes `36*N`
with 32-bit shifts and addition (`32*N + 4*N`), loads the command allocator
manager from guest global `0x09FBE75C`, and calls `0x08879DB4` with alignment
16. The addition at `0x088B03DC` is in the call delay slot: sampling `r5`
immediately before that statement captures only `4*N`, not the requested
extent. Capture the request at allocator entry with return address
`0x088B03E4`, or after the delay slot, then pair the nonzero `r2` result at
`0x088B03E4` with the same manager, request and call frame. The original does
not check allocator failure before invoking the builder. Authorize only the
requested successful extent `36*N`; allocator rounding slack is not another
command slot.

At builder entry, after the `0x088B03F8` call and its delay slot have
executed, the observed register relation is:

| Register | Required relation |
| --- | --- |
| `r30` | The same constructed, live owner captured by the caller frame. |
| `r16`, `r6` | The same selected child-2 guest pointer. |
| `r4` | `r30+0x13F0`, the selected owner's command state. |
| `r5` | The just-returned nonzero command allocation pointer. |
| `r7`, `r8`, `r9` | All zero: first output slot 0, first source record 0, and header-count selection. |
| `r31` | `0x088B0400`, set by this caller's builder call. |
| `r29` | The caller's 128-byte-frame stack pointer; the builder will create its own 80-byte frame below it. |

`0x088B0400` is the caller continuation and immediately begins its epilogue.
The builder saves the actual `r31` in its own frame and returns to it. A return
address equal to `0x088B0400`, or a matching vptr found in RAM, does not alone
prove the selected caller, owner generation or allocation. Arm a one-call
ticket at the real call edge, with the runtime/context identities, caller
frame, owner and command tokens, thread token and code epoch. A callback placed
immediately after generated label `L_088B03F8` runs before that label's call
statement; it must expect the call to set `r31` and its delay slot to set
`r9=0`. Check their actual values at builder entry, or instrument an explicit
post-delay-slot callee boundary. Consume the ticket once at that entry; pair
its return only with that entry. A
missing, duplicated, nested incorrectly, or switched-thread phase is an
incomplete observation, never an eligible call.

The new command pointer is stored into `owner+0x13F4` by the builder at
`0x0889E600`, *after* entry. Admission checks that `r4` names the selected
state and snapshots its prior bytes; it must not demand that `+0x13F4`
already equals the new `r5`. After a successful original or Native return,
the expected state has `+0x13F4=entry_r5` and `+0x13F0=entry_r6`.
Overwriting a prior nonzero pointer does not prove the old command allocation
was freed; only the observed free/reset lifecycle can release that lease.

## Child extent and permit

The producer must first establish the exact, currently completed DATA.BIN
state-8 load for this owner generation: load token, resource ID, group,
destination, actual logical byte total, descriptor generations and complete
read/copy/optional transform/postprocess/terminal receipts. The active slot
extent is `[owner+0x27C70, owner+0x27C70+total)`, with
`0 < total <= 0x5800`. A queued return, cached ID, idle poller, hash-computed
flag, mapped RAM or readable root header does not prove this extent. Pending
writers, failed/canceled work, descriptor reuse, owner reset/free, heap reuse,
untracked writes and a code change revoke admission. The inline copy route
needs its actual copy result; the worker route needs an operation/acknowledgment
pair, as described in the checkpoint map. Do not invent production events from
the offline `SourceAuthority` tests.

Under the exclusive guest execution and writer guard, copy exactly that
completed extent into `SharedBytes::take` and parse `IndexedBundle` with a
finite entry budget. This is an owned format check, not another authority
source. Require at least three indexed entries, a present, nonempty child 2,
and its `[offset, offset+length)` inside the completed extent. The parser
checks the whole table and every present entry against its parent; the
builder bridge then checks the selected child's size-prefixed record walk.
The offset and length are relative to the root, not the ISO file or entire
owner allocation. Reject overflow when adding the root and child offsets.
Require `r6` to be the exact raw pointer returned by the original provider and
indexed accessor, `r30+0x27C70+child.offset`. Keep that raw pointer for
descriptor image/palette address tokens and CPU effects. Canonicalize cached
and uncached RAM aliases for containment and overlap checks; equal physical
bytes do not license silently substituting a different raw pointer. If a
recorded owner lease uses a different raw alias, reconcile it explicitly to
the same physical interval and still check the observed caller's raw pointer.

For this call, request
`SourceAuthority::permit(owner_token, command_token, child.offset,
child.length, 0, 36*N, current_code)`. Its returned source base/total must
match the observed live slot and completed transfer; its child subrange must
resolve to the selected guest pointer. Its command base and **requested**
extent must match the allocation result and `36*N`. Require `N` to be a
positive signed count, `N <= 4096` and within the configured smaller budget,
and require the `36*N` arithmetic, raw addresses and both physical intervals
to remain in bounded main RAM. Pass `TextureCommandBounds{child.length, N,
max_blocks}` to `prepare_texture_commands` only after those relationships
hold. The bridge independently checks builder/helper/table fingerprints,
metadata traversal, selected output slots, dependency mapping, alignment and
physical disjointness of source, state, stack, output and code dependencies.
It snapshots the source and affected regions; its byte recheck is not an
allocation lifetime proof or a defense against change-and-restore.

The selected caller passes `r9=0`. If `N` is zero or has its signed high bit
set, the builder's no-command branch still writes state and stack bytes and
returns, but `SourceAuthority` only grants positive source/output permits.
Always run original AOT for that branch, including the zero-request/null
allocation case. The bridge's offline support for no-command effects does
not broaden production authority. A positive count with failed allocation,
an absent/empty child, unsupported class/provider, alternate source route or
any uncertain lifetime likewise takes the original path.

## Code and execution identity

Check the supported ELF and the actual current lobby overlay (raw entry 122,
load base `0x0A05E600`, code size `0x118B24`, header-plus-code FNV-1a
`F6300296C8D954E5`) at the relevant install and builder edges. Check the
owner vptr `0x0896FBC8`, its `+0x78` provider `0x088B7DE0`, and its `+0xB4`
caller `0x088B0114`. The builder span, descriptor helper, exponent table,
factory/constructor, transfer workers, caller and provider must retain their
certified identities. `SourceCodeIdentity` carries those observed identities
and a monotonic host code epoch, but setting its numeric fields is not a byte
check. The bridge already hashes the builder/helper/table per preparation;
the integration must supply the remaining overlay/caller/provider checks.
Notify the authority on install, unload/replacement and relevant instruction
cache invalidation, including same-byte reload. The recorder's session-scoped
code epoch is not a substitute. After `code_changed`, the current configured
identity is no longer supported; resume only from a newly verified code
identity and recaptured leases, with old writers proven quiescent.

Capture `(Runtime*, AllegrexContext*, thread_uid, switch_generation)` at the
caller ticket and builder entry and require the same pair at matched return.
Use `capture_runtime_execution_context()` and
`runtime_execution_context_matches()` so switch-away/switch-back also fails.
Serialize all calls on a `SourceAuthority` instance. Hold the same guard over
guest source, output, state, stack, code and any worker/allocator mutation
from permit/prepare through Native commit. A permit is an authority snapshot;
it is not a lock over guest writers. A writer with a known footprint must be
tracked until quiescent; missing or unbounded observation latches loss and
retains original execution.

`SourceAuthority::permit` increments its revision and saves exactly one
permit. Any later authority event that calls `touch()` or another successful
`permit()` invalidates that snapshot, even for an unrelated owner.
`revalidate()` checks exact saved fields, revision, tokens, code and pending
hazards, but does not resample guest bytes or secure the thread. Call it as
the last authority query immediately before Native commit, with no
intervening authority mutation and under the guard. Never carry an entry
permit through natural original Verify execution and assume it remains valid:
original write/lifecycle callbacks may advance the revision, and the builder
changes command/state bytes. Verify compares the entry prediction with that
one original execution; it never commits afterward. If identity or thread
correlation is lost during Verify, record the attempt as incomplete rather
than verified, even if a byte comparison happens to match.

## Proposed controller boundary

The controller should own per-runtime authority state and a bounded stack of
one-call tickets, not rely on a global `r31` search. One possible API is:

```cpp
class TextureCommandRuntime {
public:
    void observe(Checkpoint id, Runtime &, const AllegrexContext &);
    void caller_call(Runtime &, const AllegrexContext &, CallerCallEvidence);
    bool builder_entry(Runtime &, AllegrexContext &); // true only after Native commit
    void builder_return(Runtime &, AllegrexContext &, std::uint32_t jump_target);
    void invalidate_code(const SourceCodeIdentity &);
};
```

`CallerCallEvidence` contains the sampled allocator request/result and tokens,
owner/child/command pointers, caller frame and exact `0x088B0400` return
address. The callback location supplies the guest PC; an optimized generated
unit's `ctx.pc` can remain at an earlier dispatch PC through a successful
direct chain or same-unit `goto`. At builder entry, make a local copy of the
live CPU context and set **only the copy's** `pc=0x0889E5C0` before preparing
the plan. On Native admission, revalidate authority, then commit to the same
guest-memory instance and that projected local CPU copy while the guard is
held. If commit rejects, the live CPU and memory remain unchanged and the
original AOT runs. On successful commit, assign the completed local context
to the live context and let the generated callback resume at its `pc`, the
real `r31` target. Do not use a fake zero return address: the builder writes
`r31` into its 80-byte frame. A throwing guest-memory diagnostic during
commit can leave partial stores; it is a stop/error path, never a fallback to
run the original a second time.

For Verify, prepare the read-only plan from that projected entry copy, then
let natural original AOT run exactly once. At either original `jr ra` return
callback (`0x0889E650` for no-command or `0x0889E7C8` for positive count),
match the ticket, actual branch, `jump_target`, runtime/context and thread.
The callback runs after the delay slot and before generated local dispatch.
Project a **copy** of the returned CPU with `pc=jump_target` for
`compare_texture_commands`; do not mutate the live CPU merely to compare.
The positive selected call must return to `0x088B0400`. The no-command
return closes an original observation only; it cannot count as an eligible
verified or Native authority call. If the original returns inside unit 0038,
the generated local dispatch may continue without updating `ctx.pc`; use the
captured jump target rather than a guessed post-dispatch PC. Bound original
dispatch and require exactly one matched entry and exit in standalone proofs.

Off and every refused entry continue original AOT unchanged. A controller
may record attempted, eligible, Verified, Native, fallback, mismatch and
incomplete counts scoped to this caller/vtable, but none is whole-game
coverage. Baseline stays original. Existing delivered applications and user
records remain untouched.

## Required bounded tests before admission

1. Compile the build-local instrumented unit and prove one callback at the
   selected call edge, one at builder entry (including same-unit `goto`), and
   one at the actual return before caller epilogue. Compare full CPU and
   touched RAM/VRAM for the production AOT and bounded interpreter. Include
   direct chaining with stale live `ctx.pc`, same-unit return, and a switched
   thread token.
2. Drive actual factory/constructor, selector-7 provider, child-2 accessor,
   successful and failed command allocation, free/reuse and reset edges.
   Reject another vtable with the same caller address, a forged `r31`, a
   reused address, a mismatched manager and an old command pointer.
3. Populate authority from real descriptor/read/copy/transform/terminal
   callbacks. Include ring-slot reuse, short/zero/error reads, inline copy
   versus no-copy exit, worker operation and acknowledgement, queued
   fragments, cancellation, late write and code epoch change. A fixture that
   omits an event must refuse a permit or latch observation loss.
4. Exercise owned bundle parsing on exactly the observed completed extent:
   absent/empty child 2, malformed table, offset overflow, out-of-slot child,
   raw alias mismatch and physically overlapping aliases all fall back
   without guest mutation. Compare the accepted child's raw pointer and
   length with the original accessor and bridge input.
5. For Off, Verify and Native, assert exact one-call correlation and result
   accounting. Verify runs original once and compares without a shadow run;
   an authority revision change during it cannot be reused for commit.
   Native rejection preserves full guest state; success projects return PC
   and all certified state/stack/command effects. No-command and every
   unsupported path execute original. Test diagnostic exceptions as stop
   rather than original fallback.

The prepared-plan corpus and sanitizer gates already cover the bridge itself;
they do not establish these production callbacks, source producers or live
gameplay. Keep the first integration gate offline and bounded, then prepare a
related user-led paired case only after the production path is proven.
