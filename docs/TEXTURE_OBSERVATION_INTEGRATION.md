# Texture observation seams for ASSET-014

This is an implementation map for the single lobby owner with vptr
`0x0896FBC8`, selector 7, and the asynchronous DATA.BIN state-8 route. It
connects the existing [source-authority API](SOURCE_AUTHORITY_MODULE.md) to
instruction checkpoints. It is not a claim that production lifecycle
observation or live loading is already installed. Every other owner, source
route, uncorrelated writer and no-command builder call stays on original AOT.
The machine-readable checkpoint inventory is the ignored local
`out/testing/texture-observation-seams.json`.

The supported main ELF SHA-256 is
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
The lobby overlay is raw entry 122, load base `0x0A05E600`, code size
`0x118B24`, header-plus-code FNV-1a `F6300296C8D954E5`. The fixed owner
allocation is `0x2F470` bytes. Selector 7 spans
`[owner+0x27C70, owner+0x2D470)`, exactly `0x5800` bytes. The source
documents already contain the full original spans and hashes:
[factory/constructor](TEXTURE_OWNER_OVERLAY_AUDIT.md),
[allocation](TEXTURE_ALLOCATION_CONTRACT.md),
[slot lifecycle](TEXTURE_SLOT_LIFECYCLE.md),
[worker completion](TEXTURE_ASYNC_COMPLETION.md), and
[builder](TEXTURE_COMMAND_BUILDER_CONTRACT.md). The labels below were checked
against the current ignored generated C++ as control-flow anchors; this note
does not publish game bytes or redefine those contracts.

## Required checkpoint chain

Each `L_...` is a guest instruction label in the named generated unit. A
callback placed immediately after a label runs **before** that instruction;
the named successor of a call or store runs after its original effect. The
callback must use the constant label as its PC: a same-unit `goto` need not
update `ctx.pc`. Parenthesized API names are `SourceAuthority` methods, not
claims that a register value by itself is a receipt.

