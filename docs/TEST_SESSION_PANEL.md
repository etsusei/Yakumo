# In-game case panel

PAIR-001 connects the finite case protocol to the existing menu. The panel is
available only for a recording run with a supplied, bound case catalog. It does
not change game state, native replacement modes or saves. The normal game menu
is unchanged when no case session is configured.

## User workflow

The menu's **Test session** section is translated through the existing Chinese
catalog. It shows Baseline/candidate role, build version, recording health,
case instructions, checkpoint progress and the user's recorded outcomes.

1. Choose a case and read its steps.
2. Begin the case; the menu closes so gameplay can resume.
3. Reopen the section at each requested checkpoint. Recording a checkpoint or
   an anomaly returns to gameplay.
4. Finish explicitly as normal, abnormal, uncertain or skipped. Normal is
   unavailable until all required checkpoints have been recorded.

Closing the menu does not end a case. Closing the application with a case still
active records an interruption, without fabricating a normal CaseEnd. Results
marked normal are user observations; the report still checks coverage and
compatibility. A later real recording failure masks a normal label in the UI;
a clean session close does not invalidate an already finished case.

A compact noninteractive hint displays the active case and next checkpoint
over gameplay. It takes no game input. Catalog text is displayed unformatted;
only the reviewed first-pack instruction literals and checkpoint IDs have
Chinese translations. Unrecognized catalog text is preserved verbatim.

No cheat controls, file editing, terminal commands, or manual log assembly are
part of the panel flow. PAIR-002 and PAIR-003 now provide the signed local pair
and [finite first-case instructions](TEST_CASES.md). CASE-001 is the pending
user-led gameplay acceptance; the panel's offline checks do not replace it.

## Configuration and binding

In addition to the existing recording options, the launcher supplies all of:

| Variable | Meaning |
| --- | --- |
| `MHP3RD_RECORD_CASE_CATALOG` | Local JSON catalog file |
| `MHP3RD_RECORD_CASE_CATALOG_SHA256` | Canonical validated catalog hash |
| `MHP3RD_RECORD_PREREQUISITES_SHA256` | Shared input/configuration basis for the paired roles |
| `MHP3RD_RECORD_CONTEXT_SHA256` | Binding to the complete launch context |

Partial case configuration is rejected before starting recording. The C++
loader implements the same bounded `yakumo-case-catalog-v1` schema and canonical
UTF-8 JSON hash as the Python reader. It checks exact keys/types, duplicate IDs,
numeric limits, text/Unicode validity, size limits and regular-file input.
The actual catalog must match its supplied hash before publishing the panel.

`run_package.prerequisite_basis_sha256(context, baseline_id, baseline_commit)`
computes the shared basis. It hashes canonical JSON with schema
`yakumo-case-basis-v1`, the Baseline identity, and common game/ELF/overlay/save,
configuration/build/catalog and platform identities. Hash strings and the
commit are lowercase. Run ID, role, binary and source commit are excluded
because the two roles are intentionally distinct. Package validation checks
the declared basis and catalog hash when panel bindings are present; legacy
packages without these optional fields remain inspectable.

At case start, the controller combines that basis with its case identity and
the current settings fingerprint. The exact UTF-8 byte recipe, including its
final newline, is:

```text
yakumo-case-prerequisites-v1
<prerequisite_basis_sha256>
<case_id>
<decimal_case_version>
<current_configuration_sha256>
```

The current configuration hash covers formatted settings, excluding navigation
history (`ui.last_folder`, `ui.menu_hint_seen`) and per-instance network
identity/history (`network.mac`, `network.nickname`, `network.recent`). Changing
other settings during an active case interrupts it rather than silently
continuing under old prerequisites. This fingerprint is a statement about
registered inputs and known configuration, not a full game-state or RNG snapshot.

## Runtime and recording order

`CaseController` owns progress for the whole run, independently of a menu
instance. It validates supported probe selection and performs actions on the
emulation/UI thread. `case_runtime` connects its hooks to RuntimeDiagnostics,
the current settings digest, and probe summaries/detail.

Start selects observational probes, emits CaseBegin, then emits the tagged
`case_begin` snapshot. A checkpoint reads guarded state immediately before its
marker and flushes recent probe detail. Finish emits a tagged `case_end`
snapshot before CaseEnd. No native experiment switch is altered by these
operations. Unknown probe entries, recorder failure, callback failure and
configuration changes cannot create a successful normal outcome.

Case selection retains the launcher's `MHP3RD_RECORD_PROBES` mask and adds the
case's required probes. The paired launcher should select all helpers changed
by its batch in both roles, including optional discovery probes. A case without
required helper calls must not silently disable diagnostics for those changes;
zero-call optional probes still do not establish live coverage.

On normal exit, early bootstrap exit or a caught host exception, main closes
the case controller before diagnostics and the recorder. The controller's
destructor also interrupts unfinished work. Abrupt termination remains the
supervisor's responsibility. Marker reconstruction and comparison are defined
in [RUN_COMPARISON.md](RUN_COMPARISON.md).

## Offline verification

The tests cover strict C++ parsing, independent Python/C++ canonical hashes,
controller lifecycle/marker ordering, matching paired prerequisites, config
changes, interrupted closure, recorder failures and Chinese UI action states.
The UI test uses a real ImGui context with a built font atlas and widget/Layer
stubs; it does not open SDL, Vulkan or the game. Its assertions cover behavior
and text, not final pixel layout or controller ergonomics.

The runtime fixture exercises actual RuntimeRecording, RuntimeDiagnostics,
CaseController, settings hashing, local packaging and comparison together. Its
guest memory is synthetic and verified unchanged. Normal, interrupted,
configuration-change and skipped cases produce their expected distinct reports.

```sh
cmake --build out/resource-validation --target mhp3rd_case_catalog_tests mhp3rd_case_controller_tests mhp3rd_test_session_screen_tests mhp3rd_case_runtime_fixture -j2
ctest --test-dir out/resource-validation -R 'mhp3rd_(case_catalog|case_controller|case_runtime_pipeline|test_session_screen)' --output-on-failure
```

Live display, physical input behavior, trigger coverage and usability remain
pending for user-led acceptance. No game was started for these checks.

On 2026-09-27 (Asia/Tokyo), Apple Silicon macOS passed twelve integrated CTests,
five ASan/UBSan suites and one TSan suite. The renderer-enabled application
compiled and linked. Exact source/binary/log identities and the four pipeline
outcomes are recorded in local `out/testing/test-panel-validation.json`;
synthetic reports are under `out/testing/reports/pair001/`.
