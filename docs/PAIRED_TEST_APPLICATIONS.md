# Local paired Mac test applications

This is the PAIR-002 delivery contract. Implementation progress and actual validation are recorded in [tasks.json](tasks.json); this guide does not assert that a final pair has been delivered or gameplay has passed.

The pair consists of two distinct applications built for the current Apple Silicon Mac. Baseline B0 uses the registered source plus reviewed observation changes; the candidate uses the migration checkout. Both share the same recorder, case panel, build profile and case catalog. See [the B0 source boundary](BASELINE_OBSERVATION_BUILD.md), [case-panel contract](TEST_SESSION_PANEL.md), and [comparison semantics](RUN_COMPARISON.md).

## Build and identity

Build through `cmake --build`, with low parallelism and one build per directory. Both roles use `MHP3RD_RELEASE=ON`, `MHP3RD_DEBUG_MENU=OFF`, renderer enabled and certified AOT probes enabled. Keep their dependency versions and other build options equal. Release mode excludes the state-changing debug tools; the observational test panel remains available when the supervisor supplies a catalog.

For B0, set `MHP3RD_BASELINE_B0=ON`, `MHP3RD_GAMEPLAY_SOURCE_COMMIT` to the full registered B0 commit, and `MHP3RD_BASELINE_PROVENANCE_SHA256` to the prepared source manifest's `source_content_sha256`. All five native mode environment variables must be absent, `off`, or `0`; other values are rejected before game setup. Candidate-only native implementations are omitted from the Baseline application.

The `--test-preflight` application argument requires an explicit isolated `MHP3RD_DATA_DIR`. It reports nine identity/capability fields as one JSON object, before asset lookup, Runtime construction, SDL setup, or guest execution. It reads effective settings without saving them. No game inputs are required. The separate `verify_pair_preflight.py` gate checks both binaries, shared metadata, source-audit binding, native-mode rejection, and unchanged temporary settings. These checks do not prove gameplay or rendering behavior.

Each build manifest has schema `yakumo-observed-build-v1` and these fields:

| Field | Meaning |
| --- | --- |
| `role` | `baseline` or `candidate` |
| `gameplay_source_commit` | Full commit used for the role's gameplay source |
| `observer_commit` | Common checkout commit containing observation changes |
| `build_config_sha256` | Build profile emitted by that binary's preflight |
| `recorder_revision` | Observation-source digest emitted by preflight |
| `baseline_sealed` | True only for the B0 binary |
| `executable`, `executable_sha256` | Absolute local binary path and actual file digest |
| `baseline_provenance` | B0 audit `{path, sha256}`, or null for candidate |

Keep manifests local, alongside their build evidence. Do not invent source identities for a dirty build or reuse the historical archived binary as B0.

## Packaging

`profiles/mhp3rd/scripts/package_test_pair.sh` accepts the following required arguments: `--baseline-build`, `--candidate-build`, `--registration`, `--overlays`, `--cases`, `--launcher`, `--python`, and `--output`. The launcher is built separately as a small native executable. Optional `--moltenvk` and `--font` arguments select installed local resources. `--game-font` selects the common game-text face, defaulting to the previously verified macOS STHeiti Light face. The same choice must seed both binary preflight and launch settings; interface language alone does not select a Chinese game font. The font path and content digest are retained in the pair manifest. Output must be a new directory under ignored `out/testing/dist`.

The assembler checks the registered original ISO, ELF and starting-save snapshot; verifies both build identities through preflight; copies the actual recursive Mach-O dependency closure; rewrites library links; copies relevant installed library licenses; and ad-hoc signs the two bundles. This uses the installed FFmpeg ABI, not an older release bundle's libraries. The 355 registered overlay libraries are reused without rebuilding their game code.

Each application's entry point is `Contents/MacOS/YakumoTestLauncher`; the actual game executable is `YakumoGame`. Resources contain the supervisor, comparison tools, catalog and launch configuration. Only read-only paths to original game inputs are recorded. The ISO, ELF and saves are not copied into the applications.