| Checkpoint | Generated unit | Captured fact and authority action |
| --- | --- | --- |
| `0x088BD074` before `0x08879F08`; `0x088BD07C` on return | 0046 | Capture actual reverse-allocator manager, requested `0x2F470`, alignment 16 and nonzero returned `s2`. Pair by guest context and caller frame; do not use a later matching vptr as allocation evidence. |
| `0x088BD13C` before call to `0x0A0E7460`; `0x088BD144` after | 0046 | Require that `a0` is the same `s2`, the live overlay is entry 122, and the original constructor returned 1. After return check vptr `0x0896FBC8` and cleared selected slot before `construct_owner`. The constructor itself is in a prebuilt, read-only lobby library; these main-unit call edges observe it without recompiling that library. |
| `0x088A5470` entry; `0x088A54EC` tail transfer | 0040 | Validate the selected owner, selector 7, vtable/provider and mapped resource ID. The tail transfer supplies `(manager, ID, destination, group, 0, 1)` to virtual `+0x30`; bind the owner/load token to that call frame. `begin_load` revokes old source data, including a cached-ID reuse. |
| `0x08863CDC` enqueue entry; `0x08863DD8` after each descriptor commit; `0x08863E28` successful return | 0023 | Pair the manager call with the wrapper. At `DD8`, snapshot the complete 32-byte ring descriptor, including both flags after the optional hash choice, destination, offset, chunk count and original total. The original stores the producer index at manager `+0x1090` at `DD4` before the `DD8` callback. At `DD8`, `a2` (`gpr[6]`) points to the descriptor start; the descriptor is at `manager + 0x80 + ((old_index & 0x7F) * 32) + 12`. The complete G1 adapter must issue a fresh `begin_descriptor` and `begin_fragment` for **each** ring-slot reuse. The return value 1 means queued, not loaded. An unpaired/overlapping destination is an external pending writer or observation loss. |
| `0x088654D4` state-8 branch; `0x0886551C` after `sceIoRead` | 0024 | Preserve route identity and actual signed read result in `s0` against requested `record[+8]`. Reobserve the full descriptor before each attempt. Only positive equality receives `observe_read`; short/zero/error retries never advance the logical extent. |
| `0x0886577C` classifier result; `0x088652AC` copy-helper return; `0x088659B4` helper return; `0x088659C4` policy result | 0024 | For the outside-region inline route, require the same active descriptor and actual byte-copy return at `652AC`, then the helper and policy results leading to `659CC`. `659B4` alone is insufficient: the helper also has a no-copy exit at `0x0886528C`. Only the complete chain gives `observe_copy(Inline)`. |
| `0x08865368` worker copy return; `0x08865304` event-set return; `0x088657C0` main wait return | 0024 | These are the distinct copy-worker operation and acknowledgement candidates for the classifier's other route. Require one unchanged descriptor and prove its branch/event correlation in an original-code fixture before `observe_copy(Worker)`. Until then the route retains its pending writer and uses original builder. A worker signal alone cannot certify a copy. |
| `0x08865420` before in-place transform; `0x08865428` after; `0x08865440/0x08865448` around conditional SHA-1; `0x088653C4` worker event-set return; `0x08865A10` main wait return | 0024 | For `record[+0x1B]=1`, require actual descriptor-bound transform and matching worker acknowledgement, then `observe_transform` with footprint `round_up_4(chunk_bytes)`. `record[+0x1C]` alone decides whether the SHA-1 helper ran; state-8 evidence is `ComputedOnly` or `NotChecked`, never an invented reference comparison. If deobfuscation is off, record the verbatim branch and no transform receipt. |
| `0x088654F8` early group-4 abort; `0x08865814` before retirement call; `0x08865D8C` retirement entry; `0x0886581C` after return | 0024 | Mark abort independently of retirement. At `65814` retain the final full descriptor before `65D8C` clears its active marker; record postprocess. Only after the original retirement returns at `6581C`, with the complete same-token read/copy/transform chain and no intervening invalidation, call `observe_terminal(true)`. Other terminal routes call it with `false` and leave a pending hazard. |

## Current G1a implementation boundary

The build-local G1a adapter currently implements only the unit 0040 load/provider
edge and the unit 0023 enqueue/descriptor edge above. It resets the selected
owner's prior source at load start, copies all 32 bytes at `DD8`, starts a new
descriptor token and records a pending external writer for the checked
destination span. It does not call `begin_load` or `begin_fragment`: the source
route and actual transfer/completion facts are not observed yet. The manager
length lookup and descriptor's original total are observed through the real
compiled path, but that requested length is not a decoded-output size or slot
capacity check. Enqueue return 1 closes the frame and leaves every writer
pending. The controlled ring-reuse fixture resets the producer index to reuse
slot 0; it tests generation and hazard retention without claiming that a
natural 128-entry ring wrap was executed. The initial G1a-R0 reports are
historical; the current G1a-R1 source-bound reports and hashes are recorded in
`docs/tasks.json`. Both revisions compare full CPU/RAM/VRAM state and keep
`transfer_readiness=false`.

G1a-R1 confirms that `a2`/`gpr[6]` points to the 32-byte descriptor start at
the commit callback. Physical overlap with descriptor history takes precedence
over the destination filter: a reused descriptor address starts a new
`SourceAuthority::begin_descriptor` generation even when an unowned request's
valid destination is disjoint from every watched slot. Prior records become
non-current, but prior pending-writer tokens are never removed. Destination
validity is checked independently of overlap, so an invalid span cannot be
mistaken for proof of disjointness. Selected requests at exactly 0x5800 bytes
are observable; larger requests are rejected with their writer hazard retained.
The transformer can refresh a previously verified build-local output/manifest
pair for changed input, while tampered, unknown or incomplete pairs remain
protected. Its publish transaction now cleans up both temporaries on failure,
restores old files independently, and preserves any backup that could not be
restored. That is exception recovery for ordinary I/O errors; it does not
claim cross-process crash atomicity. These changes affect only the bounded
G1a build-local oracle; the transfer-boundary CMake option remains off by
default.

