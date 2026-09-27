# Original allocation observations and caller receipts

ASSET-014 now has an observation-to-metadata adapter for the selected lobby
owner. `TextureLifetimeTracker` receives 14 actual before-instruction
checkpoints through the runtime-scoped dispatch registry. It never writes
guest memory, creates source-transfer receipts, requests a source permit, or
executes a native replacement. No application installs it yet.

The guarded build-local transformer covers allocator unit 0029, owner reset
unit 0040, selected caller unit 0043 and factory unit 0046. Unit 0029 composes
with the existing leaf-probe copy. Original generated sources and prebuilt
overlay libraries stay unchanged. Each required instruction/call block has a
shape fingerprint; manifests identify inputs and outputs. The callback uses
the checkpoint enum's fixed PC rather than stale `ctx.pc`.

## From observed steps to metadata

- A reverse allocation with return address `0x088BD07C` begins a bounded
  factory frame. It must name the configured manager, `0x2F470` bytes and
  alignment 16. At the continuation, `r2` is the result; `r18` is not yet set.
- The frame pairs that allocation with `0x088BD13C` and the real lobby
  constructor return at `0x088BD144`. Only result 1, current code, vptr
  `0x0896FBC8` and a cleared `0x5800`-byte selected slot issue an owner lease.
  A vptr scan alone never issues a lease.
- The selected caller pairs the live owner with actual provider/accessor
  results and command allocator arguments/result. Its extent is the successful
  requested size, without allocation padding. Source readiness remains unproven.
- `0x088B03F8` arms a receipt before the call. Builder entry validates the
  post-delay-slot registers, tokens, raw pointers, context, stack, thread
  generation and current code. A second consumption of that frame returns no
  receipt. The value is copyable metadata, **not a one-use execution
  capability**: a controller must use it only at the current entry and obtain
  fresh source authority before commit. The old command-state pointer is
  allowed at entry because the builder has not stored the new pointer yet.
- Matching free and heap reset/init revoke leases and frames through
  `SourceAuthority`. Owner reset revokes source state while preserving the
  owner allocation. Address reuse gets a new token. Physical alias checks
  retain raw owner/child/command tokens and normalize manager identity.
  Interior frees into tracked allocations fail closed.

Storage is bounded to 32 owner/command leases and 16 pending frames. Missing
or conflicting phases, capacity exhaustion, mismatched runtime, changed code
or stale thread identity latch authority observation loss. Restoring bytes
alone cannot recover it. Lease queries then return no usable token; counters
retain historical observations. Original guest execution stays unchanged.

The required `current_code` callback must validate executable bytes and a
coherent epoch, including overlay replacement or same-byte reload. The offline
gate compares executable ELF sections and overlay header/code with verified
originals. This proves the tested byte checks, not a production epoch manager.
The callback registry does not exclude all guest writers. Serialization with
source events and exclusive execution remains necessary before Native mode.
A child returned through another raw RAM alias is conservatively ineligible.

## Original-code evidence

The macOS gate executes 34 calls on each original AOT/interpreter path and
compares full RAM, VRAM and CPU. Constructed outer stack/heap frames enter the
actual `0x088BD058` factory branch: reverse allocation, zero fill, registry
insertion, placement helper, prebuilt lobby constructor and normal epilogue.
Eight factory branches run; seven produce observed owner leases because one
deliberately omits a checkpoint. Seven selected caller tails run the original
provider/accessor/allocator/builder chain; four issue current-frame receipts.
The other three cover code loss, thread switch-away/back and no commands.

Ordinary and uncached aliases cover owner/command free and reuse, owner reset
and owner-heap reset. The latter preserves the separate command heap. The last executed gate refused source permits for all positive caller receipts:
supplied fixture bytes are not transfer receipts. A stronger assertion now
requires explicit `NotReady` and healthy authority; it compiled but was not
rerun before the user-requested pause. Three loss cases omit a constructor
checkpoint, change constructor code before a caller, and inject a host thread
switch-away/back. No stale receipt is issued. Maximum original execution is
193,515 interpreter slices, under the two-million bound; each process is
limited to 60 seconds. No real scheduler or asynchronous transfer is tested.

Seven registry cases pass CTest, including read-only lifetime-only binding.
Six injector tests use synthetic instruction bodies, with two strict report
tests. Eight Baseline staging tests cover shared files and recording revision
inputs. The prepared command corpus is a separate unowned-path regression.

`mhp3rd_texture_lifetime_sanitized` applies AddressSanitizer and
UndefinedBehaviorSanitizer to the tracker, source authority, routing and test
harness. Existing runtime/AOT/overlay objects retain production flags; this
does not sanitize the entire game.

Build `mhp3rd_texture_lifetime_oracle` with `cmake --build -j2`, then run
`tools/check_texture_lifetime.py` with `--elf`, `--overlay`, `--module`,
`--oracle`, `--build-dir` and a fresh `--output`. It checks known original
inputs, all four build-local manifests, exact counters and source/binary
identities before publishing local evidence. The ledger records final hashes.
Game data and reports stay ignored.

Remaining work includes load/descriptor/worker/cancellation receipts, pending
writers, coherent code epochs and an authority-backed Off/Verify/Native
controller with matched recording. A related paired gameplay case comes only
after those checks. Existing applications and user records remain unchanged.
