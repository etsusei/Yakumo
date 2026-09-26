# Certified leaf probes and read-only diagnostics

OBS-003 extends the shared [game observation journal](GAME_OBSERVATION.md). It
observes five identified math leaves, periodic game-state samples, and the
existing performance summary. It does not identify every gameplay function or
establish animation, combat, AI, or multiplayer correctness.

## Selection and lifecycle

Recording remains off unless `MHP3RD_RECORD_DIR` and its required run metadata
are supplied. `MHP3RD_RECORD_PROBES` is additionally empty or `off` by default.
Use `all` or a comma-separated subset of `angle,scale,translation,vector,copy`.
Unknown, repeated, or empty list entries are rejected before recording starts.
The forthcoming case panel selects probes for each case; this environment
setting supplies an explicit selection for diagnostic runs in the meantime.

The supported whole-ELF identity gates probe activation and interpreted state
fields. Every selected function entry additionally hashes its complete current
instruction span, including return delay slots. A changed span records an
uncertified entry without changing execution. No executable bytes are embedded
in the probe implementation.

`RuntimeDiagnostics` samples state at most once per host second, at a guest
vblank boundary, and emits each new valid performance-summary second once.
Closing or unwinding the diagnostics session finalizes probe evidence before
the journal closes. Unfinished scopes remain incomplete. Abrupt process death
still requires the supervisor planned in PAIR-002; unwritten data is not
recoverable merely because an in-process destructor exists.

## Function boundaries and evidence

CMake discovers the five entry registrations in the original generated corpus,
after the camera helper lookup. It instruments build-local copies of affected
units and leaves the source corpus untouched. The current corpus needs two
copies. Source-shape checks reject missing/duplicate entries or changed return
structures. `MHP3RD_CERTIFIED_PROBES=OFF` builds without these AOT callbacks;
the run metadata records whether the boundaries were compiled.

An AOT entry is recorded after its instruction label. Its exit is recorded
after the return delay-slot effect and before local dispatch continues. This
preserves original AOT execution, including direct chaining. The entry address
and return target are explicit because the context PC may still refer to an
earlier dispatch. Matching requires the recorded runtime, context, host thread,
and entry/return context; mismatches cannot create a successful timing sample.

The candidate bridges record the path actually used:

| Variant | Measured work |
| --- | --- |
| `aot` | Original compiled leaf between matched boundaries |
| `native` | Accepted native bridge execution |
| `verify` | Prediction and original-code comparison together |
| `fallback` | Original interpreter reference used by a bridge |

Verification and fallback durations must not be presented as native speedups.
Entry hits, certified entries, completed scopes, rejected spans, unmatched
returns, and incomplete scopes are separate counters. Zero calls means
`not_covered`; partial or uncertified evidence is not a passing game case.

`probe.summary` counters are cumulative since selection/configuration, not
per-second deltas. Each variant has a completed-call count, total duration and
maximum duration in host monotonic nanoseconds. Consumers use the latest row
or calculate deltas; they must not sum cumulative rows. Scope timing excludes
the per-entry fingerprint calculation but includes part of the observation
bookkeeping and is not an instrumentation-free measurement.

OBS-004 adds a stable per-session `counter_epoch` and optional `boundary` tag.
The case panel must use `flush_native_probes(false, "case_begin")` and
`flush_native_probes(false, "case_end")` inside the case boundaries; see
[RUN_COMPARISON.md](RUN_COMPARISON.md) for closed-scope accounting requirements.
Actual native comparison failures additionally emit
`native.verification_mismatch`, with a fresh full-span certification result.
Reference aborts and generic incomplete scopes are not this event and must not
be treated as confirmed unequal results.

