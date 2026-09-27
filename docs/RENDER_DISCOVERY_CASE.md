# Combined render and vector discovery case

`RENDER-DISCOVERY-01` version 1 combines the pending vector observation route
with the portable texture decoder's first live verification batch. Both roles
use one finite village case, the first occupied hunter from independent copies
of the registered save, the same device and unchanged equipment/settings.
This prepares user-led observation; it does not boot or accept the game.

The catalog is `profiles/mhp3rd/testing/cases/render_discovery.json`, SHA-256
`6245ba37859dbfa3634db50d4cf7b3adf84d085bfcf759065a5ac8d2424834a4`.
The panel translates every title, step and checkpoint into Chinese. The route
keeps the existing bounded W/D/J movements, adds a stationary inspection of
the hunter, equipment, village surfaces and text, and ends with the single
`render_observation_complete` checkpoint plus the user's outcome. Do not retry
the route just to produce calls, force past a wall, enter a quest or change
equipment. The combined two-role duration is an estimated 5–10 minutes.

## Separate policies and evidence scopes

The vector profile `render_discovery_vectors_v1.json` uses the existing v2
helper registry: Baseline keeps all nine switches off; Candidate enables
Verify for the four vector metrics and leaves the original five helpers off.
No native helper execution is requested, and there are no required vector
calls. Actual case-window observations remain optional discovery evidence;
zero calls cannot become a reference-match or gameplay-acceptance claim.

The renderer profile `render_discovery_texture_v1.json` uses
`yakumo-renderer-batch-v1`, selects Candidate Verify, requires at least one
observed decode, and explicitly declares `run_total` coverage. Baseline keeps
the portable texture decoder off. The renderer totals include loading before
the case; they cannot identify which visible object or format was exercised.
Verify returns legacy pixels while comparing both decoders on the same owned
input. A future Native profile can establish execution but cannot claim a
same-input reference match from native-only counts.

Both policies bind the same catalog. Renderer policy hashes are bundled in
both apps, passed by the launcher, echoed by early preflight, and recorded in
RunBegin. Missing/mismatched echoes reject delivery preparation. Historical
configurations and packages remain unchanged; a helper-only comparison still
rejects an active renderer experiment without the explicit renderer profile.
New renderer-profile pairs seed Auto internal resolution in both roles from
startup, retaining the verified game font. Do not mix these records with the
earlier native-data case or change that earlier pair's launch configuration.

## Conservative report rules

`compare_test_runs.py` accepts both `--execution-profile` and
`--renderer-profile`. The renderer section checks role/profile/catalog binding,
complete recordings, matched case prerequisites, normal user outcomes and
checkpoint state. Different manual input streams alone are not a regression;
settings changes, anomalies or diagnostic errors still require review.

Candidate must supply exactly one final, drained counter record after any
periodic samples. All counter fields must be valid and monotonic. Intermediate
atomic samples can contain in-flight work, so partition equality is checked
only at finalization. Missing finalization, counter resets, incompatible
policy or zero coverage never pass. Fallbacks, errors, rejected/unsupported
input and legacy failures remain visible; pixel mismatch is reported
separately. Partial last counters are retained for diagnosis without promoting
an incomplete recording to acceptance. CPU duration totals are not a speedup
measurement.

The report preserves independent optional vector coverage and renderer
run-total evidence. A mismatch in the requested helper mode profile also
invalidates the combined policy; unobserved optional vector calls do not.

## Delivery state

The matched, signed pair built from `f61ae34` is ready under the ignored
`out/testing/dist/render-discovery/` directory, with a Chinese `START_HERE.md`.
Both actual launchers passed headless preparation with independent copied
saves, unchanged source inputs, matching case/configuration/observer identities,
and the expected profile hash and modes. No recording or gameplay was started.
Each app's prepared settings also passed all 43 game-font samples, including
the 23 formerly missing characters. Fifty real-binary preflight checks, 123
targeted Python tests, three related CTests, 758 translation checks and the
compiled catalog hash check passed on Apple Silicon macOS.

The renderer profile hash is
`9be309b0a2e00a1bfa9f1e89ec16b5399eaab74fbb26806fa5273f2313ba862e`;
the vector profile hash is
`6677efb5d228a855d9d53be661378ad4185c2edf94ae8cd56edaeb057877e296`.
Archived build manifests are under
`out/testing/observed-builds/render-discovery-f61ae34/`.
Readiness evidence is `out/testing/render-discovery-readiness.json`.

All prior app bundles and records remain unchanged. The current native-data
Baseline-only rerun comes first; this ready batch then replaces the need for
a separate, unplayed vector-only discovery route. VEC-003 remains pending
actual user observations. Preparation is not acceptance.

No live renderer, vector-trigger, performance, animation, AI, combat, quest,
multiplayer or whole-game acceptance is asserted by preparation tests.
