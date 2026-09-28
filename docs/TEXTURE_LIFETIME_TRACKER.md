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

## Resumed exact-NotReady gate (G0)

After ASSET-014 was resumed, the final strengthened assertion was executed against both normal and AddressSanitizer/UndefinedBehaviorSanitizer oracles. Each gate passes with `authority.failure() == None` and `permit(...).error == AuthorityError::NotReady` after lifecycle-only observations. Each run reports 34 calls on both AOT and interpreter paths, 8 factory branches, 7 owner leases, 4 receipts, each issued once per observed frame, 3 loss cases, full CPU/RAM/VRAM equality, and `transfer_readiness=false`.

The source-bound local-only reports are recorded in `docs/tasks.json`; neither artifact replaces or modifies the historical pause-era reports. Sanitizer coverage applies to the tracker, authority, routing, and harness, not the production runtime/AOT objects. The pause-era `last executed gate` paragraph below records the earlier pre-resume run. For the resumed strict run, the child process received `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; any sanitizer finding therefore makes the oracle exit nonzero and the source-binding runner fails instead of dropping a successful-process diagnostic. Its new local-only report is `out/testing/texture-lifetime-exact-notready-c2c_a27f-i2-sanitized-strict.json`. The task ledger records the SHA-256 and identity closure for all three reports. A local audit recomputed all 32 file identities in each report and found zero mismatches.

The lifetime-only gate was refreshed after adding G1a sources. Current
local-only normal and strict-fail reports are
`out/testing/texture-lifetime-exact-notready-c2c_a27f-i3-final3-normal.json`
(artifact SHA-256 `7a9dfa207e020592313af23e61fc749f16fc9bc844106c75286d3effc1d245a9`)
and
`out/testing/texture-lifetime-exact-notready-c2c_a27f-i3-final3-sanitized-strict.json`
(artifact SHA-256 `6b92a05e5caa6755cad372eb03b23cc8e33aedfc050b35001baf7b6258ae69e1`).
Both raw reports have SHA-256
`e1943886d108f7ac17236293b0a0c771deabfb8f4403d1047289a944738b4dbd`, match all
41 registered input/source/build identities with zero mismatches, and retain
34 calls per path plus the exact healthy-authority `NotReady` result. This
refresh binds the new load/enqueue instrumentation manifests while preserving
the lifetime-only negative assertion.

## Compiled load/enqueue observations (G1a)

The bounded G1a oracle executes the original compiled path from a fixture
owner through unit 0040's selected load/provider call, the manager's real
length lookup, and unit 0023's descriptor commit and queue return. The
callback registry routes transfer events only to `TextureTransferTracker`;
`TextureLifetimeTracker` retains its fail-closed unknown-checkpoint behavior.
Descriptor generations come from the actual 32-byte commit snapshot. A queue
return of 1 closes the enqueue frame only. Every observed destination writer
stays pending across owner reset, command/owner free, address reuse, descriptor
reuse, and observation loss. G1a never calls a load/fragment/read/copy/transform/
terminal completion method and never issues source readiness.

The initial G1a-R0 normal gate is recorded at
`out/testing/texture-transfer-observation-c2c_a27f-i3-final3-normal.json`
(artifact SHA-256
`002913769aa3335d10f15153eed8b88cd29b33e0daa50d19f0bf3b67a4698c26`). The
Its strict-fail ASan/UBSan gate is
`out/testing/texture-transfer-observation-c2c_a27f-i3-final3-sanitized-strict.json`
(artifact SHA-256
`67967def93c112f06245c30afe7f67f015e0d4defa173fb6f15f73e1beca6865`). Their
raw oracle report SHA-256 is
`85b40b43c57c330198b3ed8fcd756abca1c4ead02245d6b7c855410c7f0a474b`; each
report matches all 39 registered source, input, binary, build and manifest
identities. Both compare complete CPU/RAM/VRAM effects over 62 paired
AOT/interpreter calls and 193,515 maximum interpreter slices. The healthy
scenario commits three requests with the same resource ID, including two
physical ring reuses: one through a cached/uncached alias and one at the exact
same raw descriptor pointer. The fixture resets the producer index to slot 0
between calls to exercise those generations; this does not claim a natural
128-entry ring wrap. All three descriptor and writer tokens are fresh, and all
three pending writers remain live. Thirteen missing, duplicate,
mismatched-runtime, mismatched-context, missing-return, changed-code and thread-generation
fault cases plus two capacity-exhaustion cases fail closed. The healthy
owner/command permit remains exactly `AuthorityError::NotReady`; every report
says `transfer_readiness=false`.

For the sanitized run, the child process used
`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Sanitizer coverage applies
to the tracker, lifetime adapter, source authority, routing and harness. The
generated AOT units retain production compiler flags. The PSP imports reached
while initializing the fixture allocator and notifying the queue return zero
without I/O or worker scheduling; no game process or real scheduler ran.