The initial proposed integration path is the outside-region inline copy. The
selected synthetic destination `0x09000000` takes that branch; a live owner
address is runtime-dependent, so its classifier result must be observed. The
other copy route already has 14 bounded original AOT/interpreter scenarios
in the ASSET-012 gate with modeled scheduling. Those tests do not establish
production callback coverage: the route receives no live permit until the
actual observation chain and operation/acknowledgement pairing are proved. Multiple queued
fragments are represented in the authority API; a one-fragment fixture does
not establish live split-request coverage.

## Current G1b-read implementation boundary

Iteration 6 adds a separate read checkpoint channel at state-8 entry
`0x088654D4`, helper entry `0x08863608`, the original `sceIoRead` call edge
`0x0886365C` and result edge `0x0886551C`. At each checkpoint the adapter
resamples the complete 32-byte descriptor and correlates the original
descriptor generation, selected request/load generation, owner allocation
token, owner-slot invalidation generation, runtime/context/execution identity,
worker stack, manager/record, fd, scratch and requested length. Each read has
its own bounded serial and signed result. Short, zero and negative results are
retry attempts; bytes are not accumulated across them. An exact positive result
only closes that attempt and is never submitted to `SourceAuthority` as a
completion receipt.

The separately compiled unit 0024 test copy stops after `ReadResult` and before
the branch that compares the returned result with `record[+8]`. The interpreter
stops at that same edge. Both sides compare full CPU, RAM and VRAM state, and
the test confirms the owner destination remains unchanged. The stop hook is
isolated from `mhp3rd_generated` and the Yakumo application. Iteration 6's
69-case normal/strict matrix is retained as historical evidence; independent
review found retry, historical-unowned-generation, Baseline-staging and
report-closure gaps. The iteration-7 R1 normal matrix pins 75 scenario
identities and 450 AOT/interpreter calls. It retains 12 representative
descriptor-field samples and covers retry/result classes, callback/register
faults, aliases, owner reset/free/reuse/reload invalidation, incomplete
results, capacity loss, historical unowned reads, partial-overlap rejection
and a late-result rejection after descriptor-generation change. Its normal and
strict source-bound reports pass and are recorded in `docs/tasks.json`. The first strict
single-process attempt reached the existing 300-second budget without a
sanitizer diagnostic. R1 now runs the same fixed matrix in three ordered
25-scenario shards and verifies their exact nonoverlapping ID union under one
unchanged source/binary identity; this changes scheduling only, not coverage.

After a short/zero/error result, a selected frame requires a fresh State8
callback; HelperEntry cannot clear the retry requirement itself. At State8,
the current descriptor generation is initialized independently of whether the
read is selected, while owner/request invalidation checks remain selected-only.
Two actual compiled selected-to-unowned enqueue cases reuse descriptor history
through the same raw address and through cached/uncached aliases; they retain
the old writer and issue no selected read attempt. A clearly labeled synthetic
partial-overlap fault exercises fail-closed storage classification. The
active-attempt same-byte generation case invokes the production tracker commit
callbacks with a controlled context; it is not evidence of concurrent
compiled producer scheduling. The source-bound runner hashes the complete
probe/lifetime/transfer/read output and manifest chain and checks the fixed
scenario IDs and counters.

The new `TextureReadInstrumentation.cmake` is in the observed Baseline staging
allowlist. A fresh staged B0 source tree configured with read, transfer and
probe boundaries off; this verified the unconditional include resolves
without building or launching the application.

