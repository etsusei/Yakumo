# Native-data short-case review

Reviewed on 2026-09-27 for `NATIVE-DATA-01` version 1, using the delivered
`native-data-v1` execution profile and catalog hash
`a1f1aa5cabf652fc2bd150ec205483c3d39d61e4dfd455bb288263ee41d94b76`.
The user completed both gameplay roles. This review retains that completed case
evidence while leaving strict native execution acceptance pending a matched
case configuration.

## Runs and validation

| Role | Run ID | Source | Binary SHA-256 |
| --- | --- | --- | --- |
| Baseline | `run-1e209ecac64848dca9123b100258a852` | `4292eb66ee66eab37c327575382d071addcf6249` | `d9a45bd1c9546307892734fa1b5cafc5afa7bbaef14c09ba596abada3a25bdb5` |
| Candidate | `run-a0f498b6550f46c499f47df247e9a37f` | `73abb64af0c6b4a3931ee687f8b5f508044df803` | `a31698792cc77a7237229c04f0a7fc34df3f03efd5bdd5498f7248f259620633` |

Both packages match their delivered application launch configurations for source,
binary, initial configuration, and starting-save identities. Their launch
contexts match on game, ELF, overlay, starting save, initial configuration,
build flags, case catalog, and Apple Silicon macOS platform. Both have bound
identities, complete journals, no writer loss or diagnostics, and a supervised
normal menu exit. Baseline recorded all five replacements off; Candidate
recorded scale and matrix copy native and the other three off, as declared by
the profile.

Each role recorded one complete attempt, the `native_path_observed` checkpoint,
a fresh `character_loaded=true` sample at that checkpoint, and a normal user
outcome. Neither role recorded a user anomaly or a configuration change during
the case. The marks record the user's outcome; they are not a deterministic
comparison of gameplay streams.

The independently validated in-case counter windows are:

| Required helper | Baseline original AOT / completed | Candidate native / completed | Window health |
| --- | ---: | ---: | --- |
| Scale matrix `0x08878B28` | 185,039 / 185,039 | 166,586 / 166,586 | Certified and complete in both roles |
| Matrix copy `0x08879D08` | 3,504 / 3,504 | 3,180 / 3,180 | Certified and complete in both roles |

All four windows have zero fallback, Verify, incomplete, uncertified, orphan,
or return-mismatch counts. The Baseline windows have zero native calls; the
Candidate windows have zero AOT calls. Both meet the one-call minimum. These
counts establish which path executed in each separate manual run, but do not
prove same-input equivalence or a speedup.

Each journal has three `probe.detail_gap` markers for bounded recent-detail
retention. These are not recorder drops: both final observer-health events
report zero dropped or invalid events and no I/O error. The certified cumulative
counter windows remain complete; individual call-detail history is not
complete.

## Acceptance gate

The profile-bound comparison reports overall `incomparable` and
`native_execution: not_covered`. The case prerequisite hashes differ:
Baseline `72f517e2e22fd6492b92496d99234d69b92aab04cb10ba5bf5ebad32992fd204`;
Candidate `3d4656a2810ffa72d57e069c7b95ff3902e87f50c16698f04cc9083e742ce603`.
The comparison's finding is `case_prerequisites_or_version_differ`; both case
versions are 1, so the differing case configuration is decisive here.

The last effective-settings snapshots before CaseBegin contain 73 comparable
settings fields. Exactly one differs: `video.internal_scale` was `2` in
Baseline and `auto` in Candidate. Both started at `2`; Candidate changed it
to `1` and then `auto` before beginning the case. The case prerequisite digest
includes the live case configuration, so the strict pilot must not accept this
pair despite clean, exclusive call counts. The manually observed input streams
also differ, as expected for separate gameplay runs.

One new **Baseline-only** case can be paired with the existing Candidate
package. Launch the same unmodified Baseline application so the registered
initial configuration and fresh copied starting save remain bound. In the
application, set internal scale to `auto` **before** beginning
`NATIVE-DATA-01`, then use the same hunter and finite route, record the
checkpoint and normal outcome if observed, and close normally. Do not edit the
stored launch configuration before starting: that would change the launch
context and prevent comparison with the existing Candidate. The comparison
tool pairs role packages by case ID and attempt, without requiring adjacent
run times. The new package still needs its own clean validation, matching
prerequisite hash, complete certified windows, and a fresh profile-bound
comparison before acceptance.

Local-only evidence is under `out/testing/native-data-user-acceptance/`:
`report-20260927-1713/report.json` and `index.html` are the unmodified
comparison output; `counter-and-settings-evidence.json` records the independent
window extraction, delivered-binary checks, and pre-case settings diff. No
applications, source game data, saves, or previous run records were changed.
