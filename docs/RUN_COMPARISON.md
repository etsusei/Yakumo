# Local run packages and paired comparison

OBS-004 implements a bounded offline reader, a fixed local evidence package,
case reconstruction, and standalone JSON/HTML comparison reports. It does not
launch the game, operate menus, upload data, or assert whole-game acceptance.
The Chinese case panel and automatic supervising launcher are PAIR-001/002.

## Launcher context and journal binding

The launcher prepares a JSON context before starting either application. Its
schema is `yakumo-run-context-v1`, with these fields:

| Field | Meaning |
| --- | --- |
| `run_id` | Unique run identity, matching RunBegin and supervisor metadata |
| `game_sha256` | Hash of the actual read-only disc image |
| `elf_sha256` | Hash of the executable actually loaded by the runtime |
| `overlay_sha256` | Hash of the canonical overlay identity inventory |
| `starting_save_sha256` | Hash of the immutable starting-save manifest |
| `config_sha256` | Hash of canonical relevant gameplay/input configuration |
| `build_config_sha256` | Hash of compiler/build-feature configuration |
| `case_catalog_sha256` | Hash of the canonical finite case catalog |
| `source_commit` | Actual source revision; retained even when roles differ |
| `platform_os`, `platform_arch` | Platform facts used for comparison |

Hashes are SHA-256 hexadecimal strings. Canonical JSON uses sorted object keys,
compact separators, UTF-8 without ASCII escaping, and no nonfinite numbers:
`json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False,
allow_nan=False)`. A complete launch context contains every field. For
inspection of incomplete legacy records, the packager represents missing
context fields as null before computing its digest; those records remain
ineligible for a passing comparison.

The launcher passes this context digest as `MHP3RD_RECORD_CONTEXT_SHA256`.
RuntimeRecording validates the 64 lowercase hexadecimal characters and writes
the value to RunBegin before publishing its observer. Packaging requires the
provided context to match that digest. Runs without this setting remain useful
for inspection but are explicitly unbound for paired comparison. A digest
binds records to supplied metadata; it is not independent proof that a launcher
measured its inputs correctly or a cryptographic authenticity signature.

Existing `MHP3RD_RECORD_DIR`, role/run/batch/Baseline identifiers and native
mode metadata remain defined by [GAME_OBSERVATION.md](GAME_OBSERVATION.md).
The launcher, not the user, will calculate actual input/configuration identities.
No claim of complete input identity is made for historic logs that lack them.

After exit, the launcher supplies `yakumo-supervisor-v1` JSON with `run_id`,
`status` (`exited`, `signaled`, or `unknown`), `exit_code` (integer for exited,
otherwise null), and `stop_reason`. A completed journal and matching supervisor
reason are both required. Exit 0 corresponds to `guest_finished`; exit 4 is
normal only for `window closed` or `quit from the menu`. Signals, missing
supervision, unexpected exit reasons, and incomplete writer health cannot pass.
If final recorder closure fails, the application returns exit code 5 instead
of the ordinary close code. This also covers failures detected after RunEnd
was written; a supervisor must not infer durable completion from that record
alone or from the absence of a crash.

## Package format and integrity

The packager accepts one run directory containing `events.journal`, the explicit
context file, and optional supervisor metadata. It publishes only:

- `manifest.json`: version, packager revision, identities, context, supervision, artifact sizes and hashes;
- `events.journal`: unchanged journal bytes, copied read-only;
- `events.jsonl`: independently decoded valid records;
- `statistics.json`: event counts and latest cumulative probe summaries;
- `markers.json`: case/checkpoint/anomaly records;
- `diagnostics.jsonl`: structured errors and recording-loss records;
- `summary.json`: framing, health and coverage limitations.

ISO data, saves, memory dumps, arbitrary stdout files and unrelated files in the
run directory are excluded. No recursive collection is performed. All supplied
path components must be real paths without symlinks; use canonical paths for
system aliases such as macOS temporary directories. Inputs remain untouched,
outputs must be new and outside the input run, and staging is published with
an atomic no-replace rename. The tested platform is macOS; Linux/Windows
publication branches remain unverified here.

The independent reader validates v1 framing, CRC, sequence, UTF-8, flat scalar
JSON, integer ranges and lifecycle. Bounds are 256 MiB per raw journal,
1,000,000 records, 65,536 bytes per payload/metadata file, and 512 MiB per
derived artifact. It stops at the first bad tail and retains the valid prefix;
it never searches for a later plausible frame. Host timestamps need not be
globally increasing across concurrent producers.