The read import data and event scheduling are modeled. No real file identity,
open/seek behavior or concurrent PSP worker schedule is exercised, and a
change restored between observation points is not detectable. G1b-read stops
before target-byte copy, transform, worker acknowledgement, retirement,
cancellation or quiescence; all selected pending writers remain live and the
healthy authority result remains exactly `NotReady`. Transfer readiness and
Native mode stay disabled.

## Invalidation checkpoints

The selected reset `0x088A53C8` in unit 0040 revokes the source before it
clears `owner+0xB80/+0xB84`; its conditional group cancellation at
`0x088A53FC` does not prove the active worker stopped. Group cancellation
`0x08866044` and full-queue cancellation `0x08865F00` in unit 0024 mark
affected tokens canceled but retain destination hazards until a matched
successful terminal or separately proven quiescence. Ring descriptor reuse at
`0x08863DD8` revokes the previous descriptor generation even if its physical
address is unchanged.

For command release, unit 0039 `0x088A3474` identifies the selected
`owner+0x13F4` state; its direct free call at `0x088A3530` and pointer clear
at `0x088A3538` pair the lease release with the original allocator. Unit
0045 `0x088BAA10` identifies the owner on the container release path before
reset, command cleanup, destructor and the selected owner free at
`0x088BAAAC/0x088BAAB4`. Generic free `0x08879FF0` and heap reset/init
`0x08879D58/0x08879DA4` in unit 0029 must revoke matching live leases
regardless of which caller reached them. A later allocation at the same
physical address does not erase an old pending writer. Unknown overlapping
manager requests and actual writes must use the external-writer API or latch
`observer_lost`; silently ignoring them would make a stale permit possible.

Code identity needs its own monotonic host epoch even when no recording
session exists. `overlays.cpp` already compares header and code on install and
revalidation; `hle_system.cpp` calls `revalidate_overlays` and
`observe_overlay_code_epoch` on guest instruction-cache invalidation.
`GameObserver::code_epoch` increments only when an observer is active and is
therefore a journal field, not an authority clock. Notify the authority of
install, unload/replacement and every relevant instruction-cache invalidation,
including a same-byte reload; recheck the lobby header/code and main-ELF
builder/helper/table identities at builder entry. A changed epoch revokes
permits even if a vptr or descriptor remains in RAM.

## AOT insertion and bounded verification

`tools/instrument_probes.py` demonstrates the safe pattern: transform only a
build-local generated copy, require unique instruction labels and expected
block shapes, and emit a manifest. Its certified-leaf return-count rule does
not apply to the builder because its span contains call-continuation
`return;` statements. The specialized
`tools/instrument_texture_commands.py` inserts a callback at unit 0038
`L_0889E5C0`, including same-unit `goto` entries, and return callbacks at
`L_0889E650` (no command) and `L_0889E7C8` (positive) after their `jr ra`
delay slots. The no-command path remains original and ineligible for authority.
`TextureCommandInstrumentation.cmake` now compiles this build-local copy under
`MHP3RD_TEXTURE_COMMAND_BOUNDARIES` (default ON). `TextureCommandDispatch`
provides at most 16 runtime-specific callback bindings, serialized with their
registration and destruction. No application installs a binding yet, so the
ordinary game and Baseline keep original execution. Callbacks cannot dispatch
guest code, reenter the registry or throw. Owners must outlive all their guest
calls and be destroyed before their runtime while execution is quiescent.
The selected allocation/factory/caller/reset/free producer is now implemented
by [TextureLifetimeTracker](TEXTURE_LIFETIME_TRACKER.md) and exercised through
actual original factory/overlay/caller paths. Transfer/worker producers,
writer exclusion and the production mode controller remain pending.
The lifetime transformer now covers 14 shape-checked points in units 0029,
0040, 0043 and 0046. Generic free/reset observations also cover selected release
paths in units 0039/0045. Remaining transfer and worker checkpoints need units
0023/0024 and coherent code-epoch integration. An outer `Runtime::register_function` wrapper or pre/post
dispatch hook misses same-unit `goto` transfers and cannot claim complete
observation. Production callback signatures should take
`(Runtime&, const AllegrexContext&, checkpoint_id)` for observations and
use only bounded, read-only guest snapshots; the builder uses the separate
`texture_command_entry(Runtime&, AllegrexContext&) -> bool` and
`texture_command_return(Runtime&, AllegrexContext&, uint32_t jump_target)`
callbacks.

