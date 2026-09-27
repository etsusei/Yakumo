# Original lobby owner construction and command release

The [overlay audit](TEXTURE_OWNER_OVERLAY_AUDIT.md) establishes the exact
factory allocation pointer passed into the lobby constructor. The standalone
`mhp3rd_texture_owner_oracle` executes the original allocator, placement
identity helper, constructor, source provider, state reset and release helpers
with constructed heap/stack inputs. It does not run the game or its scheduler.

## Independent original execution

The AOT path registers the existing main corpus and the original lobby overlay
library. The interpreter path loads the same supported ELF and raw overlay
bytes but registers no AOT functions. Both start with equal memory/context.
The harness compares full RAM, VRAM and CPU state after each top-level call.
The original lobby constructor and its real base constructors execute; neither
the constructor nor the fill helper is replaced by a test stub.

The library is read from the preserved local test application. Its exact
SHA-256, module ABI, slot address, code size and header/code FNV identity are
checked before use. The test is currently macOS-only and does not establish
compatibility with other builds of the same library. The loaded library's
runtime ABI must remain compatible with the harness; no reusable runtime
headers were changed in this work.

The caller allocates `0x2F470` bytes from the original reverse allocator and
passes that exact pointer through the original placement helper. Resource
slots start with nonzero test bytes so the constructor's fill is independently
observable. It installs vptr `0x0896FBC8`, returns 1, and clears all `0x2E000`
slot bytes. The harness checks that constructor writes stay within the owner
allocation and a bounded stack window. The real slot-7 provider then returns
`owner+0x27C70`.

A separate original heap supplies a 108-byte command allocation. The harness
puts that pointer in the owner's actual texture state, then invokes original
`0x088A3474`. That helper frees the command allocation and clears the 16-byte
state. The matching original allocator node records the free. The command
heap and owner heap are distinct, as required by the recovered release path.

Finally, freeing the owner leaves its old vptr in memory, and another original
allocation returns the same address with that stale vptr still present. This
demonstrates why vptr equality and mapped RAM cannot replace a live allocation
identity. No production lifetime registry is implemented by this test.

## Results and reproduction

On Apple Silicon macOS, both ordinary and uncached address variants passed:
22 top-level original calls per AOT/interpreter path, two constructor cases,
two state resets, two command release cases, two freed-owner vptr observations,
and two exact owner address reuses. The largest call used 96,567 bounded
interpreter slices. Four report-validation tests and strict C++20 compiler
warnings, including signed/conversion warnings, passed.

The final local report is `out/testing/texture-owner-provenance-final.json`.
Its runner binds the original inputs, local AOT library, oracle binary and
source files before and after execution. Each process is limited to 60 seconds,
each call to 2,000,000 interpreter slices and 10,000 AOT dispatches. The report
retains actually executed code pages and refuses overwrite.

```sh
cmake --build out/mhp3rd --target mhp3rd_texture_owner_oracle -j2
python3 profiles/mhp3rd/tests/test_texture_owner_runner.py -v
python3 profiles/mhp3rd/tools/check_texture_owner.py \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --overlay profiles/mhp3rd/analysis/resources/raw-entries/00122.bin \
  --module 'out/testing/dist/render-discovery/Yakumo Baseline.app/Contents/Frameworks/overlays/ovl0A05E600_lobby_task_F6300296C8D954E5.dylib' \
  --oracle out/mhp3rd/bin/mhp3rd_texture_owner_oracle \
  --output out/testing/texture-owner-provenance-rerun.json
```

## Remaining scope

The full factory and outer owner-removal workflow are not executed by this
harness. It tests their identified component functions with explicit inputs;
the static audit separately establishes the real pointer chain. Successful
asynchronous source loading, source-ID-to-slot mapping, gameplay reachability,
concurrent mutation and complete interception of invalidation events remain
unverified. A cleared slot is not a loaded resource. ASSET-012 stays in progress
until a usable loading/lifetime authority is specified without conflating
construction, queue state and source bytes.
