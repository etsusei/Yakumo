# Building an observed B0 source tree

PAIR-002 uses the pinned Baseline B0 source at `4292eb66ee66eab37c327575382d071addcf6249` and the same recording, probe, and Chinese case-panel revision as the candidate. B0 is a source identity, not the older executable archived with its registration. The registration explicitly calls that binary historical, with unverified source-to-binary provenance. A paired Baseline application must be newly built and identified as **B0 plus an observational overlay**, with both identities recorded.

This audit compared B0 with candidate source `a3f041f96f0d631540664d6e98f884df6d7c7314` on 2026-09-27. There are no tracked changes under `src/` or `include/psprecomp/` between those revisions. The registered `source.tar` SHA-256 is `35e1a97acd89a6ed78cc8154b1152afebeb33c3b369da344c49ca04b0ab114f1`, and its commit tree is `f4c74d45c194fc215a8266227c2c3b53cd995b88`. The preparation command checks both against the registration and recomputes the Git archive bytes. It does not use the historical binary as its reference.

## Source boundary

The source starts from the registered B0 archive. [The preparation tool](../profiles/mhp3rd/tools/prepare_observed_baseline.py) has an explicit per-file allowlist for the shared observation layer and offline test/packaging tools. Its manifest records the SHA-256 and origin of every staged file, the original B0 hash for each override, a source-content digest, and the observer revision computed by the same hash recipe as the application build. No generated code or game data enters the source archive or this manifest.

| Source area | Observed B0 selection | Reason |
| --- | --- | --- |
| `src/`, `include/psprecomp/`, other unlisted tracked files | Registered B0 bytes | Retain the execution runtime and all unrelated game support exactly as pinned. |
| `host/kernel/iso_image.cpp`, `host/movie/psmf_demuxer.cpp` | B0 bytes | Candidate adds parser hardening; this is a behavior change outside observation. |
| `host/native/angle_step_bridge.*`, `host/native/scale_matrix.hpp`, `host/native/scale_matrix_bridge.cpp` | B0 bytes | Candidate changes verification contracts and probe-aware bridges. B0 keeps its original two implementations, with their native modes sealed off at launch. |
| New translation, vector, and copy native implementations and bridges | Absent from the Baseline application | These are candidate migration work. Baseline AOT probes can still observe their original generated-code calls. |
| `host/testing/*`, probe instrumenter, case panel/catalog/controller, journal and comparison tools | Same reviewed observation checkout as candidate | Gives both roles the same event protocol, recorder semantics, UI instructions, and probe boundaries. |
| Camera, SDL, input-script, overlay, settings, menu and text-input hooks | Reviewed observer overrides | Add read-only events and test UI to existing behavior. Their exact source hashes are in the staging manifest. |
| `host/main.cpp`, `host/hle/hle_media.cpp`, `CMakeLists.txt` | Reviewed mixed-file overrides with Baseline-specific compilation seams | These files connect the recorder to B0. They require special checks below; copying the candidate program unguarded would not establish B0 provenance. |

`host/hle/control_delivery.*` is copied so the observation revision has the same source inputs in both roles. The Baseline control HLE retains B0's literal guest-buffer write loop and emits the same `GameObserver::pad` event only after that loop succeeds. The helper is not the Baseline's delivery path. The cached `kernel().now_us()` getter means the earlier candidate refactor was not established as a gameplay difference; both current roles now use the original loop plus the observation hook.

The `host/main.cpp` Baseline compilation path skips all native configurator calls, so no replacement bridge can be installed. It does not include or report the three candidate leaves. All five replacement environment settings are checked before asset lookup, with anything other than `off`/`0` rejected. The `MHP3RD_BASELINE_B0` build definition seals those choices at compile time; a runtime role string alone is insufficient. Candidate-only native source files and their test targets are excluded from the Baseline application build. `host/native/contracts.hpp` and `bridge_contracts.hpp` are copied for mode parsing and optional offline harness support, not as replacement bridges.

The generated AOT corpus stays local and ignored. Both builds use the same unmodified generated input files and the same probe instrumenter. The latter makes build-local copies of affected generated units; its manifest identifies source/output hashes and certified entry/exit anchors. The original 355 overlay libraries may be shared read-only when their validated identity and compatibility are recorded, since the reusable runtime is unchanged; neither application should rebuild or silently substitute them during this staging step.

The Baseline CMake source-list audit found no missing application source in the explicit staging allowlist. The only source references absent from the staged tree are the three new native implementations/bridges and their unit-test files; the `MHP3RD_BASELINE_B0` branch excludes those targets and removes their six implementation files from `Yakumo`. The Baseline AOT harness guards its candidate-native calls and uses the copied common comparison header. The registered `test_offline_gate.py` CTest runs a self-contained synthetic repository with fake build targets; it does not require the omitted native binaries. The production OFF-006 offline gate is candidate migration evidence, not a Baseline build requirement.

## Preparation and acceptance evidence

After the Baseline compilation seams are present, stage a fresh source directory:

```sh
mkdir -p out/testing/observed-sources
python3 profiles/mhp3rd/tools/prepare_observed_baseline.py \
  --repo . --registration out/testing/baselines/B0 \
  --output out/testing/observed-sources/B0
```

The output must be new. The command fails if the registered archive differs, a source path is unsafe, a required B0 game file is absent or overlaid, the compile-time Baseline seals are missing, or the controller delivery loop no longer matches B0. `observed_baseline_manifest.json` in the output is the source ledger for that prepared tree; it is local-only. Re-stage to a new directory after observer changes. The command does not build, run, or package either application.

Before calling an application a paired Baseline, record and verify:

1. The staging manifest's pinned commit/tree/archive and all `origin: B0` file hashes, especially the retained parser/native files; a source diff against B0 contains only the reviewed observation allowlist.
2. Baseline build flags and actual compiled/link inputs: `MHP3RD_BASELINE_B0` is enabled, all five native replacement modes are compile-sealed off, new candidate bridge objects are absent, and original generated units or their certified observational copies are used. Record both build command/target and binary SHA-256.
3. The candidate and Baseline expose the same `recording_revision`, `observer_schema`, probe selection, case catalog hash and prerequisite basis, while their binary/source identities remain distinct. The recorder's revision hash covers its listed sources; the staging manifest provides the wider file-level audit for integration hooks.
4. Shared read-only ELF/ISO/overlay identities and independent writable save/run directories match the registered inputs. Never infer this from the archived historical executable or a role label.
5. Offline synthetic checks and build/link validation pass for both roles. No full-game boot, menu navigation, or screenshot is part of this initial gate; live case coverage remains for the user's paired session.

The observation callbacks and panel add work to the host, so this process establishes source provenance and comparable recorder semantics. It does not by itself establish gameplay equivalence or performance neutrality. Those are assessed from the later bounded, user-led paired cases.
