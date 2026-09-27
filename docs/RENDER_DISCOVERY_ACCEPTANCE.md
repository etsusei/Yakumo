# Render discovery: paired user observation

The user completed `RENDER-DISCOVERY-01` in both delivered applications on
Apple Silicon macOS. The paired recordings support a scoped **verified texture
decoder observation**: the Candidate's final, drained run-total counters show
132 cache-miss decode requests, all 132 successfully compared with the legacy
decoder on the same owned input, with no pixel mismatch, fallback or diagnostic
error. The user marked the village case normal in both roles. This is evidence
for the recorded route and observed inputs, not acceptance of every texture
format, visible surface, frame, or game area.

The case also covered three of the four optional vector metrics in Candidate
Verify mode. `vector_norm_squared` had zero calls in both roles, so it remains
unobserved in gameplay. The combined comparison's overall
`observed_difference` label is caused by differing manual input streams; its
renderer-specific outcome is `verified` and its case is eligible. No renderer
issue was found in these records. The user's waiver of a native-data Baseline
Auto-resolution rerun applies only to that earlier pair and was not used to
waive any render-discovery finding.

## Records and identity

| Role | Actual gameplay package | Mode | Delivered game binary SHA-256 |
| --- | --- | --- | --- |
| Baseline | `out/testing/runs/packages/run-74e201a8871a4681ab09aa104559a8df/` | Texture Off; all nine native helpers Off | `210c47867e82763ec1d7a3ab514dd515884e73280f36b23c92ae1d9cf7d1d0fe` |
| Candidate | `out/testing/runs/packages/run-562a09cfb4684142bdd2e69997ea98dd/` | Texture Verify; four vector metrics Verify; other five helpers Off | `8e1b91b661bcd755c2c964b555a29e291a2a87b571d32d02dfe851ca4b5ecdfa` |

These are the complete gameplay recordings for the `render-discovery` batch.
Preparation-only runs `run-e4965453fa484812bee06d03bb908ddc` and
`run-1d448480505e46599d68f634a25ce722` have no recording and were not
treated as user observations. Earlier vector-only and native-data packages
were not mixed into this comparison.

The current `load_package` validator rechecked each fixed artifact and raw
journal. Both recordings are complete, metadata-complete, and bound to their
launch contexts, with no package, journal, or case-lifecycle issue. Each
package's binary hash equals the actual `YakumoGame` file in its corresponding
`out/testing/dist/render-discovery/` app and the app's launch configuration.
The apps embed the same case and both profiles used for this analysis. The
Baseline gameplay source is `4292eb66ee66eab37c327575382d071addcf6249`;
the Candidate and observer source is
`f61ae346fc21f20539d965fd1b4d7052a5ccc9bd`. Both roles share the
registered game, ELF, overlay, initial save, build configuration, initial
settings, recorder revision, and case prerequisite identities. The
catalog hash is
`6245ba37859dbfa3634db50d4cf7b3adf84d085bfcf759065a5ac8d2424834a4`;
the vector and renderer profile hashes are respectively
`6677efb5d228a855d9d53be661378ad4185c2edf94ae8cd56edaeb057877e296`
and `9be309b0a2e00a1bfa9f1e89ec16b5399eaab74fbb26806fa5273f2313ba862e`.
There are no pair-compatibility or profile-mode issues.

Both roles recorded attempt 1 with the required
`render_observation_complete` checkpoint, normal user outcomes, and a fresh
`character_loaded: true` state sample at the checkpoint (zero guest-frame
age). Neither role recorded a configuration change, anomaly, diagnostic error,
recording loss, or checkpoint-state difference. The observed input streams
differed, as expected for manually controlled runs. That difference does not
establish a regression or frame equality.

## Renderer run-total evidence

Baseline Off emitted no active renderer counter. Candidate Verify emitted 63
cumulative samples, including exactly one final sample at journal sequence
19327. The final sample is last, has `workers_drained: true`, and follows the
case end at sequence 18769. Counter fields are valid and monotonic; no partial
or incomplete counter flag remains. The final values are:

| Counter | Candidate |
| --- | ---: |
| Requests / immediate / asynchronous | 132 / 0 / 132 |
| Asynchronous capture attempts / rejected snapshots | 132 / 0 |
| Unsupported state | 0 |
| Portable successes / verified comparisons / native commits | 132 / 132 / 0 |
| Fallbacks / pixel mismatches / errors / legacy failures | 0 / 0 / 0 / 0 |
| Portable / legacy reference instrumented elapsed duration | 45,614,749 ns / 24,366,707 ns |

All 132 requests partition into asynchronous requests, portable successes,
and same-input Verify comparisons. There is no partial mismatch observation.
These counters cover renderer cache-miss decodes over the **whole recorded
run**, including startup before the village case. They do not name a visible
object, source texture, pixel format, or all drawing. Verify returned the
legacy pixels while checking the portable output. These steady-clock elapsed
durations can include scheduling, and the reference path includes its snapshot
copy. They are not an isolated comparison of production performance or a
speedup claim.

## Optional vector case-window evidence

The vector profile requires no native execution and no vector calls for case
completion. Its `native_execution: not_covered` result is therefore expected.
The following deltas are bounded by the case markers, unlike the renderer
totals. All observed calls are certified and completed; each Candidate call
used Verify, with zero fallback, incomplete scope, orphan exit, or return
mismatch. Instrumented durations include observation overhead and are not a
speed comparison.

| Metric (entry) | Baseline AOT calls; duration | Candidate Verify calls; duration | Coverage |
| --- | ---: | ---: | --- |
| Vector norm (`0x08877244`) | 7,070; 1,234,596 ns | 6,530; 19,485,692 ns | Observed and verified |
| Vector norm squared (`0x08877264`) | 0; no duration | 0; no duration | Not covered |
| Vector distance (`0x08877280`) | 44,949; 7,954,959 ns | 41,511; 159,703,670 ns | Observed and verified |
| Vector distance squared (`0x088772A8`) | 2,814; 1,324,502 ns | 2,457; 10,314,964 ns | Observed and verified |

The case report's `reference_verification: not_covered` reflects that the
catalog has no *required* helper probe; the separate optional vector profile
reports the three actual Verify observations. Norm-squared remains a coverage
gap for a future related case with a real trigger. No replay of this village
route is implied by zero calls.

## Reproduction and limits

The local, ignored report is
`out/testing/render-discovery-user-acceptance/report-20260927/report.json`
(SHA-256
`b19fd8a2cff2b7c9c4915ffd71eef3219786970038a805947a338fa4c82f3846`);
the same directory has a readable `index.html`. It was generated with the
current `profiles/mhp3rd/tools/compare_test_runs.py`, using both gameplay
packages above, `profiles/mhp3rd/testing/cases/render_discovery.json`,
`--execution-profile profiles/mhp3rd/testing/profiles/render_discovery_vectors_v1.json`,
and `--renderer-profile profiles/mhp3rd/testing/profiles/render_discovery_texture_v1.json`.
The report itself retains complete package validation, profile results,
case-window deltas, and diagnostic summaries without changing the immutable
source recordings.

No Native texture mode, all-format coverage, exact frame or route equality,
rendered-image pixel comparison, gameplay performance gain, animation, AI,
combat, quest, multiplayer, or whole-game acceptance was verified here.