The unowned original texture-command corpus also passed after the callback
routing change. The G1a-R0 report was
`out/testing/texture-command-instrumented-corpus-c2c_a27f-i3-final.json`
(artifact SHA-256
`46c4fd79ee3665302c50b4275f7862a1f7893dcfdcae10b43c607d73bbd6f86d`) and covers
2,256 inputs, 8,866 descriptors, 4,512 builder calls and 11,122 command slots.
The registry and source-authority CTest pair passed 2/2, and 39 related Python
instrumentation, report, runner and Baseline-staging tests passed.

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
and owner-heap reset. The latter preserves the separate command heap. The pause-era gate refused source permits for all positive caller receipts:
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

Remaining work includes actual read/copy/transform/terminal/cancellation
receipts, independently quiesced workers, coherent code epochs, writer
exclusion and an authority-backed Off/Verify/Native controller with matched
recording. G1a does not complete G1 and does not enable Native. A related paired
gameplay case comes only after those checks. Existing applications and user
records remain unchanged.

## G1a-R1 descriptor reuse and range hardening

R1 tightens generation accounting at the real descriptor-commit checkpoint.
`gpr[6]` is the descriptor start at `0x08863DD8`; the original ring address
calculation has already added 12 bytes to skip its prefix. When any valid new
32-byte descriptor overlaps recorded descriptor history, the tracker issues
a fresh descriptor token and marks every superseded overlapping record
non-current. Reuse by an unowned manager request is recorded even when the new
destination is valid RAM outside watched owner slots. The old asynchronous
writer remains pending; descriptor reuse does not imply cancellation or
completion. A later selected reuse receives another fresh generation and
cannot resurrect the old one.

The tracker checks RAM-range validity separately from watched-slot overlap.
Zero length, pointer arithmetic overflow, destination-plus-offset overflow,
and spans outside guest RAM fail closed rather than being treated as
disjoint. A valid 0x5800-byte selected span is accepted as an observation but
remains pending and `NotReady`; a 0x5801 request is rejected while retaining
both the previous and newly observed writer hazards. A five-byte transformed
chunk tracks its eight-byte rounded write footprint. Wrong manager and wrong
descriptor destination are injected at the actual callback using the original
runtime/context identity, so the tests reach the intended validation.

The iteration-4 R1 reports below are historical and are superseded for the
updated publication tool by iteration-5 reports:
`out/testing/texture-transfer-observation-c2c_a27f-i4-r1-final6-normal.json`
(artifact SHA-256
`eb634aee6913aa34d73c818b55969f67e747acac293a2adaeabe07d45808f885`) and
`out/testing/texture-transfer-observation-c2c_a27f-i4-r1-final6-sanitized-strict.json`
(artifact SHA-256
`9d3287ecbefa2a42e2bd80508dd37ce47b8a03bc1ba165cd8d8f601e08703fa5`). Both
have raw-report SHA-256
`e6bf5eb1a6dc667a3a8ff356ff6f87a130ab0e3e9bd05fb3d46fd3683ac2ac8a`, match
all 39 registered source/input/binary/build/manifest identities, and report
the same exact counts: 117 AOT/interpreter calls, four selected requests, five
descriptor generations, three controlled descriptor-slot reuses, one unowned
reuse/enqueue, four live pending writers, fourteen fault cases, two capacity
losses and four range faults. They compare full CPU/RAM/VRAM effects, retain
healthy `AuthorityError::NotReady`, and keep `transfer_readiness=false`. The
strict child uses halt-on-error ASan and UBSan settings. It sanitizes the
tracker, lifetime/dispatch routing, source authority and harness; it does not
sanitize production runtime/AOT objects.

R1 also covers one zero-length fault, exact and over-slot boundaries, a
rounded transformed footprint, stale `ctx.pc`, raw pointer reuse and a
cached/uncached alias. No application or game was built or run, and no native
mode or source readiness was enabled.

