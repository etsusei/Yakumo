# Offline native validation gate

This is the Milestone 1 gate in [the development plan](DEVELOPMENT_PLAN.md). It does not launch Yakumo, navigate the game, decode movies for playback, or establish user acceptance. Native switches remain off by default.

## Prerequisites and command

Register B0 and prepare the resources first, following [RESOURCE_PREPARATION.md](RESOURCE_PREPARATION.md). Keep the original inputs at their registered locations. A configured MHP3rd build directory, CMake/CTest, a C++20 compiler, and Python 3.9 or newer are required.

An existing configured build can be used. For a separate test-only build:

```bash
cmake -S . -B out/resource-validation -G Ninja \
  -DPSPRECOMP_PROFILE=mhp3rd -DMHP3RD_RENDERER=OFF \
  -DMHP3RD_FFMPEG=OFF -DCMAKE_BUILD_TYPE=Release
```

The renderer/audio configuration above is for offline tests. It is not the configuration used to assess audiovisual playback. Never run two builds concurrently in the same directory.

Run the gate with the user's local executable:

```bash
python3 profiles/mhp3rd/scripts/check_offline.py \
  --build-dir out/resource-validation \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --baseline out/testing/baselines/B0/manifest.json \
  --output-dir out/testing/offline
```

The runner builds a fixed list of test targets with two compiler jobs, requires the registered CTest suites, and executes all five native differential binaries with the explicit ELF argument. A no-argument example run is not accepted as original-code coverage. The gate checks registered original ISO, ELF, archive and starting-save fingerprints before and after execution.

The current gate records and enforces minimum differential coverage: 806,432 angle cases; 100,512 each for scale, translation and vector; and 100,348 matrix-copy cases. Scale and translation each require 10,000 prefix-fallback cases. Counts may grow. Deliberately reducing a suite requires an explicit coverage-contract update rather than silently accepting any nonzero sample.

The output directory is ignored and local-only. Each run has its own logs, and `report.json` identifies the latest result. A missing prerequisite, timeout, failed command, missing suite, absent differential coverage or changed input cannot be reported as a pass. Preserve failed and incomplete runs for diagnosis.

## Evidence scope

| Evidence | What it establishes |
| --- | --- |
| Framework/save-data unit tests | Tested runtime and save-format contracts |
| Synthetic baseline/resource tests | Input preservation, preparation bounds, staging and verified reuse |
| Synthetic ISO/PSMF tests | Tested reader and demuxer behavior; no visual or audio fidelity claim |
| Five local-ELF differential suites | Original-instruction equivalence under each certified leaf's tested inputs |
| Native mode/counter/error tests | Supported calls, refusal, fallback and bounded-reference behavior |
| Source-input fingerprints | Registered original files remained unchanged during the gate |

The report preserves command outcomes, durations, platform/build identity, binary fingerprints, logs and coverage limitations. Expected negative tests may log deliberate mismatches or refused code. Their assertions and final zero-failure result determine success; searching logs for the word `mismatch` is not a valid test oracle.

The full host application is separately compiled and linked to verify startup/exit integration. Building that executable does not require launching it. User-led paired gameplay remains a later milestone, with the same observational recorder on both versions.

## Current result

The five suites have passed individually, totaling 1,208,316 local-ELF differential cases. The complete application compiled and linked without launch. The combined runner is being implemented and has not yet produced its first complete report; see `OFF-006` in [tasks.json](tasks.json) for current status.
