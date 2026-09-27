# Vector call discovery pair

This finite observation stage supports VEC-003. It is separate from the
scale/copy native-data pilot and does not replace that pilot or its records.
Finish the current pilot first. This stage discovers actual metric calls before
defining required metric gameplay coverage.

The catalog is `profiles/mhp3rd/testing/cases/vector_metrics_discovery.json`:
one `VECTOR-DISCOVERY-01` case, version 1, with eight short instructions and one
checkpoint. Both roles use the first occupied character in the registered save,
unchanged equipment, one input device and the verified game font. The route is
the familiar village stand/walk/camera sequence. Stop at a wall or door rather
than repeating the route or entering a new area. No quest or combat is required.

The profile is `profiles/mhp3rd/testing/profiles/vector_discovery_v1.json`:
`yakumo-native-batch-v2`, all nine modes explicit, old five off, and all four
metrics Verify in the candidate. Baseline keeps every mode off. The candidate
retains actual original AOT results while comparing native predictions. Both
roles select all nine observations so a zero count is retained as evidence.

## Human marks and function coverage are separate

The case has no required function calls and the profile has no required native
entries. This is intentional: no current evidence establishes that a specific
village action triggers these four metrics. The loaded-character state and
normal/abnormal/uncertain/skipped human mark still describe the observed case.
They do not establish metric coverage or native execution. Missing calls are
not a user mistake and do not require retrying or wandering to force a hit.

The comparison's optional profile observations inspect closed case counter
windows for each enabled metric, including entries not listed as required
probes. Separate results describe actual observed calls, valid in-process
verification, fallback-only hits, zero coverage and incomplete/incompatible
records. Native execution remains not covered for this Verify-only profile.
Different manual inputs and durations cannot establish deterministic equality
or a speedup. Only subsequent real evidence can justify required metric steps.

## Delivery boundary

Build both roles with the same observation revision and localized case panel.
Use fresh uniquely identified applications under a new `vector-discovery`
directory, explicit v2 manifests, identical source resource identities and
independent writable save copies. Preserve every earlier app and original save.
The signed native launcher's `--headless --prepare-only` readiness path must
pass, and the production font probe must still cover the reported Chinese text.
Preparation must not start a game, window, or recording session.

The local Chinese `START_HERE.md` gives the actual application paths and steps.
The user operates each role once and closes it normally. The assistant reads
the resulting local packages and reports which entries were observed, what was
verified, and what remains unknown. A readiness report is not user acceptance.