## G1a-R1 publication transaction follow-up

The transfer instrumenter now creates both temporary outputs inside one
cleanup scope. If installing the new output/manifest pair fails, it attempts
every rollback independently. It deletes old backups only after both new files
are installed successfully; when restoring an old file fails, the remaining
backup is preserved and the raised error names its recovery path. Tests inject
second-temporary creation failure, backup movement failure, new manifest
installation failure, and a rollback unlink plus restore failure. They verify
that a successful rollback restores both original bytes and an incomplete
rollback retains the old output bytes in the named backup. This covers ordinary
filesystem I/O errors, not process-crash atomicity across two renames.

The current iteration-5 G1a reports are
`out/testing/texture-transfer-observation-c2c_a27f-i5-final7-normal.json`
(artifact SHA-256
`ddfe6d72471769910af724b4ada49526255f999c2f9fc81e2faa57e9a14c6269`) and
`out/testing/texture-transfer-observation-c2c_a27f-i5-final7-sanitized-strict.json`
(artifact SHA-256
`ff5b86dcb68fea19279f0195bfcba5509003d0c90de320dbaf0b4d0a760efef3`). Both
share raw-report SHA-256
`e6bf5eb1a6dc667a3a8ff356ff6f87a130ab0e3e9bd05fb3d46fd3683ac2ac8a`, match
39/39 current source/input/binary/build/manifest identities, and retain 117
AOT/interpreter calls, five descriptor generations, four pending writers,
fourteen callback faults, two capacity losses, four range faults and
`transfer_readiness=false`. The normal and sanitized oracle binary hashes are
`a7321df0aaafc5b690b98c38aeab309b9fb2cc3b770f2cc837956c480abd1acd` and
`708c8d6df6f3ed0d4cee53833ceeca4b54918015f2572e10097e6cd121f4e0aa`.

