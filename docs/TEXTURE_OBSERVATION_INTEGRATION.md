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
| `0x08863CDC` enqueue entry; `0x08863DD8` after each descriptor commit; `0x08863E28` successful return | 0023 | Pair the manager call with the wrapper. At `DD8`, snapshot the complete 32-byte ring descriptor, including both flags after the optional hash choice, destination, offset, chunk count and original total; issue a fresh `begin_descriptor` and `begin_fragment` for **each** ring-slot reuse. The return value 1 means queued, not loaded. An unpaired/overlapping destination is an external pending writer or observation loss. |
| `0x088654D4` state-8 branch; `0x0886551C` after `sceIoRead` | 0024 | Preserve route identity and actual signed read result in `s0` against requested `record[+8]`. Reobserve the full descriptor before each attempt. Only positive equality receives `observe_read`; short/zero/error retries never advance the logical extent. |
| `0x0886577C` classifier result; `0x088652AC` copy-helper return; `0x088659B4` helper return; `0x088659C4` policy result | 0024 | For the outside-region inline route, require the same active descriptor and actual byte-copy return at `652AC`, then the helper and policy results leading to `659CC`. `659B4` alone is insufficient: the helper also has a no-copy exit at `0x0886528C`. Only the complete chain gives `observe_copy(Inline)`. |
| `0x08865368` worker copy return; `0x08865304` event-set return; `0x088657C0` main wait return | 0024 | These are the distinct copy-worker operation and acknowledgement candidates for the classifier's other route. Require one unchanged descriptor and prove its branch/event correlation in an original-code fixture before `observe_copy(Worker)`. Until then the route retains its pending writer and uses original builder. A worker signal alone cannot certify a copy. |
| `0x08865420` before in-place transform; `0x08865428` after; `0x08865440/0x08865448` around conditional SHA-1; `0x088653C4` worker event-set return; `0x08865A10` main wait return | 0024 | For `record[+0x1B]=1`, require actual descriptor-bound transform and matching worker acknowledgement, then `observe_transform` with footprint `round_up_4(chunk_bytes)`. `record[+0x1C]` alone decides whether the SHA-1 helper ran; state-8 evidence is `ComputedOnly` or `NotChecked`, never an invented reference comparison. If deobfuscation is off, record the verbatim branch and no transform receipt. |
| `0x088654F8` early group-4 abort; `0x08865814` before retirement call; `0x08865D8C` retirement entry; `0x0886581C` after return | 0024 | Mark abort independently of retirement. At `65814` retain the final full descriptor before `65D8C` clears its active marker; record postprocess. Only after the original retirement returns at `6581C`, with the complete same-token read/copy/transform chain and no intervening invalidation, call `observe_terminal(true)`. Other terminal routes call it with `false` and leave a pending hazard. |
| `0x088B03DC` allocator call; `0x088B03E4` result; `0x0889E5C0` builder entry | 0043, 0038 | Capture actual manager, computed request `36*child[+8]`, alignment 16 and returned pointer. At `03DC` the final size addition is the call delay slot; capture after that statement or at allocator entry with RA `03E4`, not immediately after the label. `allocate_command` uses the **requested** successful extent, never heap rounding slack. At builder entry require the same owner `+0x13F0` state and `+0x13F4` pointer, exact child/source interval, selected vtable/provider/caller code, current permit and stable execution context. |

The initial proposed integration path is the outside-region inline copy. The
selected synthetic destination `0x09000000` takes that branch; a live owner
address is runtime-dependent, so its classifier result must be observed. The
other copy route already has 14 bounded original AOT/interpreter scenarios
in the ASSET-012 gate with modeled scheduling. Those tests do not establish
production callback coverage: the route receives no live permit until the
actual observation chain and operation/acknowledgement pairing are proved. Multiple queued
fragments are represented in the authority API; a one-fragment fixture does
not establish live split-request coverage.

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
This injector is a fixture-tested prototype, not wired into CMake. Its
callback header/implementation, production controller and lifecycle producers
are still pending; its output has not been compiled or run as an AOT unit.
The remaining lifecycle/worker checkpoints require a separate fixed-address,
shape-checked extension for units 0023, 0024, 0029, 0039, 0040, 0043,
0045 and 0046. An outer `Runtime::register_function` wrapper or pre/post
dispatch hook misses same-unit `goto` transfers and cannot claim complete
observation. Production callback signatures should take
`(Runtime&, const AllegrexContext&, checkpoint_id)` for observations and
use only bounded, read-only guest snapshots; the builder uses the separate
`texture_command_entry(Runtime&, AllegrexContext&) -> bool` and
`texture_command_return(Runtime&, AllegrexContext&, uint32_t jump_target)`
callbacks.

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