Scope tracking is bounded to 32 simultaneous host-thread slots and 64 nested
entries per thread. An overflow is recorded explicitly. AOT stack overflow
invalidates outstanding timings on that thread and suppresses further AOT
pairing there for the session, avoiding a false match after irregular unwinding.
The last 32 scope details are retained in completion order. Finalization,
detected issues, or `flush_native_probe_detail()` emit only unseen retained
records. Evicted detail is explicitly labeled `probe.detail_gap` with reason
`bounded_retention`; it is distinct from journal loss. Details include logical
entry, variant, outcome, token, start time and valid duration, without raw host
pointers. Common guest timeline fields describe the flush; `start_ns` describes
the scope's host monotonic start and is the field to align with input records.
This bounded history cannot provide a complete call trace for a whole play
session. The full input journal remains separate from that detail.

## Read-only state and performance

The state reader uses the existing documented game-state offsets with const
memory access. It never links the debug command/cheat-write implementation.
The character name is checked only for loaded-state plausibility and is never
recorded. Base fields require a supported executable and valid loaded
character; unavailable fields are null.

Quest fields additionally require an active corpus-matched `game_task.ovl`
observation, a freshly matching immutable header/code fingerprint, and an
unchanged validation generation. The recorded fields include hunter health,
recoverable/maximum health, stamina in game units, quest frame counters and
five monster slots. Invalid pointers, nonfinite values, and out-of-range data
remain explicitly unavailable. Conservative numeric bounds protect diagnostics;
they are not new game rules or proof of game maxima.

These are periodic snapshots, not a stream of every damage, spawn, or animation
transition. Short events can occur between samples. Relevant later cases need
their own validated event probes when periodic evidence is insufficient.

`perf.summary` preserves valid metrics independently when others are unavailable,
including GPU timing. Frame rate, game frame rate, emulation speed, time splits
and rendering counters come from the existing frame statistics. An offline
probe microbenchmark describes only the tested isolated helper and cannot
predict whole-game recording overhead.

## Offline verification

Build synthetic checks without launching the application:

```sh
cmake --build out/mhp3rd --target mhp3rd_probe_tests mhp3rd_state_observation_tests mhp3rd_runtime_diagnostics_tests -j2
ctest --test-dir out/mhp3rd -R 'mhp3rd_(probe|state_observation|runtime_diagnostics|probe_instrumentation)_tests' --output-on-failure
```

The optional original-code harness reuses production generated objects:

```sh
cmake --build out/mhp3rd --target mhp3rd_aot_probe_tests -j2
out/mhp3rd/bin/mhp3rd_aot_probe_tests profiles/mhp3rd/game/EBOOT.ELF
```

It executes bounded leaf calls only, with no game boot, HLE profile, window or
audio. It compares all CPU fields and output/canary bytes against the independent
interpreter, with observation enabled and disabled. It covers both angle
returns, matrix delay-slot stores, RAM aliases, direct chaining with a stale PC,
same-unit continuation, rejected
span changes, exception interruption, and actual native/verify/fallback paths.
An isolated vector microbenchmark reports enabled/disabled host costs without
claiming gameplay speed or live coverage.

Verification results and their exact local logs are tracked in `docs/tasks.json`.
Live trigger coverage, recorder usability, and subjective gameplay acceptance
remain pending until the user-led paired cases.

On 2026-09-27 (Asia/Tokyo), Apple Silicon macOS passed four synthetic CTests,
three AddressSanitizer/UndefinedBehaviorSanitizer suites and three ThreadSanitizer
suites. The production-object harness passed 1,280 interpreter differential
calls, direct chaining, a 2,048-return same-unit chain, span rejection, exception
interruption and all five native/verification paths plus fallback. The existing
1,208,316 native differential cases also passed, and the full renderer-enabled
application linked without launch. Exact source, input, binary and log hashes
are in local `out/testing/certified-probes-validation.json`.

This build used `PSPRECOMP_AOT_PRODUCTION_FASTPATHS=OFF` and
`PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH=OFF`. Other build-option combinations and
other platforms were not tested. OBS-003 implementation is complete within
these offline boundaries; it does not complete paired gameplay acceptance.