The local launcher uses an installed Python interpreter pinned by path and SHA-256. Consequently this is a delivery for the current development Mac, not a notarized or portable standalone distribution. An interpreter update requires rebuilding the local pair configuration. The binaries and all local manifests remain ignored by Git.

## Run lifecycle and evidence

The native launcher starts `launch_test_run.py` without a shell. The supervisor validates configured fingerprints, creates a fresh run directory, copies the starting-save payload into its own writable installation, seeds settings and calls binary preflight. Only after successful preparation does an ordinary launch start the game. Each role and run uses a different save directory; originals are never restored over a running process.

The inherited `MHP3RD_*`, `PSPRECOMP_*` and `DYLD_*` variables are removed before applying the manifest's approved environment. Baseline modes stay off. The initial candidate packaging selects verify mode; verification retains the original result while comparing supported same-input helper outcomes. It does not measure native speedup.

The supervisor captures bounded stdout/stderr, waits for the user to finish, and preserves the journal's valid prefix after abnormal child termination. A normal window close uses exit code 4 with a matching stop reason; code 5 means recording finalization failed. Missing, corrupt or truncated evidence remains explicit. No user session has the agent's short smoke-test timeout.

| Result status | What it establishes |
| --- | --- |
| `prepared` | Preparation and binary preflight succeeded; the game was not launched |
| `completed` | The recording is complete with valid metadata and identity binding; game cases still need analysis and human acceptance |
| `incomplete` | Some evidence may have been recovered; inspect errors and missing data |
| `preflight_failed` / `invalid_launch` | Preparation or launch validation failed; do not claim a test run passed |

For offline integration, use the native launcher's `--headless --prepare-only` options. This verifies installation and collection plumbing without opening the game or any dialog. Lifecycle failure tests use synthetic child processes. Full game startup, physical controls, final dialog appearance and live case coverage belong to the later user-led acceptance batch.

After an ordinary run, the launcher offers a results-folder action. The supervisor's separate `--result-directory` query validates the result file's run identity before returning a path. The application does not upload records. A saved record is ready for comparison; it is not a claim that animation, AI, collision or feel matches B0.

## Recorded offline delivery check

On 2026-09-27, implementation commit `4759d99` was built and checked on Apple Silicon macOS 27.0. Both actual signed applications were assembled; their native entry points completed `--headless --prepare-only` against the registered inputs. Their initial save contents matched, their writable files/directories were independent, and original inputs were unchanged. The supervisor retains `result.json` beside each run's evidence after the launcher's temporary handoff is removed.

The gate passed 32 component tests, 21 real-binary preflight checks and both 1,280-call production AOT leaf checks. Local evidence is in `out/testing/paired-preflight-validation.json`, `out/testing/paired-preparation-validation.json`, and `out/testing/dist/pair-manifest.json`. No game was launched and no dialog was displayed. PAIR-003 still owns the final concrete case catalog and user handoff; the current packaged catalog is provisional.

The subsequent formal handoff at commit `75ca5ff` completed PAIR-003. Its apps
are under `out/testing/dist/initial-batch/`, with the local Chinese guide
`START_HERE.md`. The final readiness report is `out/testing/readiness.json`.
Use [the formal case pack](TEST_CASES.md) for CASE-001; the earlier pair above
is retained as provisional delivery evidence. The first user sessions have now
been analyzed in [the scoped acceptance report](FIRST_PAIRED_ACCEPTANCE.md);
broader gameplay acceptance remains pending.

A later user-reported shared Chinese glyph regression was fixed in the local
`out/testing/dist/font-fixed/` copies. See [the font regression report](GAME_FONT_REGRESSION.md)
for reproduction, coverage and preserved historical artifacts. These copies
use a different effective font configuration from the initial-batch records.
