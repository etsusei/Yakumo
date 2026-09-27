# Original transfer completion gate

The gate executes original `0x08865450` reader control flow, its original
inline/copy-worker paths, and original `0x08865378` postprocessing. It compares
AOT and single-step interpretation with explicit, identical imported-boundary
models. File reads, delays, cache operations, event scheduling and sleep are
models; the original copies, deobfuscation, SHA-1 code and queue control are not
stubbed. Event waits run the relevant original worker through its next wait,
retaining its completion signal and full CPU context for comparison. This is
not a test of actual concurrent scheduling or PSP cache hardware.

## Exercised scenarios

All 64 scenarios per path passed on Apple Silicon macOS. Each compares whole
RAM, VRAM, main-worker CPU, secondary-worker CPU contexts and modeled import
arguments/results. Fixtures cover ordinary and uncached addresses, both sides
of the original special copy-address classification, mode-0 copying and mode-1
deobfuscation, full reads, short/zero/error reads followed by an exact retry,
an explicit descriptor clear during a retry, and original group cancellation.

- 24 retry scenarios complete only after an exact-length read.
- Eight injected-marker-clear scenarios leave the destination untouched. These clear the active descriptor at the
  modeled delay boundary; they do not execute an owner or manager reset.
- Sixteen group-cancel scenarios still write the active destination later;
  status-pointer signaling does not itself prevent the write.
- Four five-byte cases transform eight bytes. The three padding bytes come
  from existing destination memory, not fabricated input; outer canaries hold.
- Four private-sample cases use original encoded entry 01489 (`0x5800` bytes)
  from its manifest-bound ISO window. This is the extracted block span,
  not a separately established semantic root length. Original deobfuscation matches the
  extracted entry exactly, and original SHA-1 output matches its original ELF
  reference entry. The test harness makes that digest comparison: **the
  state-8 worker does not enter the alternate-file comparison branch**.

The original transform worker runs in 56 scenarios, the separate copy worker
in 14, and data deobfuscation in 32. The original raw-address classification
means an uncached alias of the special region can take a different copy route;
the test preserves that behavior. The largest interpreted context uses
1,485,992 bounded slices, including modeled import boundaries.

## Reproduction and evidence

```sh
cmake --build out/mhp3rd --target mhp3rd_texture_transfer_oracle -j2
python3 profiles/mhp3rd/tests/test_texture_transfer_runner.py -v
python3 profiles/mhp3rd/tools/check_texture_transfer.py \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --iso profiles/mhp3rd/game/disc.iso \
  --workspace profiles/mhp3rd/analysis/resources \
  --oracle out/mhp3rd/bin/mhp3rd_texture_transfer_oracle \
  --output out/testing/texture-transfer-completion-rerun.json
```

The final local report is `out/testing/texture-transfer-completion-certified.json`.
The runner stages only the selected private sample, checks its exact encoded
and decoded hashes, and verifies the selected ISO window before and after.
It does not claim to rehash every byte of the ISO. ELF, manifest, source,
binary and decoded-entry hashes are bound before/after execution. Publication
refuses overwrite, and the process has a 60-second limit. Five report-rejection
tests and strict C++20 compiler warnings passed.

The gate starts the reader in state 8 with a positioned modeled descriptor;
it does not execute the complete open/seek/enqueue path. Single-fragment cases
do not establish multi-fragment coverage. The alternate `ms0:` route, its digest
failure handling, actual cache/scheduler timing and live selector-7 traffic
remain unverified. The sample's identity does not prove it appears in any
particular live scene.

These results complete the finite ASSET-012 boundary investigation and support
the [source-authority contract](TEXTURE_SOURCE_AUTHORITY.md). ASSET-013 must
implement and test that bounded state machine; actual production observation,
hook completeness and user gameplay acceptance remain subsequent work.