The current G0 exact-NotReady reports are
`out/testing/texture-lifetime-exact-notready-c2c_a27f-i5-final8-normal.json`
(artifact SHA-256
`00ec2ba4147c8516092e8fae8129233914df10f1aee579c6b3471d693d4b4303`) and
`out/testing/texture-lifetime-exact-notready-c2c_a27f-i5-final8-sanitized-strict.json`
(artifact SHA-256
`24020b131f2ba99d58829a645bee4e0a6747d75efc9e604f90b765315298b80e`). Both
share raw-report SHA-256
`e1943886d108f7ac17236293b0a0c771deabfb8f4403d1047289a944738b4dbd` and
match 41/41 current identities. They preserve the exact healthy-authority
`NotReady` check, 34 calls per path, full CPU/RAM/VRAM equality and
`transfer_readiness=false`. Their normal/sanitized oracle binary hashes are
`9f59154f14fbeba31b0e5d8fc418ed3d4a4042d000c6c7b611cb661cd8b6e56b` and
`a0c3f8be7440ed29bbf737cfe81ac4ca4ad1e28f6f7d4b1cba0b05bac7c1654b`. The
strict G0 child received `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; the G1a strict runner
records the same options. Sanitizers instrument the test boundary and harness,
not production runtime/AOT/overlay objects. The lifetime runner now describes
four receipts as issued once per observed frame; it does not call them
one-use execution capabilities.

The exception-injection suite has ten tests; nine ASSET-014 Python modules pass
50 tests, and the focused source-authority/dispatch CTest pair passes 2/2. The
iteration-4 full Python discovery record is `Ran 258 tests; FAILED (errors=7)`.
Those seven errors involve six `test_tmh_pixel_oracle` methods (one has two
failing subtests): discovery leaves the module's `ORACLE` variable as `None`,
so `FileNotFoundError` occurs before the oracle is invoked. This is a test
runner invocation issue, not a pixel-oracle comparison result. No unverified
pass count is inferred from the seven errors.

## G1b-read descriptor and attempt observation (iteration 6)

G1b-read adds a read-only callback family for the original worker state-8 entry
(`0x088654D4`), helper entry (`0x08863608`), actual `sceIoRead` tail call
(`0x0886365C`) and signed result before the original compare (`0x0886551C`).
Each observation rereads and checks the complete 32-byte descriptor against its
selected descriptor generation, load/request generation, owner lease and
per-owner invalidation generation. State-8 entry binds the worker manager,
consumer slot, record address and descriptor; helper entry binds the fd;
invocation binds fd/scratch/request length; result binds the returned signed
value. Read attempts receive bounded monotonic serials and remain separate:
short, zero and negative reads are retries, are not accumulated, and an exact
positive return only closes that attempt. No `SourceAuthority::observe_read`,
copy/transform receipt, completion receipt or writer retirement is issued.

The build-local unit 0024 copy contains a test-only prefix stop after the
`ReadResult` callback. The AOT path returns before the branch at `0x0886551C`;
the interpreter stops at the same program counter. The oracle compares full
CPU, RAM and VRAM state, then verifies that the owner destination is unchanged.
The stop hook lives only in a separate `EXCLUDE_FROM_ALL` read-prefix object
library and is not linked into the application. Both read and transfer boundary
options remain default Off.

Final normal and strict-fail ASan/UBSan reports are
`out/testing/texture-read-observation-c2c_a27f-i6-final2-normal.json`
(artifact SHA-256
`67189dea6d05733fe452c651979545e548c53bd7cc3b2a92db0fc28d4da166e8`) and
`out/testing/texture-read-observation-c2c_a27f-i6-final2-sanitized-strict.json`
(artifact SHA-256
`c2d81d57efc929346797c99444ad5159fd17a13ba4de9c4b164a7e958ee095b1`). They
share raw-report SHA-256
`4ee7731cdb322a1e714d37384a9e23226cda8fac1ac0705810c441fcb295214f` and bind
30 input/source/build/manifest/binary identities. Each passes 69 scenarios and
412 original AOT/interpreter calls, with a maximum of 193,515 interpreter
slices. The observed result counts are 11 exact, 9 retry, 1 rejected overread
and 1 unowned result; all 68 selected scenarios retain their pending writer.
The selected healthy paths still receive exactly `NotReady`, and
`transfer_readiness=false`.

The descriptor mutation matrix samples 12 execution-affecting fields across
the four callbacks (three fields at each edge); all four edges compare the
complete byte snapshot. A 185-scenario and then a 105-scenario strict attempt
hit the 300-second runner limit without sanitizer diagnostics. The final
69-scenario suite preserves all read/correlation/capacity/invalidation paths
while keeping a practical bounded strict run. `sceIoRead` data and event
scheduling are modeled: real file identity/open/seek, concurrent PSP worker
scheduling and unsampled change-and-restore races are not covered. An exact
syscall result is not proof of bytes loaded into the target slot.

G0 and G1a-R1 were refreshed after the tracker changes. The final G0 normal and
strict artifacts are
`out/testing/texture-lifetime-exact-notready-c2c_a27f-i6-final1-normal.json`
(SHA-256 `aaad03c326aad9f72dedaa9c5bf94d29af4b45dc25d8fd4becd4d4772f8f9312`)
and
`out/testing/texture-lifetime-exact-notready-c2c_a27f-i6-final1-sanitized-strict.json`
(SHA-256 `4d045a49bf425e000f12d206a9787052f864d188c3ee127b241a14b6c78d1700`).
The final G1a normal and strict artifacts are
`out/testing/texture-transfer-observation-c2c_a27f-i6-final2-normal.json`
(SHA-256 `63a07f90cfdd835562dadf0e79b37b30598d743bb2643a3b4578279bc42c5627`)
and
`out/testing/texture-transfer-observation-c2c_a27f-i6-final2-sanitized-strict.json`
(SHA-256 `76337a40157c06f82526c7c9d5fbab2b17bb76291e6d36d6df1a5b0f6126cb35`).
They match the G0/G1a raw reports and preserve exact `NotReady` and all pending
writers. The unowned original texture-command corpus also passed 2,256 inputs,
8,866 descriptors, 4,512 builder/portable/adapter/plan calls and 11,122 slots;
its local report is `out/testing/texture-lifetime-unowned-corpus-c2c_a27f-i6-final1.json`
(SHA-256 `36f10495c88874c78e0c1b3122319e08a7a34b2c6c4664bc60345ef9bb4a6920`).

Iteration 6's first G1b-read result was not closed by review. The reviewer
identified a retry that could skip a new State8 callback, missing generation
initialization on a historical unowned descriptor, an unstaged read CMake
include, and incomplete source/count closure. Those findings are retained as
review history; the 69-case reports above are not evidence for the R1 source.

## G1b-read-R1 retry and evidence hardening (iteration 7)

After a retry result, selected read frames now require the next state-8
checkpoint before HelperEntry. The helper cannot clear a pending State8
requirement. State-8 observation copies the current descriptor and request
generations for every current record; owner token, request validity, slot
invalidation and pending-writer checks remain selected-only. Two actual
compiled selected-to-unowned ring-reuse cases cover the same raw descriptor
address and cached/uncached aliases. They confirm the old selected generation
is revoked, the old writer remains live, the unowned read creates no selected
attempt, and authority stays `NotReady`.

The fixed 75-scenario matrix adds the dropped-second-State8 counterexample, two
historical-unowned reads, a sum-of-shorts retry, a labeled synthetic partial
descriptor-storage overlap, and an active-attempt same-byte generation change.
The last case drives the production tracker commit callbacks with a controlled
context to test rejection of a late result; it does not claim a concurrent
compiled producer schedule. The strict single-process run reached its
300-second budget without sanitizer diagnostics, so the unchanged 75-case
matrix is executed in three contiguous 25-case shards. The runner verifies
each ordered ID range, aggregate counts, complete duplicate-free coverage and
one unchanged source/binary identity across all shards. The normal run passes
450 AOT/interpreter calls, 85 modeled import calls, 40 selected attempt
records, and full CPU/RAM/VRAM comparison. It records 11 exact attempts, 12
retries, one rejected result and three unowned results. All 75 writer
observations retain hazards. The source-bound report covers 52
input/source/build/manifest/binary identities. Normal artifact SHA-256 is
`1b65742457490d26deff6dd219227d6f6e395472eed6140ff3c0892c66ecc32f`; merged
report SHA-256 is
`86f0bc2890eaaee0d6a2d18297acfdb036696aec6f85244fbe4c8912dbb06320`; child
report hashes are
`03cc6a6d514ed58e4b16213d9441fa8dfa54c811c11033cc7972a06a8ef4f633`,
`dc9ef104ddc758322d290d8d71fb012b72e852d0ea8e37b8071ffffc5eae6191`, and
`e5ed9be95b0c746b98dad854ff6dfa36254e0b66936a05c61ece3201b1e02597`. The
normal/strict oracle binary SHA-256 values are
`1b1e70db6d2bb3014b65aab31d6b0698e954949cdda8bec46769d8a1f011c17e` and
`868fe92e81f6c933522c9be42dd06a83a70b205964e36e9800d28a5e0dec0ebb`. The
strict artifact SHA-256 is
`e51fc1f8c78094e7bc1c4580336269433bdd2f8c166ad4696343923d50d4af31`; it uses
the same merged report and child hashes as normal. Both runs set
`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` in each shard.

`prepare_observed_baseline.py` now stages the unconditional
`TextureReadInstrumentation.cmake` include. A fresh pinned-B0 stage configured
successfully with read, transfer and probe boundaries off; this was a
configure-only check and did not build or launch Yakumo. The source-bound
runner verifies upstream probe/lifetime/transfer/read inputs, outputs and
manifests, pins scenario IDs and outcome counts, and checks attempt-to-selected
invocation correspondence. Refreshed G0 and G1a-R1 normal/strict reports pass:
G0 artifact SHA-256 values are
`49deb5f982a68ebded9aef40e0973cf92f5c9214c3b3ffd6b3a4810360150717` and
`7a3657543533f0b512a3a85b9613c7fb99ce5e5017047c1c682f0f0fa68f9177`; G1a-R1
values are `66e8a3481138817374e53742b648d70d83b599fc8dd54a5acf722f2739ef45c5`
and `3fca2df6f2a9bcdbb9b1f8ba8dfe9a727935bf578f6b54a9d1ed27f475b550a5`. The
2,256-input unowned corpus also passes 2,256 inputs, 8,866 descriptors, 4,512
calls per path and 11,122 command slots; artifact SHA-256 is
`184a2fd2b13a77fcb7b0e5c4584863f0e9d19cbeb27e9e6ec58f9062ccbe6793`. The 53
scoped Python tests and two focused CTests pass.

G1b-read-R1 ends at the read-result prefix and has been accepted in scope by the independent C2C review. Copy, transform, worker acknowledgement, terminal, cancellation/quiescence, epochs, writer exclusion, the Native controller and new paired applications remain outside this slice. ASSET-014 stays in progress; no next phase is authorized until the user requests another concrete batch.
