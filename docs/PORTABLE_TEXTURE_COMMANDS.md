# Portable texture command generation

`host/resources/texture_commands.hpp` exposes a value-only command generator
for the [certified original builder](TEXTURE_COMMAND_BUILDER_CONTRACT.md).
It has no Runtime, GuestMemory, register, environment or renderer dependency.
It returns owned command patches and state metadata; it does not write guest
memory, install a hook or change the delivered applications.

## Explicit input and result

Each descriptor supplies image/palette address tokens, format, dimensions and
palette count. Tokens are 32-bit data, not host pointers: the core encodes
them without dereferencing or asserting that storage exists at those values.
The request supplies source and command-buffer tokens, the full declared
header count, initial source record and destination slot, a raw count override,
and caller-provided destination capacity. Payload ownership and any physical
alias checks belong to a later adapter.

Zero override means use the declared count, **not** the number of records
remaining after the first index. A zero or signed-negative effective count
returns metadata with no patches and no descriptor/destination inspection.
Positive requests mask only the initial record and slot to eight bits; later
indexes can exceed 255. Each patch names its source record and destination
slot and owns nine command words. Partial updates leave all other slots to
the caller. The full declared count and its low-byte state field are retained
separately from the number of emitted patches.

The command-buffer token is preserved rather than advanced as a cursor. The
original store order is available as `kTextureCommandStoreOrder` for a future
ABI adapter; producing a patch does not itself perform those stores.

## Checked domain

Positive requests are limited to 4,096 blocks, or a smaller caller budget.
The full selected source range, destination capacity, word alignment and last
32-bit command address are checked before output is produced. Dimensions must
be in `1..1024`, matching the certified exponent table. Formats `0..10` are
recognized. Formats 1, 3, 8, 9 and 10 require the zero palette metadata emitted
by the original helper. Other formats accept palette IDs 0..3 and nonnegative
signed-16-bit counts, including zero; the load count is not clamped to a
renderer palette limit.

A later invalid descriptor cannot leak earlier patches. Errors return a typed
reason, no state update and no patches; required range sizes remain available
for diagnostics. Inputs are unchanged, and successful output survives later
input mutation or destruction. Allocation failure retains ordinary exceptions.

The checked domain intentionally rejects undefined or unverified raw original
inputs. It does not widen the owned TMH reader's established file-format scope,
validate pixel payloads or prove that commands are safe to submit to hardware.

## Differential and synthetic verification

The original-code gate now calls the actual portable implementation and
compares its patches/metadata with the independent packet model and original
AOT/interpreter output. Its report requires `portable_core_compared: true` and
an exact `portable_calls` count, so an older original-only report cannot stand
in for a portable-core result. The runner binds the core source/header and
binary hashes alongside the original executable and resource inventory.

The standalone core suite covers all eleven formats, palette count rounding,
no-command requests, initial byte masks, indexes passing 255, partial metadata,
late failure atomicity, address overflow/alignment, output/source limits,
budgets and output ownership. The core passes strict C++20 compiler warnings
and AddressSanitizer/UndefinedBehaviorSanitizer: all 55 checks passed on
Apple Silicon macOS. Five runner rejection tests also passed.

The completed local gate covers 2,256 inputs, 8,866 descriptors, 4,512
portable/original calls and 11,122 command slots. Another 32 synthetic calls
cover 44 slots, including all eleven image formats and both address mirrors.
The largest original interpreter call used 19,168 bounded slices (3,420 for
the synthetic set). Every actual portable result agreed with the independent
model and original AOT/interpreter paths. The report is local at
`out/testing/texture-commands-portable-original-gate.json`; its SHA-256 is
`8b21afd92fa9eeccdb83117605b522a736716a4d8f74ca09fab6077def3a22da`. Independent read-only review found no actionable defects.

Reproduce after building `mhp3rd_texture_commands_tests` and
`mhp3rd_texture_command_oracle` with `cmake --build`:

```sh
ctest --test-dir out/mhp3rd -R '^mhp3rd_texture_commands_tests$' --output-on-failure
python3 profiles/mhp3rd/tests/test_texture_command_runner.py -v
python3 profiles/mhp3rd/tools/check_texture_commands.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --layout-report out/testing/tmh-layout-complete-inventory.json \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --oracle out/mhp3rd/bin/mhp3rd_texture_command_oracle \
  --output out/testing/texture-commands-portable-original-gate-rerun.json
```

Use a fresh output path: the runner does not overwrite prior evidence.

Guest register/stack effects, physical memory overlap, original dependency
guards and actual call-site integration remain outside this core. They must
be handled by a separately tested adapter; command generation alone is not
gameplay or rendering acceptance.