The compiled production unit passed 32 entry/return cases, including registered
and direct-chain calls, original nop fallthrough into the builder, both returns,
aliases, controlled same-unit returns, declined replacement and mismatched
prediction. A stopped return callback exits before local redispatch and retains
the actual return PC; it does not substitute zero for RA. Another 18 cases run
the real selected `0x088B0398` caller tail through its virtual provider, child-2
accessor, original heap allocator, builder and epilogue with observational,
Verify or prepared-commit callbacks. Full RAM, VRAM and CPU results match the
bounded interpreter. These tests supply constructed owners/resources and
explicit fixture bounds; they do not manufacture production authority.
In particular, builder entry sees the old command-state pointer before the
builder stores the new one. See [the admission contract](TEXTURE_COMMAND_ADMISSION.md).

Six asset-free registry cases passed CTest and memory sanitizers. Fourteen
Python tests cover injector shape/output safety and report validation. Eight
Baseline staging tests cover the shared seam files and matching recording
revision inputs. A fresh configuration also retains the dedicated oracle with
certified probes OFF and texture boundaries ON. A new B0 application has not
been built or delivered. The
unowned instrumented path also retained the complete 2,256-input/4,512-call
original corpus result. Source/binary-bound evidence is in ASSET-014's ledger.
That earlier checkpoint did not establish owner-constructor or transfer
callbacks. The subsequent lifetime gate adds the former; no game, app delivery,
transfer readiness or live native activation is established by either gate.

For `Verify`, prepare the existing nonmutating `TextureCommandPlan` at builder
entry, let original AOT run **once**, then compare at the matched return
before the generated local dispatch continues. `Native` commits only after
fresh authority and execution-context revalidation and resumes at the actual
return address; `Off` and all rejections continue original AOT. The builder
saves `r31` in its 80-byte stack frame, so replacing RA with zero for a
standalone oracle changes observable stack bytes and is invalid. The selected
caller sets RA to `0x088B0400` in unit 0043, but other callers may return to
unit 0038. A bounded standalone original wrapper must pass the real RA,
arm a one-call completion scope, and at the instrumented return **after**
the delay slot capture `jump_target`, set `ctx.pc` to that target and return
from the generated unit without entering the caller. Bound dispatches and
require exactly one matched entry/exit and unchanged guest-thread identity.
The no-command return callback can close an original observation, but cannot
make the current positive-only authority API permit that branch. Do not
compare a shadow original that consumes guest side effects twice.

## What the next gate must prove

Use the existing standalone original-code harnesses, not a new user route, to
run the full factory-to-overlay constructor edge, selected enqueue including
ring reuse, state-8 exact/short reads, inline copy versus the no-copy branch,
transform/hash acknowledgement, success versus abort retirement, reset and
late write, selected command/owner free, address reuse and a code-epoch
change. Compare AOT/interpreter CPU and touched RAM for bounded fixtures;
assert the event sequence and authority result alongside original effects.
Force a same-unit `goto` entry and an intra-unit return in the fixture so
the callback manifest is exercised, not merely outer dispatch. Capture
`runtime`, `ctx`, `capture_runtime_execution_context()` and epoch at each
checkpoint; reject a switched guest thread, missing callback, duplicate
phase, ring reuse or budget exhaustion. Baseline receives the same read-only
observations with Native mode off. Neither a clean recorder summary nor an
outer-dispatch-only count proves these internal edges ran.
