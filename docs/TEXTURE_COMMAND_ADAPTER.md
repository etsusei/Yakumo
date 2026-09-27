# Bounded texture command guest adapter

ASSET-011 connects the [portable command module](PORTABLE_TEXTURE_COMMANDS.md)
to the [certified builder ABI](TEXTURE_COMMAND_BUILDER_CONTRACT.md) in an
offline harness. `native/texture_commands_bridge.hpp` accepts guest memory,
an entry context and explicit source-allocation and command-capacity bounds.
It is not installed as a game hook.

## Prepared plans (ASSET-014, partial integration)

`prepare_texture_commands` now returns a move-only `TextureCommandPlan` and
does not write guest memory or registers. The owned plan holds input context,
bounded source/dependency snapshots, expected output regions and ordered
stores. `compare_texture_commands` reads an original execution's result,
including a separate memory instance, and checks full CPU state, state bytes,
stack gaps, selected command bytes and unchanged source/code dependencies.
It never applies or rolls back predicted writes. This permits a later Verify
controller to prepare first and observe one natural original execution.

`commit_texture_commands` accepts only the preparing memory instance and
rechecks the complete input context and captured regions before the first
store. Changed input, output, stack, source or dependency bytes reject without
further mutation. A successful commit consumes the plan before its first store;
it cannot be replayed, including after a throwing write-watch diagnostic.
Move operations invalidate the source plan. `apply_texture_commands` remains
the convenience prepare/commit wrapper.

These byte checks do not prove allocation lifetime, prevent another thread
from writing, or detect a change followed by restoration. The caller must
retain a revalidated source-authority permit and exclusive execution context.
Allocation failures during preparation can throw before any guest mutation.
No production lifecycle producer or mode controller is installed. The
[observation seams](TEXTURE_OBSERVATION_INTEGRATION.md) now compile in the
production AOT unit with a runtime-scoped callback registry. Bounded original
entry/return and caller-tail fixtures exercise the bridge there; the actual
application still installs no callback owner and enables no replacement.

## Boundary and ownership

The caller supplies the source allocation's byte extent and the destination
allocation's capacity in 36-byte slots. These are required evidence, not values
inferred from mapped RAM, a marker or the resource's declared record count.
The caller also excludes concurrent mutation for the entire operation.
Only the selected output slots are mapped and checked; unused capacity is not
multiplied into an address range. Source extents are limited to 16 MiB and
positive requests to 4,096 blocks or a smaller caller budget. Palette traversal
also has a 4,096-subblock ceiling.

The adapter checks the current builder, descriptor helper and exponent table
identities on every call. It accepts bounded main-RAM spans and rejects
physical overlap between source, state, stack, selected output and protected
dependencies. Address aliases are canonicalized for overlap checks while raw
tokens remain in command words and returned registers. VRAM is outside this
adapter's domain.

Positive calls traverse size-prefixed metadata within the supplied source
extent and each selected record. The initial record/slot indexes use their
low byte; later indexes may exceed 255. A nonzero positive override may select
records beyond the declared header count if the actual bounded records exist.
Palette selection follows the record's subblock count. No TMH marker, guessed
file role or pixel payload interpretation substitutes for those reads.

The no-command branch still writes the state prefix and saved-register stack
slots and preserves the original return context. It does not inspect output
capacity, command pointers or descriptor records. On positive calls, the core
produces the command patches; the adapter reproduces the final descriptor,
saved stack words, state and scratch-register effects. Unwritten frame gaps,
other memory and floating-point/vector state remain untouched.

All range, format, dependency, alias and budget checks, plus adapter allocations,
precede the first guest mutation. Rejected calls leave the entire context and
memory unchanged. This prepares a future caller to retain the original path;
the adapter itself never executes fallback or schedules guest code.

The guarantee applies to returned rejections. The gate disables guest-memory
write watches; throwing host diagnostics during a successful commit are not
transactional. Write-event ordering and diagnostic failure recovery have not
been certified for production use.

## Validation scope

The differential harness runs the actual adapter in separate guest memory and
compares its full CPU context and selected memory/canary regions with both
original AOT and bounded interpreter execution. The report must explicitly
declare `guest_adapter_compared` and exact `adapter_calls`; a core-only report
is insufficient. The runner binds the adapter source/header and binary hashes.

The rejection suite uses the local original ELF to establish real dependency
identities, then constructs malformed and overlapping inputs. It checks the
full RAM, VRAM and context after rejected calls. No original game bytes or
resource payloads are included in the tracked test source.

The Apple Silicon macOS rejection suite passed 10 modeled successes and 68
atomic rejections, including dependency changes, RAM aliases, VRAM exclusion,
truncated metadata, invalid strides/counts, late failures and no-command input.
It also passed with AddressSanitizer and UndefinedBehaviorSanitizer applied to
the adapter, portable core, harness and runtime sources. Strict C++20 warnings,
including signed/conversion warnings, passed for the new adapter and harness.
Five report-validation tests passed.

The plan extension adds 24 checks for read-only preparation/comparison,
changed CPU/regions, cross-instance commit rejection, move ownership, failed
preparation and one-shot consumption. The complete corpus oracle now prepares
before either original execution, compares the same prediction with original
AOT and interpretation, then commits only to its separate adapter memory.
Reports must include `prepared_plan_compared` and exact `plan_calls`; an older
adapter-only report cannot certify this added contract. The updated gate
retains the corpus and synthetic counts below. Current evidence and remaining
integration work are recorded under ASSET-014 in the ledger.

The complete original differential gate covers 2,256 inputs, 8,866 descriptors,
4,512 calls and 11,122 command slots. Another 36 synthetic calls cover 52 slots,
including an override beyond the declared count and palette selection after
two subblocks. Final hash-bound evidence is recorded in the task ledger.

Reproduce with the local supported ELF and resource inventory:

```sh
cmake --build out/mhp3rd --target mhp3rd_texture_commands_bridge_tests mhp3rd_texture_command_oracle -j2
out/mhp3rd/bin/mhp3rd_texture_commands_bridge_tests profiles/mhp3rd/game/EBOOT.ELF
python3 profiles/mhp3rd/tests/test_texture_command_runner.py -v
python3 profiles/mhp3rd/tools/check_texture_commands.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --layout-report out/testing/tmh-layout-complete-inventory.json \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --oracle out/mhp3rd/bin/mhp3rd_texture_command_oracle \
  --output out/testing/texture-command-adapter-rerun.json
```

Bound the standalone harness to 60 seconds with a process supervisor. The
corpus runner already enforces a 60-second timeout per process. Use a fresh
report path; prior evidence is never overwritten.

Production call-site
registration, live allocation-bound provenance, fallback/observation policy,
concurrency guarantees and user gameplay acceptance remain separate work.
Existing delivered applications and user records are preserved.