Package loading verifies fixed artifact names, lengths and hashes, re-reads the
raw journal and recomputes derived artifacts and health. Editing summary or
manifest flags cannot turn incomplete evidence into a pass. Writer loss,
invalid events, observer failures, multiple/nonfinal health records and
contradictory supervisor metadata remain explicit. Metadata compatibility and
recording completion are separate properties.

The context executable hash must also agree with the runtime's own
`runtime.inputs` event and the supported executable identity. Missing,
unsupported or contradictory runtime input observations are not eligible for
a passing comparison, even if the supplied context is internally consistent.

## Case catalog and event contract

The `yakumo-case-catalog-v1` catalog contains a `cases` array. Each case has:

```json
{
  "id": "NATIVE-01",
  "version": 1,
  "title": "Observe a selected helper",
  "steps": ["Perform the specified route", "Mark the checkpoint"],
  "checkpoints": ["checked"],
  "required_probes": [{"entry": 143095832, "min_calls": 1}],
  "required_state_fields": ["health_current"],
  "human_acceptance": true
}
```

This is a format example, not a promised route that triggers the vector helper.
Actual trigger cases must be validated in the first user-led case pack. Catalogs
are bounded to 128 cases, 128 steps/checkpoints per case and 32 required
probes/state fields. Unknown keys, duplicate IDs and ambiguous scalar types are
rejected. Normalized catalog content determines its hash.

All case markers carry `case_id`, `case_version` and positive integer `attempt`.
Attempt numbers are scoped to a case and paired explicitly across roles; a
missing counterpart remains missing rather than being silently aligned with
an unrelated retry.

| Journal kind | Additional payload |
| --- | --- |
| CaseBegin | `prerequisites_sha256`, computed from that case's relevant starting conditions |
| Checkpoint | `checkpoint_id`, in the catalog's declared order |
| Anomaly | Optional user `message`; preserved as a review requirement |
| CaseEnd | `outcome`: `normal`, `abnormal`, `uncertain`, or `skipped` |

Case markers are sequential and nonoverlapping. Interrupted, malformed,
unknown, duplicate or overlapping attempts remain visible. Missing checkpoints
and explicit skips are not normal completion. Closing the window never invents
a normal CaseEnd for an unfinished case.

The panel must follow this order on the emulation thread:

1. Configure the selected probes and emit CaseBegin before allowing the case's operations.
2. Call `flush_native_probes(false, "case_begin")` to capture the starting counters.
3. Record ordered checkpoints and user anomalies. Call `flush_native_probe_detail()` at these markers to preserve recent detail.
4. Call `flush_native_probes(false, "case_end")` before CaseEnd; then record the user's explicit outcome.

The implemented panel/controller and launcher binding recipe are described in
[TEST_SESSION_PANEL.md](TEST_SESSION_PANEL.md). It samples guarded state at
checkpoints and leaves unfinished application-close cases interrupted.

Both tagged summaries must have the same `counter_epoch`. It changes whenever
a new probe session is configured. Comparisons subtract cumulative counters;
they never sum summaries or assume that a single row describes one case.
The accounting must have no open scopes at either boundary, preventing calls
crossing the boundary from canceling into apparent in-case coverage. Minimum
calls, valid spans and complete scopes are required separately for both roles.

## Evidence interpretation

For a declared native-execution batch, pass `--execution-profile` to the
comparison command. `native_batch.py` validates the bounded scale/copy pilot
against the canonical catalog. The report adds `native_execution`, checking
the actual recorded mode map, certified complete case windows, exclusive
Baseline AOT/candidate native counts, clean diagnostics/settings and normal
user marks. Its `observed` result establishes that the requested native paths
ran under those conditions; `reference_verification` remains `not_covered`
without an in-process reference. Manual-stream differences stay visible in
the ordinary comparison. See [the finite next batches](NEXT_NATIVE_BATCHES.md).

Package prerequisites include matching game, overlay, save, configuration,
build flags, catalog, recorder semantics, platform and Baseline identity. Run,
source revision and binary identities remain distinct where expected. Every
Baseline native mode must be off. Unknown modes and unbound contexts are not
compatible evidence.

