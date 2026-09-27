# Native data execution pilot

NAT-002 delivers one finite case, `NATIVE-DATA-01` version 1. Its catalog is
`profiles/mhp3rd/testing/cases/native_data_batch.json`; its declared mode profile
is `profiles/mhp3rd/testing/profiles/native_data_v1.json`. The catalog hash is
`a1f1aa5cabf652fc2bd150ec205483c3d39d61e4dfd455bb288263ee41d94b76`.

Baseline keeps all five replacements off. Candidate requests native scale and
matrix copy, with angle, translation and vector construction off. Both keep
all five observation probes selected. This batch establishes actual native
execution in a bounded route; it does not repeat the prior in-process verify
mode or enable new default settings.

## User operations

Run Baseline, then Candidate, from their independently prepared copies of the
same registered save. Load the first occupied hunter in both, preserve its
equipment, and use the same device and settings. The restored Chinese game
font is part of the common configuration. Use the new native-data apps; older
font-fixed and initial-batch apps remain historical references.

After reaching the village, open Yakumo with Esc or L3+R3 and select Test
session (Q/W or L1/R1 changes sections). Begin the single case. Stand for three
seconds and inspect the hunter, equipment and Chinese text. Hold forward for
two seconds, release for two, then right for one second and wait three. Stop
before a wall or door, without forcing passage or retrying. Move the camera
left for one second. Open Test session and record Native path observed; reopen
it after the marker returns to gameplay and mark normal, abnormal, uncertain
or skipped. Finish the case before closing the game normally and waiting for
collection. The second role follows the same steps. The combined 5–10-minute
target is an estimate, not a measured duration.

The only checkpoint is `native_path_observed`; it requires fresh
`character_loaded` observation. Both scale and copy must have at least one
certified completed call inside the case in each role. Existing live records
already show both on the village route, but the new native case must establish
its own coverage. No combat, quest or networking trigger is requested.

## Delivery and analysis gates

Build both roles with matching observation revisions, use the explicit mode
profile, retain the installed game-font fingerprint, and run signed
preparation-only readiness. The same profile must be supplied to
`compare_test_runs.py --execution-profile` after the user finishes. Require the
separate `native_execution` verdict and inspect ordinary case findings too;
differing manual streams do not become deterministic replay. Native-only calls
must not produce a same-input reference-match claim.

Clean shutdown alone is insufficient: require all markers, normal user
outcomes, no diagnostic/configuration problem, complete certified windows,
exclusive Baseline AOT counts, and exclusive candidate native counts. Fallback
or zero calls cannot pass this strict pilot. The other three helpers remain
explicitly uncovered unless separately observed; this case does not accept
them. Runtime patches/mods and new default-native enablement are outside this
controlled batch.

## Recorded delivery

The local signed pair built from `73abb64` is ready under
`out/testing/dist/native-data/`, with its Chinese `START_HERE.md`. Nine relevant
CTests, 21 real-binary preflight checks, both production AOT harnesses and the
actual native launchers' preparation-only flow passed. Both prepared settings
also passed all 43 game-font glyph checks. `out/testing/native-data-readiness.json`
binds the declared execution profile and delivered catalog. No game was
launched during preparation. The user subsequently completed both cases;
[the acceptance review](NATIVE_DATA_ACCEPTANCE.md) records exclusive native
execution and a pre-case resolution mismatch. NAT-004 awaits only a matched
Baseline rerun, retaining the existing Candidate record and the mode-specific
evidence limits above.