Reports distinguish:

| Result | Interpretation |
| --- | --- |
| `confirmed_mismatch` | A supported selected helper emitted an explicit certified same-input reference mismatch during the candidate case |
| `observational_match` | Required paired observations are present without a confirmed regression; this is not deterministic replay |
| `observed_difference` | Manual input or periodic state differs; investigation is needed, not an automatic bug claim |
| `incomparable` | Version, identity, prerequisite or context binding differs/is missing |
| `not_covered` | Required case, trigger, checkpoint, probe calls or state is absent |
| `incomplete` | Journal, process, case lifecycle or scope evidence is incomplete |
| `inconclusive` | User uncertainty/skip/anomaly, diagnostics, or insufficient observable evidence prevents a conclusion |

Within each case, `reference_verification.outcome = same_input_match` describes
successful in-process original-code checks for all required candidate helper
calls. It is separate from paired route equality. Different manually recorded
inputs never become a claim of identical complete state or random events.
Counts or durations differing between manually played sessions do not prove
a bug. Generic probe failures cannot be relabeled as value mismatches:
`native.verification_mismatch` is emitted only by the actual comparison-failure
branches, with a fresh code-span check. Uncertified failures remain diagnostics.

State comparisons use the last valid supported sample since the previous
checkpoint. It must be no more than 1.5 host seconds old and, when both guest
frame ordinals are available, no more than 30 guest frames old. These are
periodic observations near a checkpoint, not its exact state. Missing, stale,
unavailable or unsupported fields do not count as covered.

Performance reports contain sample counts and medians as diagnostics only.
Errors from either role, errors outside cases, unknown attempts and retained
user markers remain visible. Configuration changes during a case require review;
the initial configuration hash cannot silently authorize changed conditions.
HTML escapes dynamic content, has no external
resources or scripts, and uses a restrictive content-security policy.

## Commands and offline checks

These commands are intended for the supervising launcher and development
automation. The user's eventual workflow remains opening each test app,
performing its finite cases and closing the window.

```sh
python3 profiles/mhp3rd/tools/run_package.py \
  --run-dir out/testing/runs/run-1 --context out/testing/context-1.json \
  --supervisor out/testing/supervisor-1.json --output out/testing/packages/run-1
python3 profiles/mhp3rd/tools/compare_test_runs.py \
  --baseline out/testing/packages/baseline-1 --candidate out/testing/packages/candidate-1 \
  --cases out/testing/cases.json --output out/testing/reports/batch-1
```

A successfully generated package/report returns zero even if its evidence is
incomplete or shows a mismatch. Automation must inspect the JSON outcome;
nonzero exit indicates the tool could not perform the requested operation.
Reports contain `report.json` and a standalone `index.html`; existing outputs
are never overwritten and source packages are never used as output folders.
The JSON report identifies its comparison-tool source revision. Destination
parent directories must already exist.

Synthetic Python suites cover package integrity, recovery, lifecycle, coverage,
compatibility, input/state distinctions and HTML escaping. A C++ fixture uses
the real RuntimeRecording/SessionRecorder path to produce normal, mismatching,
abrupt-exit and recorder-error journals for independent Python packaging and comparison:

```sh
cmake --build out/resource-validation --target mhp3rd_run_package_fixture mhp3rd_runtime_recording_tests mhp3rd_probe_tests -j2
ctest --test-dir out/resource-validation -R '^mhp3rd_(run_package|run_cases|run_comparison|run_pipeline|runtime_recording|probe)_tests$' --output-on-failure
```

The fixture observations are deliberately synthetic; they are not real game
matches or gameplay acceptance. The task ledger records exact verification
evidence and remaining platform/live limitations.

On 2026-09-27 (Asia/Tokyo), Apple Silicon macOS passed six integrated CTests,
including 47 Python cases and the real-writer pipeline's four expected outcomes.
Three ASan/UBSan suites and two TSan suites passed. The production-object AOT
regression harness and full renderer-enabled application build also passed
without launching the game. Local evidence is
`out/testing/run-comparison-validation.json`; synthetic reports are under
`out/testing/reports/obs004-verified/`.

HTML content and escaping were checked automatically. Visual preview was not
verified: the available browser rejected local-file URLs, and no bypass was
attempted. Linux/Windows publication branches, actual gameplay collection,
paired application packaging and panel usability remain unverified or pending.
