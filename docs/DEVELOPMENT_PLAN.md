# MHP3 Native Migration and Paired Validation Plan

Plan version: **1.3**. Decision date: **2026-09-26**; amended **2026-09-28** ([Amendment 1.2](#amendment-12--texture-path-and-working-rules)) and **2026-09-29** ([Amendment 1.3](#amendment-13--loading-experience-first)). Task ledger: [tasks.json](tasks.json).

Handoff refreshed: **2026-09-29T01:40:00Z**. This refresh preserves the agreed scope and historical evidence; it records unfinished implementation explicitly.

This is the durable handoff for the agreed development approach. Read this document and the task ledger before starting work. The ledger is authoritative for task status; checkboxes below are its human-readable summary. Update both in the same change when a task changes status. A future chat summary must not replace these recorded decisions silently. New explicit user instructions can amend the plan; record the amendment and update the ledger before acting on the new scope.

## 1. Goal, decisions, and current evidence

### Goal

Preserve MHP3's content, rules, timing, and feel while progressively replacing dependencies on the PSP execution model with independently implemented native modules. Reuse original resources and behavior wherever possible. Static recompilation already produces native machine code; the additional goal is reducing dependencies on guest registers, guest addresses, PSP scheduling/HLE, code overlays, and PSP graphics commands.

A native leaf behind a guest ABI adapter is a useful intermediate result, not evidence that the whole game or subsystem is runtime-independent.

### Confirmed decisions

- Start with **Apple Silicon macOS**. Keep the architecture portable, but do not claim other platforms are verified.
- Freeze **Baseline B0 at `4292eb6`**, with the Chinese interface retained and **every native experiment disabled**. Both test applications receive the same observational recording-tool revision.
- Prioritize **expanded offline native migration before the first manual handoff**, rather than stopping at the two existing helper experiments or delivering a tools-only prototype.
- Initial implementation and verification must not boot the full game, navigate menus, or depend on gameplay triggers. Loading a local ELF/resource into a bounded standalone test harness is allowed.
- Resource preparation comes first: **complete raw extraction and a traceable resource index**, followed by format-specific analysis as needed.
- Later work is delivered in related batches. The user performs cases in Baseline and candidate through an **in-game Chinese test panel**, with **15–30 minutes total per batch across both versions**.
- The assistant owns implementation, automatic checks, case preparation, log collection/analysis, diagnosis, fixes, and regression conversion. The user owns gameplay execution and subjective observations.
- Do not return to repeated assistant screenshot navigation as the normal test workflow. Screenshots can supplement a report; they cannot establish animation, collision, AI, or combat correctness.
- Preserve original ISO/save inputs. Use separate copies of saves for each role/run. No automatic upload of logs or game data.
- Do not combine this initial effort with new gameplay, HD remastering, a new Metal renderer, a public room-code service, or App Store distribution.

### Amendment 1.2 — texture path and working rules

Decided by the user on 2026-09-28 after the G1c integration repair (iteration 11). It supersedes earlier text where they conflict; historical records are unchanged.

1. **Close G1c with one focused review.** ASSET-014 is narrowed to the observation work it has delivered (G0, G1a-R1, G1b-read-R1 and G1c). One review by a different session or agent covers only the iteration-11 fixture, decoder and tracker changes; the staged-B0 default-off configure is rerun. If both pass, ASSET-014 is completed. Further review rounds happen only for a confirmed defect, not as a routine gate.
2. **Measure live routes before proving more offline.** The remaining G1 work (positive copy-worker route, real file identity/open/seek, scheduler behaviour, global epochs and writer exclusion) is no longer proved exhaustively in advance. The tracker is first installed in the candidate application in observe-only mode (Native Off, authority `NotReady`) with per-route counters, and the user runs one 15–30 minute case. Only the routes that actually occur, weighted by frequency, receive the additional offline proof needed for enablement; routes that do not occur stay on the original path.
3. **First enablement is Verify, then Native.** The existing native texture command/decoder path is first enabled in Verify mode for the observed, proved routes only, comparing against the original result, with every other route falling back to original code. Native mode follows only after a clean Verify batch.
4. **Tool-neutral agents.** Any tool or model may be used. Record the actual tool and model in the ledger; do not invent or normalize them.
5. **Working rules** (in addition to section 5):
   - A result counts as passing only if it was produced in the same session from binaries freshly rebuilt from the current tree. Older reports are historical evidence, not current status.
   - Evidence runs use a frozen tree: do not edit sources while a runner that hashes them is executing.
   - Parallel agents never edit the same file. A shared fixture has one owner per iteration.
   - Fixture constants and guest addresses must cite a traced original instruction or a recorded fact. Do not seed arbitrary addresses to make a fault disappear.
   - Temporary diagnostics are removed before evidence runs and never change fixture semantics. Check the radix of any value copied from trace output.
   - Keep the ledger proportional: one concise history entry per meaningful result; detail belongs in the reports it references.

### Amendment 1.3 — loading experience first

Decided by the user on 2026-09-29. The active focus moves from full texture de-PSP work to what players feel: loading. ASSET-014 stays paused and ASSET-015..ASSET-019 are deferred; the Amendment 1.2 working rules still apply.

The goal is that area changes, including those inside quests, show only a short black fade instead of a loading screen or animation. Work proceeds in measured steps, each with an off switch:

1. **Measure** (LOAD-001..LOAD-003). The user plays one short session with `MHP3RD_TRACE_LOAD` on, from a copy of the registered starting save, covering town transitions and several in-quest area changes. The analysis reports each load's real and game time, whether fast loading engaged, and where the time went. Hypothesis to test: in-quest area changes keep sound playing, so the silence-based fast-loading detector does not engage.
2. **Black fade instead of the loading screen** (LOAD-004). Host-side presentation only: fade to black when a load starts, hold, and fade back in on the first game frame after it. No guest code changes.
3. **Faster in-quest loads** (LOAD-005). Scope set by the measurement. Candidates, in order: let only the identified loader threads stop waiting on the emulated clock while the main thread and audio keep real time; then native replacements for the load's own CPU work (SHA-1 check, decompression) behind the existing differential gates.
4. **Prefetch on approach** stays out of scope unless the earlier steps leave a noticeable wait: it changes the game's own transition logic and competes with its 32 MB memory management.

### What already exists

- Chinese interface work: commit `e5a5d9a`, draft PR [#1](https://github.com/etsusei/Yakumo/pull/1).
- Native helper experiments: implementation commits `26398fe` and `f60e77f`; evidence recorded in `4292eb6`, draft PR [#2](https://github.com/etsusei/Yakumo/pull/2).
- The supplied image passed supported executable checks. Its archive has **6,043 entries and 355 code overlays**. All overlay header/code fingerprints matched the release libraries.
- The supplied save passed hashes, decryption, and round-trip checks. Testing used independent copies and verified the source archive/copy unchanged.
- Angle helper: **806,432 offline differential cases passed**, but **zero calls** were observed on the tested village route. In-game coverage remains absent.
- Scale-matrix helper: **100,512 offline cases passed**, including 10,000 prefix fallbacks. Live verification compared **48,365 calls with zero mismatches**; a separate native run used **48,405 calls with zero fallback**.
- The tested route was read-save, character selection, village entry, and a short walk. Audio output, combat/quests, multiplayer, other platforms, and long-session stability were not verified.
- Full raw resource extraction and source-anchored reuse verification passed. All 6,043 decoded entries matched the independent C++ path byte for byte; the shared journal core passed offline recovery and persistence checks. Input/timeline/overlay observer hooks have offline tests and the first paired live recordings; broader coverage remains pending. Certified leaf probes and guarded state/performance readers have offline verification. Local packaging and comparison reports have passed offline checks. The Chinese case panel has offline verification; both signed paired applications passed preparation-only delivery checks. The first user pair is complete, with scale/copy same-input verification and explicit comparison limits; wider gameplay coverage remains pending. Existing input scripts inject input; they are not a complete recorder.

See [NATIVE_EXPERIMENT.md](NATIVE_EXPERIMENT.md) for the exact experiment boundaries. Historical successful boot tests do not override the new offline-first workflow.

## 2. Ordered milestones and checklist

Task IDs match the ledger. A checked box means the task's stated scope is completed, not that the whole feature is gameplay-validated.

### Completed foundation and planning

- [x] **FOUND-001** — Implement the Chinese interface and language selection.
- [x] **FOUND-002** — Validate local image/save inputs and establish the historical village baseline.
- [x] **FOUND-003** — Implement and offline-test the angle helper, retaining its live-coverage limitation.
- [x] **FOUND-004** — Implement and validate the scale helper offline and on the historical live route.
- [x] **PLAN-001** — Audit recorder seams, offline boundaries, and additional native candidates; record user decisions.
- [x] **PLAN-002** — Persist this revised plan, synchronized checklist, and task ledger; validate the handoff.

### Milestone 0 — Freeze inputs and prepare raw resources

- [x] **RES-001** — Register B0, source/build/dependency identities, input fingerprints, and immutable starting-save snapshots.
- [x] **RES-002** — Build and test a guarded resource-preparation command around the existing ISO/DATA.BIN readers.
- [x] **RES-003** — Extract the full raw image contents and DATA.BIN entries locally; generate and validate the manifest.

Resource preparation requirements:

1. Reference the original ISO in place; do not move or rewrite it. Retain its fingerprint and the supported executable identities.
2. Extract ISO filesystem content and deobfuscate DATA.BIN entries into a staging directory, then publish the completed workspace atomically. Re-running must reuse only hash-verified outputs or regenerate them safely.
3. Put the workspace under the already ignored `profiles/mhp3rd/analysis/resources/`. Separate `raw-disc/`, `raw-entries/`, `derived/`, and `working/`. Keep `manifest.json` alongside them.
4. Index each entry by original numeric ID, source span, stored/extracted lengths, SHA-256, known header/type, confidence, and overlay identity where applicable. Preserve empty entries and padding/unknown-length distinctions; do not invent meaningful filenames for unknown entries.
5. Validate bounds, traversal-safe paths, truncation, output lengths, hashes, and repeatability. Treat the observed 6,043/355 counts as checks for this supported image, not universal format constants.
6. Preserve raw outputs unchanged. Later conversions operate on derived/working copies and retain source-ID mappings.
7. Extraction means byte-level preparation, not semantic understanding of models, animation, hit data, or AI. Those interpretations require separate work and later gameplay confirmation.
8. Baseline and the first candidate continue reading the original ISO. Running from an extracted directory is a separate future change with its own acceptance cases.

**Exit:** a complete validated local resource inventory, reproducible extraction command, immutable input identities, and no game-data additions to Git.

### Milestone 1 — Expand genuinely offline native coverage

- [x] **OFF-001** — Standardize helper contracts, feature switches, fallback behavior, and evidence counters.
- [x] **OFF-002** — Add the translation-matrix leaf at `0x08878B4C` (36 bytes including the return delay slot).
- [x] **OFF-003** — Add the four-word vector constructor at `0x08877818` (24 bytes including the delay slot).
- [x] **OFF-004** — Add the nine-word matrix-layout copy at `0x08879D08` (80 bytes including the delay slot).
- [x] **OFF-005** — Add independent synthetic ISO-reader and PSMF-demuxer regression coverage.
- [x] **OFF-006** — Run the complete offline gate and produce a versioned coverage report before manual handoff.

The three new leaves were identified from the existing local analysis and generated corpus. Recheck their full spans against the registered ELF before implementing:

| Target | Behavior to preserve | Important constraints |
| --- | --- | --- |
| Translation matrix | Identity matrix with raw FPR 12/13/14 values at byte offsets 48/52/56 | Preserve final VFPU state and the delay-slot store. Initially accept standard VFPU prefixes only; unusual prefixes use the original path. |
| Vector constructor | Raw FPR 12/13/14 values at offsets 0/4/8 and literal zero at offset 12 | No arithmetic or prefix dependency; preserve signed zero, NaN payloads, all other state, and return PC. |
| Nine-word copy | Copy offsets 0/4/8, 16/20/24, and 32/36/40 | Leave row-padding words untouched. Preserve original read/write ordering for overlap and final VFPU scratch lanes; do not substitute an assumed memcpy contract. |

Implement portable data functions separately from guest ABI adapters. Use full-span fingerprints and independent `off / verify / native` switches, initially named `MHP3RD_NATIVE_TRANSLATION_MATRIX`, `MHP3RD_NATIVE_VECTOR_CONSTRUCT`, and `MHP3RD_NATIVE_MATRIX_COPY`. Preserve the existing angle/scale switches.

Each contract identifies inputs, outputs, changed CPU fields, readable/writable memory, exceptional inputs, and non-restorable side effects. Reject unsuitable functions rather than treating speculative register meanings as facts. Source bytes and generated code remain local.

Standardize `calls`, `verified`, `native`, `fallbacks`, `mismatches`, and `errors`, with the measured scope recorded. These are evidence counters, not a whole-game migration percentage.

The offline gate must run the existing angle/scale suites **with the local ELF argument**, not only their small CTest examples. Add original-code differential checks for all three new leaves, raw-bit edge cases, seeded random inputs, aliases, overlap, memory canaries, fallback behavior, changed fingerprints, and bounded execution.

ISO and PSMF components are already native. Their new tests count as infrastructure assurance, not new game-code migration. Test byte/record behavior with synthetic fixtures; do not claim movie/audio/display fidelity. Do not expand this milestone to Vulkan presentation, audio devices, kernel scheduling, animation playback, AI, or combat.

**Exit:** five helper contracts with zero supported-input differential mismatches, correct rejection/fallback, repeatable reports, and replacements still disabled by default. Live coverage of new leaves remains explicitly pending.

### Milestone 2 — Implement the recorder and paired test delivery

- [x] **OBS-001** — Implement the versioned event journal, bounded buffering, crash-prefix recovery, and offline lifecycle tests.
- [x] **OBS-002** — Connect observational input, timeline, camera, and overlay-identity sources.
- [x] **OBS-003** — Connect certified probes, read-only state observations, performance summaries, and errors.
- [x] **OBS-004** — Implement record packaging and the offline comparison/report command.
- [x] **PAIR-001** — Add the Chinese in-game case panel without exposing state-changing cheat controls.
- [x] **PAIR-002** — Package separate Baseline/candidate Mac applications with the same recorder revision and isolated run data.
- [x] **PAIR-003** — Validate preparation/export behavior using synthetic child processes and publish the first case pack.

Build and test the tooling using constructed events, fake clocks, temporary files, synthetic overlays, and subprocesses that exit or crash. Real-game hook coverage and UI usability remain pending until user acceptance; an offline test of the recorder must not be presented as validation of its live integration.

**Exit:** both applications, the case pack, automatic recording/export, and the comparison command are ready for the user. This is readiness for manual testing, not a manual pass.

### Milestone 3 — First user-led acceptance batch

- [x] **CASE-001** — User completes the initial Baseline/candidate batch, totaling 15–30 minutes.
- [x] **CASE-002** — Analyze the records, resolve defects, identify missing coverage, and publish the acceptance report.
- [x] **FONT-001** — Restore the previously verified Chinese game font lost by paired configuration seeding; validate glyph coverage and deliver corrected app copies.

| Case | User actions | Evidence required |
| --- | --- | --- |
| `REC-01` | Launch, load the agreed save/character, start and finish a case in the panel | Correct role/version/input identities, loading events, case boundaries |
| `REC-02` | Walk, stop, turn, move the camera, open/close the menu; mark an observation | Ordered applied input, camera input, UI-consumed input, pause/focus boundaries |
| `NATIVE-01` | Inspect the same character, enter the village, follow a short route | Per-helper real call coverage, verified/native/fallback counts, state differences, visible observations |
| `REC-03` | Finish the active case and close the window normally | Complete local record package, correct completion status, independent save directories |

Do not make the user search complex combat scenes merely to trigger an unobserved helper. Zero calls means **not covered** and requires a later targeted case. It is not a pass.

### Milestone 4 — Repeat related implementation and acceptance batches

- [ ] **ITER-001** — Maintain the module/dependency inventory and create finite child tasks for each accepted batch.
- [ ] **ITER-002** — Repeatedly deliver, analyze, fix, and regress gameplay/resource batches with the user.

Finite work beneath these ongoing workstreams:

- [x] **INV-001** — Complete the initial source-based module inventory and finite batch contracts.
- [x] **NAT-001** — Implement explicit scale/copy native execution profiles, gates and production chained-call checks.
- [x] **NAT-002** — Deliver the matched one-case native execution pair.
- [x] **NAT-003** — User executes the paired native data case.
- [x] **NAT-004** — Analyze actual native execution and retain scoped acceptance or off defaults.
- [x] **VEC-001** — Certify the four vector-metric contracts and numerical/state behavior offline.
- [x] **VEC-002** — Implement a portable vector-metric core, adapters and differential tests.
- [x] **VEC-003** — Add bounded observations and evidence-based user cases for that module; three metrics observed, norm-squared remains uncovered.
- [x] **ASSET-001** — Certify nested indexed resource-bundle boundaries from original consumers.
- [x] **ASSET-002** — Implement a portable bundle view and provenance-preserving offline validation.
- [x] **ASSET-003** — Certify the internal layout of TMH-marked children using original consumers.
- [x] **ASSET-004** — Implement owned encoded TMH views and the original descriptor/corpus gate.
- [x] **ASSET-005** — Establish explicit texture layout, stride/extent and pixel-decoder inputs.
- [x] **ASSET-006** — Implement explicit byte-window decoding and scoped pixel-oracle validation.
- [x] **ASSET-007** — Integrate owned texture inputs and the portable decoder at the renderer boundary, with explicit modes and offline dispatch checks.
- [x] **ASSET-008** — Prepare the renderer-specific comparison policy, finite related case, and signed paired delivery.
- [x] **RENDER-001** — User runs the combined render/vector discovery case; the earlier native-data rerun was explicitly waived.
- [x] **RENDER-002** — Analyze actual combined observations and retain scoped acceptance or explicit coverage gaps.
- [x] **ASSET-009** — Certify the original texture-state command builder as an offline native data boundary.
- [x] **ASSET-010** — Implement a portable texture-command module against the certified original consumer contract.
- [x] **ASSET-011** — Implement and offline-certify a bounded guest adapter, preserving CPU/memory effects and rejecting unsupported inputs before mutation.
- [x] **ASSET-012** — Certify a real caller's source/command allocation bounds and lifetime before production integration.
- [x] **ASSET-013** — Implement bounded source authority from original transfer/lifecycle receipts, including late-writer invalidation and exact fragment completion.
- [ ] **ASSET-014** — Paused by the user on 2026-09-28 before the Amendment 1.2 focused review reported; the direction is being reconsidered. G0, G1a-R1 and G1b-read-R1 refreshed normal/strict gates pass. G1c connects the selected state-8 inline copy/optional transform/digest/worker acknowledgement/retirement path to the real tracker; the compiled integration executable passes 14 named cases in normal and strict modes (six writer releases with full CPU/RAM/VRAM comparison and observed original retirement, eight fail-closed faults retaining their writer), and the lower 64-case oracle passes. Independent copy-worker routes remain unsupported and retain their writers. Source authority stays exactly `NotReady`, `transfer_readiness=false`, and Native remains Off. G1c awaits independent acceptance; G1 and ASSET-014 are incomplete.
- [ ] **ASSET-015** — Install the transfer/completion tracker in the candidate in observe-only mode with per-route live counters; deliver a matched pair and a 15–30 minute coverage case.
- [ ] **ASSET-016** — User runs the observe-only texture coverage case.
- [ ] **ASSET-017** — Analyze live route coverage and define the minimal proof set for the first enablement.
- [ ] **ASSET-018** — Close proof gaps for the observed routes only; enable authority-backed native texture loading in Verify mode for them with original fallback elsewhere; deliver a matched pair.
- [ ] **ASSET-019** — User-led Verify texture batch; analyze, fix, and decide whether Native mode may follow.
- [x] **LOAD-001** — Prepare the load-measurement session: an isolated data directory from the registered starting save, a launch script with load tracing, and Chinese instructions.
- [ ] **LOAD-002** — User plays the load-measurement session.
- [ ] **LOAD-003** — Analyze each load's real/game time, fast-loading engagement and time split; set the scope of LOAD-004/LOAD-005.
- [ ] **LOAD-004** — Present a black fade instead of the loading screen, host-side, with an off switch.
- [ ] **LOAD-005** — Make in-quest area loads fast within the scope set by LOAD-003, with an off switch.

The initial inventory is [NATIVE_MODULES.md](NATIVE_MODULES.md); finite requirements are in [NEXT_NATIVE_BATCHES.md](NEXT_NATIVE_BATCHES.md). ITER-001/002 stay in progress as workstreams. Finite tasks depend on INV-001 or other finite prerequisites, not on treating an ongoing workstream as already finished.

Every batch follows: investigate behavior and dependencies -> define contract and observations -> implement and run automatic checks -> prepare cases -> user plays both versions -> analyze -> fix -> retain reusable regression inputs -> accept or keep disabled.

Prioritize modules with independent inputs/oracles, then modules with validated replayable samples, then modules requiring new observation and subjective cases. Keep each batch related and individually switchable. Animation, models, movement, collision, combat, AI, quests, saves, and networking are grouped by actual dependencies and evidence readiness rather than treated as a fixed sequence of wholesale rewrites.

Every case states starting save/character, equipment/settings, steps, checkpoints, observations, required trigger/coverage, stopping conditions, and which claims require human judgment. The user operates the game; the assistant performs analysis and diagnosis. Repeat only affected cases after a fix, with broader regression at milestone boundaries.

### Milestone 5 — Remove residual guest dependencies and finish integration

- [ ] **EXIT-001** — Audit and remove obsolete guest execution dependencies only after their callers and scenarios are accepted.
- [ ] **EXIT-002** — Complete the agreed full-game, save, multiplayer, platform, and long-session acceptance matrix.

Track remaining guest-register, guest-memory, scheduler/HLE, overlay, and graphics-command dependencies per module. A helper still using an ABI bridge remains partially dependent. Keep old paths until their consumers and compatibility obligations are covered.

The final native build must no longer need execution of original PSP code or interpreter fallback; original resources may still be read from the ISO as a data container. Baseline remains a test reference, not a shipping runtime dependency. Full-game completion is not inferred from a small set of high-frequency helpers or lines rewritten. Discovery may change effort estimates; no full-rewrite deadline has been promised.

## 3. Recorder, interfaces, and comparison rules

### Minimum public contracts

- `RunManifest`: schema/run/batch/role IDs; Baseline ID and commits; binary/tool/build/config identities; game, overlay, and starting-save fingerprints; feature switches; device/platform facts; recording mode and coverage/loss flags.
- `CaseSpec`: stable ID/version, prerequisites, steps, checkpoints, required probes/state, expected observations, human acceptance fields, and completion criteria.
- `ProbeSpec`: stable logical ID, module/entry identity, variant, observed fields, timing/count semantics, and explicit coverage limitations.
- Recorder operations: begin/end run, begin/end case, checkpoint, anomaly marker, input/state/error event, and scoped certified-probe timing.
- Offline comparator: two record packages in; machine-readable findings and a readable HTML report out. The user does not need to assemble files or invoke the comparator manually.

### Input and timeline

Record both game-window input events and the final control tuple written by `sceCtrlReadBufferPositive`, after camera/aim rewriting and suppression. Include mouse/touch camera motion separately: final pad samples alone cannot explain those camera changes. Observe injected script actions explicitly if used in a diagnostic run.

Use guest flip/frame, vblank/virtual time, control-read ordinal, host monotonic time, and input order. Label game, paused UI, over-game UI, text input, and setup domains. Do not use renderer/interpolation frame counts as the sole game clock. Log focus loss, device/config changes, and UI consumption. Do not record keys from unrelated applications.

### Calls, state, and performance

Keep exact native/HLE scope counts, guest-entry hits, and dispatch-segment statistics distinct. Existing outer/chained hooks are not universal logical function boundaries: inline paths, interpreter execution, stops, and exceptions need explicit coverage treatment. Never label a dispatch duration as an entire gameplay function duration.

For the first certified leaves, validate observational entry/return wrappers offline before claiming exact counts or durations. Baseline must keep its original AOT behavior; do not replace its normal execution with an interpreter merely to obtain performance statistics. Uncertified probes report limited entry/segment observations instead.

Associate guest observations with overlay slot, immutable header/code fingerprint, load generation, and entry. A reused address must not merge unrelated functions. Keep stable logical probe IDs across baseline and replacement implementations.

Default to case-selected probes, aggregated counts/total/max duration, periodic performance summaries, and a bounded recent-detail buffer. Retain relevant detail around errors/user markers. Detailed chain tracing is a separate diagnostic mode because it can change the execution fast path. Measure and report recording overhead; compare timings only under compatible recording modes and conditions. No speedup is inferred from verification mode.

Reuse or extract validated read-only game-state readers, guarded by the correct executable/overlay context. Mark unknown or unavailable fields rather than inventing labels. The recorder must not expose the debug menu's state-changing cheats.

### Journal and process lifecycle

Use versioned append-only records with sequence, length, and checksum. Write in the background with bounded queues; submit at least once per second and at case/checkpoint/anomaly boundaries. Record overflow, incomplete scopes, and I/O failures explicitly. A truncated/corrupt tail cannot become valid evidence silently.

A lightweight Mac test launcher supervises the game process, collects diagnostics, and finalizes the existing journal after normal exit or abnormal termination. Record why the game stopped: the current program's window-close exit code is 4 and must not be misreported as a crash merely because it is nonzero. Some GPU failures use `_Exit`, so in-process cleanup alone is insufficient. Preserve the valid journal prefix; do not promise preservation of unwritten data or power-loss durability.

Produce a local package with manifest, journal/events, statistics, diagnostic logs, summary, and user markers. Exclude ISO contents, whole saves, and full memory dumps by default. Provide an Open results folder action. No cloud upload or continuous background monitoring is included.

### Comparison and acceptance

Report at least: same-input match, confirmed mismatch, incompatible prerequisites/inputs, insufficient coverage, and incomplete recording. Exact equality is appropriate for validated same-input function results and canonical data; manually played routes align by case/checkpoint/event and relevant starting state. Timing differences are diagnostic evidence, not automatically functional failures.

Manual input, wall-clock seeding, random events, asynchronous work, and caches mean separate sessions are not automatically deterministic. No full-game save-state or deterministic replay capability is assumed. Samples become replayable only after dependencies, state capture, and reproduction are independently validated.

Do not execute side-effectful functions twice without isolation. RNG consumption, audio, threads, allocations, file writes, and network effects must be accounted for before using a shadow comparison. Screenshots are optional supporting observations, not the behavioral oracle.

## 4. Paired applications and user workload

- Supply Baseline and candidate as separate identifiable Mac applications with the same observational tool revision/profile. Baseline's native replacements stay off; the candidate's batch manifest controls enabled modes.
- Create isolated writable run directories from the same untouched starting snapshot. Share only read-only source resources. Never restore a save over a running process or mutate the original supplied archive.
- Keep B0 immutable. If a reference version must change, create a new explicit Baseline identity and invalidate incompatible cached results.
- Reuse Baseline records only when the case, prerequisites, save/config/resource identities, and recording semantics remain compatible.
- The Chinese panel shows role/version, recording health, the current case and steps, and begin/checkpoint/anomaly/end controls. Outcomes are normal, abnormal, uncertain, or skipped. Closing an unfinished case marks it interrupted; absence of a crash is not acceptance.
- A user-driven session is not cut off by the agent's 60-second smoke-test convention. The agreed 15–30-minute batch is manually operated and ends when the user closes it. Agent-driven diagnostic runs, if separately needed later, remain bounded and do not become open-ended navigation loops.
- After the user reports completion, the assistant reads local records and produces the comparison. Fixes should reduce repeated user effort by retaining reproducible regression samples where feasible.

## 5. Automatic gates, reporting, and continuation protocol

Automatic gates cover synthetic resource bounds/truncation, original-code differential results, aliases/canaries, feature-off behavior, fingerprint rejection, journal recovery, input-domain semantics, overlay reuse, abnormal subprocess exits, overflow/disk failure, version mismatch, interrupted cases, and zero-call coverage. Test runs must not alter source saves.

Label every result by evidence tier: synthetic specification, local-original-code differential, captured-sample replay, live observation, or user experience acceptance. A completed implementation task can still have `pending`/`not_covered` manual validation. Display these separately.

For each finite implementation task:

1. Read this plan, `tasks.json`, the relevant source, and current Git state.
2. Claim a ready task in the ledger with the actual agent identity and start time. Do not claim work already performed by another agent.
3. Record changes, exact artifact paths, evidence/limitations, dependencies, and status transitions. Use repository-relative paths; private artifacts are explicitly local-only.
4. Keep synthetic fixtures and independently written code in Git; keep game assets, generated game code, saves, captures, and local traces out of Git.
5. Use branches/draft PRs, English committed prose/code, targeted checks, `cmake --build -j2`, and one build per directory. Do not delete incremental build metadata. New Chinese UI strings use the translation catalog as required by the requested interface.
6. On completion, update the ledger and matching checkbox together. On pause/block, state why and what resumes work. Do not mark a task completed while required evidence or outputs are missing.
7. Log reassignment and preserve history. Any tool or model may be used (Amendment 1.2); record the actual tool and model, and give subagents complete task context. Earlier sessions used gpt-6-sol and later gpt-5.6-luna under the then-current instructions; preserve those historical facts.
8. Follow the working rules in Amendment 1.2: fresh rebuilds before claiming a pass, frozen trees during evidence runs, one editor per file, traced constants only, and a proportional ledger.

### Resume here

Current continuation (Amendment 1.3): **LOAD-002 awaits the user**: double-click `out/testing/load-measure-1/开始测量.command` and follow `说明.md` there; LOAD-003 then analyzes the log. ASSET-014 remains paused: it was **paused by the user on 2026-09-28** while the direction is reconsidered (loading/performance was discussed). The focused review was stopped before it reported. When resuming under Amendment 1.2, **ASSET-014 needs one focused review** of the iteration-11 changes plus a rerun of the staged-B0 default-off configure; if both pass it is completed, and **ASSET-015** (observe-only live tracker and route counters) starts. Earlier: the user resumed ASSET-014 from Codex task `01a0de15-428d-71b2-becb-c93878a0cd71`. The historical [PAUSE_CHECKPOINT.md](PAUSE_CHECKPOINT.md) remains an accurate record of the earlier pause; the live status and current action are tracked in `tasks.json`.

G0 remains complete on the current G1a source: the latest normal and strict-fail ASan/UBSan reports each match all 41 registered input/source/build identities and pass the exact healthy-authority `NotReady` assertion, full CPU/RAM/VRAM checks, bounded calls and `transfer_readiness=false`. The pause-era `not_run` snapshot and earlier resumed reports remain historical evidence.

G1a-R1's bounded compiled load/enqueue/descriptor-generation gate passes in normal and strict-fail ASan/UBSan builds. It runs the actual unit 0040 owner/provider path and unit 0023 manager length/enqueue/descriptor path. Four selected requests produce five descriptor generations because an unowned disjoint enqueue reuses a selected descriptor address; the superseded generation becomes non-current while its pending writer remains live. The controlled fixture exercises three descriptor-slot reuses (including cached/uncached alias reuse), fourteen callback/correlation faults, two capacity losses, invalid ranges and exact/over-slot boundaries; it does not claim a natural full-ring wrap. AOT/interpreter CPU/RAM/VRAM agree across 117 calls, and healthy authority remains exactly `NotReady` with `transfer_readiness=false`. The build-local output/manifest writer now retains recoverable backups when rollback fails and tests temporary-file, backup-move, install and restore failures. Fresh G0/G1a reports bind the updated tool source; 50 scoped Python tests and two focused CTests pass. G1a does not certify loaded bytes: read/copy/transform/terminal/cancellation observations remain later gated work. Independent C2C review accepted G1a-R1 and its publication-safety follow-up. Iteration 6 is limited to descriptor revalidation and state-8 read-attempt/result association, with readiness false and pending writers retained. No application installs a tracker or callback owner; keep the production option default Off and preserve existing paired apps and user records.

Iteration 6 G1b-read passed its original 69-scenario normal and strict matrix, but independent review did not accept it as closed. The reviewer found that a retry could skip the next State8 callback, and that a historically tracked unowned descriptor did not initialize its public generation identity. The report also omitted upstream lifetime outputs/manifests and did not pin the fixed matrix's key call counts; the new read CMake include was absent from the Baseline staging allowlist.

Iteration 7 G1b-read-R1 fixes those bounded read-prefix and evidence gaps. The 75-scenario matrix passes 450 AOT/interpreter calls in normal and strict ASan/UBSan modes, using three contiguous shards with exact ordered coverage and one unchanged source/binary identity. It tests a dropped second State8 after a short result, selected-to-unowned descriptor-ring reuse at the same address and through cached/uncached aliases, a synthetic partial-storage-overlap fault, and a same-byte descriptor-commit callback during an active attempt followed by a rejected late result. The runner pins scenario identities and counters, and binds the lifetime, transfer, probe, and read source/output/manifest chain. A fresh B0 source stage includes the new unconditional read instrumentation module; its default-OFF configure passes without building or launching the application. Refreshed G0 and G1a-R1 normal/strict reports pass, as do the 2,256-input unowned corpus, 53 scoped Python tests, and two focused CTests. All results retain exact `NotReady`, all pending writers, and `transfer_readiness=false`. Independent C2C review accepted R1 in scope; ASSET-014 remains active and no later phase is authorized until the user requests another concrete batch.

Iteration 8 G1c connects the selected inline completion observation batch and its tracker design. The isolated original-code oracle runs 64 AOT/interpreter scenarios with 56 completion-event cases and zero sequence failures; normal and strict final artifacts are `out/testing/texture-completion-c2c_a27f-i9-final3-normal.json` and `out/testing/texture-completion-c2c_a27f-i9-final3-strict.json`. A separate synthetic tracker smoke reuses the compiled R1 owner/enqueue/read path, forwards a bounded completion sequence to `TextureTransferTracker`, and proves one exact writer reaches `Completed` and `prove_external_quiescent` while authority remains `None`/`NotReady`; its smoke reports are embedded in those artifacts. Refreshed G0/G1a/G1b normal/strict reports, 51 scoped Python tests, focused CTest 2/2 and the completion progress test pass. The smoke is explicitly not live completion-generated unit-0024 integration; file identity, scheduler behavior and the original positive copy-worker route remain modeled or unsupported. The application target remains unchanged, Native remains Off, and source-loading completion APIs remain unused. G1c remains awaiting independent acceptance.

The build/evidence handoff now defines normal and sanitized integration targets, guarded by the completion object set, the real tracker/lifetime/command/source targets, and workflow B/C's decoder and fixture entry points. Workflow B's decoder source and workflow C's integration fixture are present and staged in the source hash chain. The `check_texture_completion_observation.py --integration-oracle` mode requires the compiled integration executable and executable-owned fields for synthetic-sequence use, fixture retirement, readiness, source receipts, named cases, writer release/retention, and CPU/RAM/VRAM comparisons; it binds the generated completion outputs/manifests, decoder source, fixture source, stop header, CMake module and executable hashes. Direct Clang syntax and focused Python/static checks pass. The existing cache supplies CMake 4.4.3 at `/private/tmp/yakumo-build-tools/lib/python3.9/site-packages/cmake/data/bin/cmake` even though it is absent from `PATH`; reconfiguration of `out/testing/texture-read-prefix` generated both integration targets. Normal and sanitized `cmake --build --parallel 2` completed, with one decoder compile in each direct sanitized tracker target and no direct decoder compile in normal integration; the link step warned only about a duplicate `libmhp3rd_texture_commands.a`. The six-argument integration runner reaches the executables; the inline-verbatim path now reaches the acknowledgement and retirement boundary, while transform/digest and several fault cases still fail before a consolidated writer/comparison report is published. The progress and decoder CTests passed. The separate lower `--oracle` runner passed in normal and strict modes with 64 cases, 56 completion events, one tracker completion and 14 unsupported copy-worker cases; fresh reports are `out/testing/texture-completion-c2c_a27f-i10-normal.json` and `out/testing/texture-completion-c2c_a27f-i10-strict.json`. These lower reports do not claim compiled integration execution. ASSET-014 and G1 remain in progress.

Iteration 11 (Claude Code session, 2026-09-28) repairs the compiled G1c integration. The shared fixture had regressed after iteration 10: a diagnostic edit read the hexadecimal resource `0x11` as decimal and seeded the manager length table with 11, so every case failed at `DescriptorCommit`. The `0x014FA816` fault was the original RNG helper `0x088E7D54` reading its state through the zero object pointer at `0x09FC8BE8`; the fixture now stores the value game startup stores (`0x08ABAE40`) and seeds the state so the original enqueue selects each scenario's digest branch. Worker-frame edges now decode the worker's own registers, the post-digest join at `0x088653B4` is accepted as `DigestJoined`, operations closed before the worker started correlate their later syscalls, register faults are applied to the observed context object, and the wrong-worker fault binds the worker to a non-manager word. Two pre-existing breaks hidden by stale binaries were fixed: the G1a/G1b oracles compiled G1c-only fixture state, and the lower 64-case oracle stopped at retirement before its terminal sleep. Reports: integration `out/testing/texture-completion-integration-claude-i13-{normal,strict}.json` (14 cases, 6 released, 9 retained); lower oracle `out/testing/texture-completion-claude-i13-{normal,strict}.json`; G1b `out/testing/texture-read-observation-claude-i12-{normal,strict}.json`; G1a `out/testing/texture-transfer-observation-claude-i12-{normal,strict}.json`; G0 `out/testing/texture-lifetime-exact-notready-claude-i12-{normal,strict}.json`. The decoder test gained worker-frame, digest-join and acknowledgement cases; 4 completion CTests and 47 focused Python tests pass. The application was not built or launched, the staged-B0 default-off configure was not rerun, and no independent review has happened. G1, ASSET-014 and the later phases are unchanged.

**Local artifacts removed (2026-09-28, user request).** Local-only artifacts removed at the user's request on 2026-09-28 to free disk space: out/testing/dist (all delivered app pairs), out/testing/observed-sources and out/testing/observed-builds (their source snapshots and archived binaries), out/testing/texture-read-prefix and other scratch build trees, sanitizer/test binaries and out/native-experiment (except the packaging font). Ledger deliverables at those paths are now unavailable; their recorded evidence and hashes remain historical facts. Kept: out/mhp3rd, out/testing/baselines, out/testing/runs (user records), reports, profiles/mhp3rd/analysis and out/ccache. The lobby overlay module used by the texture oracles was copied to out/testing/texture-transfer-inputs/. Rebuild from the recorded commits if an old app or snapshot is needed again.

#### Historical paired delivery checkpoints

PAIR-002 is completed at implementation commit `4759d99`, published in draft PR [#11](https://github.com/etsusei/Yakumo/pull/11). The local applications are `out/testing/dist/Yakumo Baseline.app` and `out/testing/dist/Yakumo Candidate.app`. The 412-file B0 source manifest preserves registered B0 game files and records 84 explicit observation overrides. Both roles share the same recording revision, build configuration, effective settings and 355 overlay libraries, with distinct game binaries.

Validation includes 32 synthetic component tests, 21 real-binary preflight checks, both 1,280-call production AOT comparisons, and each actual application's native entry point running headless preparation-only supervision. The two prepared save copies have equal contents and distinct files/directories; original inputs stayed unchanged. No full game, native result dialog or live case was exercised. Evidence paths and actual contributor attribution are recorded in PAIR-002 and [the paired-app guide](PAIRED_TEST_APPLICATIONS.md).

**FONT-001 completed.** The user subsequently reported missing Chinese game text in both roles. Actual logs show 23 blank glyphs under Hiragino Sans W4; earlier complete-text experiments explicitly selected STHeiti Light.ttc. The paired launcher lost that setting when seeding isolated configurations. The corrected copies are ready in `out/testing/dist/font-fixed/`, using the previously selected font. The production rasterizer restores all 23 reported omissions and all 43 probe samples, including both app-created settings. Signed preparation checks pass. The user confirmed normal game text after the correct font-fixed app was launched. An intervening screenshot came from the old first-pair copy; distinct corrected app identifiers and display names now separate them. The full case batch does not need to be repeated. See [the font regression report](GAME_FONT_REGRESSION.md). Shared warnings are not evidence that a visual defect is harmless.

**First-pack migration evidence:** the user finished both formal first-pack applications. Both packages are complete and all eight case outcomes are normal. The original automatic comparison remains inconclusive because manual inputs differ and two paused-menu rendering-scale changes were recorded. Candidate case-scoped scale/copy verification totals are 511,126 and 9,772, with no mismatch, fallback or incomplete scope; angle, translation and vector helpers remain untriggered. See [the first paired acceptance](FIRST_PAIRED_ACCEPTANCE.md) and `out/testing/reports/initial-user-acceptance-1/analysis.json`.

CASE-002 fixed the recorder's missed transient setting change: save/snapshot observations now invalidate an active case immediately, even if the value is restored before the next marker. The UI explains the interruption and reopens an active case directly at Test session. The host build and six offline CTests passed. No user session was repeated, no delivered app was replaced, and no native replacement was enabled by default. Updated live menu behavior remains for a future related batch.

The delivered apps remain under `out/testing/dist/initial-batch/`, built from observation/candidate source `75ca5ff`; the B0 source snapshot is `out/testing/observed-sources/B0-initial-75ca5ff` with its build under `out/mhp3rd`. Preserve these snapshots and the two immutable user packages. The current checkout contains newer source-only recording fixes. Before a future paired handoff, rebuild both roles with matching observations and a fresh catalog/manifest as required. Do not confuse current source with already delivered binaries.

INV-001 now maps 15 source-anchored architecture areas and records static callers for the three untriggered leaves. Their 17/1/2 base JAL references and overlay references do not establish live coverage. Four vector-metric spans were independently fingerprinted as the next coherent module candidate; VEC-001 subsequently recorded the exact numerical/state contract; VEC-002 subsequently implemented and checked the module against production AOT.

**NAT-003 and NAT-004 are completed.** The following preserves delivery details; the user later waived the Auto-resolution rerun as an accidental setting change. See [NATIVE_DATA_ACCEPTANCE.md](NATIVE_DATA_ACCEPTANCE.md) for the actual scope and comparison limits. The matched native-data applications are ready in `out/testing/dist/native-data/`, with Chinese `START_HERE.md`. The one-case catalog hash is `a1f1aa5cabf652fc2bd150ec205483c3d39d61e4dfd455bb288263ee41d94b76`; both profiles and actual prepared settings passed signed readiness and the 43-glyph game-font check. Candidate uses native scale/copy; Baseline keeps all replacements off. Other replacements stay off and probes stay all. The previous apps/records remain unchanged. Use [NATIVE_DATA_CASE.md](NATIVE_DATA_CASE.md); the user does not repeat the four-case initial batch. Analyze actual native-data records under the explicit execution profile in NAT-004; no same-input or performance conclusion is inferred from native-only execution.

**VEC-002 is completed.** The portable four-operation core and owned adapter passed 12,060 full-state and 512-byte comparisons across Off/Verify/Native, including 3,720 native commits and 3,720 verified calls. The separate 5,516-call production AOT audit demonstrated why the interpreter cannot be the numerical oracle: rounding and NaN payload order differ. The new core follows actual AOT. Default-off behavior, fingerprint refusal, aliases, overlaps, numerical edges, unusual-prefix fallback, no speculative writes, original failure handling and mismatch persistence across reconfiguration are covered. The asset-free CTest also passed. See [VECTOR_METRICS_MODULE.md](VECTOR_METRICS_MODULE.md) and the source/binary identities in `out/testing/vector-metrics-module.json`. Implementation `1e17ef0` is published in draft PR [#17](https://github.com/etsusei/Yakumo/pull/17). VEC-003 subsequently integrated observation/mode dispatch; this offline gate itself does not establish live caller, performance or gameplay coverage.

**VEC-003 is completed after combined render-discovery acceptance.** The following preserves its earlier standalone delivery history. Nine probes and explicit v2 mode metadata are implemented. The owned metric dispatcher covers real same-unit local calls as well as registered and compiled chain calls; its internal AOT reference is suppressed from duplicate observation. The production runtime gate, rebuilt metric and legacy gates, four CTests and 101 Python tests pass. Two original user packages still validate under their five-field legacy identity. Independent B0 and candidate builds now pass 33 real-binary paired preflight/seal checks, and the original metric unit objects are identical. Their immutable binaries/manifests are archived under `out/testing/observed-builds/vector-observation-399754a/`. Source `399754a` is published in draft PR [#18](https://github.com/etsusei/Yakumo/pull/18). The combined render-discovery case subsequently supplied observations for three metrics; norm-squared remains unobserved. See [VECTOR_METRICS_OBSERVATION.md](VECTOR_METRICS_OBSERVATION.md). A separate signed `vector-discovery` pair built from `a223ed3` is ready with one localized case and four-Verify v2 profile, no required metric calls. Both native launchers passed preparation-only readiness, the C++/Python catalogs agree, independent save copies were verified, and both settings passed 43/43 game-font samples. Optional report windows distinguish call coverage from verification and native acceptance. That delivery order is historical and does not request a rerun; no apps were overwritten. See [VECTOR_DISCOVERY_CASE.md](VECTOR_DISCOVERY_CASE.md) and draft PR [#19](https://github.com/etsusei/Yakumo/pull/19).

The delivered NAT-002 source is `73abb64`; its B0 snapshot is `out/testing/observed-sources/B0-native-data-73abb64` with build at its `out/mhp3rd`. The current checkout may advance independently. Do not replace the delivered apps while the user tests. The inventory and native pilot do not establish migrated animation, AI, collision, quests or networking.

The pair pins an installed Python interpreter and local resource paths; it is not a portable standalone distribution. A successful preparation or complete record is not gameplay acceptance. The first user-led cases are recorded, and broader animation, AI, collision and combat claims remain pending relevant cases.

### Task ledger fields and update rules

Each task records its stable `id`, scope, dependencies, status, `created_at`/`updated_at` and start/completion times, actual owner/contributors, `work_summary`, deliverable paths, validation evidence and limitations, blockers/pause reason, and append-only transition history. Timestamps use ISO 8601 UTC; the project timezone is Asia/Tokyo. Unknown historical times stay null. Paths are relative to the repository root, and local-only artifacts are marked explicitly.

Allowed statuses are `not_started`, `in_progress`, `awaiting_review`, `awaiting_user_test`, `blocked`, `paused`, `completed`, and `cancelled`; their meanings are defined in the JSON. Only `completed` maps to a checked box. Recording an artifact as existing does not establish that its task has passed review or gameplay acceptance.

Before a handoff or context reset, refresh the current/next task pointer, actual work and deliverables, evidence, and unresolved limitations. Preserve historical attribution and entries. Keep this document's checklist synchronized with the JSON in the same change.

### Context-recovery checklist

Use this procedure after context compression or when another agent takes over. These are recurring checks, not additional implementation tasks.

1. Read `AGENTS.md`, this plan, and `tasks.json`; compare them with the current working tree before making changes.
2. Follow `execution_state.current_task_id`, then read that task's latest history, dependencies, deliverables, and validation limitations. An `in_progress` entry identifies unfinished work; it does not prove an agent or process is still running.
3. Check that the needed files and evidence exist. Treat a missing local artifact as unavailable until recovered or regenerated; retain the historical record of the original result.
4. Resume from the first unmet acceptance criterion. Do not repeat completed extraction, full builds, or gameplay navigation just to reconstruct context.
5. Record actual work and attribution, append history, refresh timestamps and continuation pointers, and synchronize the checklist before handing off again.

| Tracking requirement | JSON fields |
| --- | --- |
| Stable task number and dependencies | `id`, `depends_on` |
| Creation, latest modification, start, and completion | `created_at`, `updated_at`, `started_at`, `completed_at` |
| Actual responsible and contributing agents | `owner_agent_id`, `contributor_agent_ids`, `updated_by_agent_id`, `history[].actor_id` |
| What changed and why | `scope`, `work_summary`, `history[].description` |
| Deliverable location and availability | `deliverables[].path`, `state`, `local_only` |
| Progress and resumption conditions | `status`, `pause_reason`, `blockers` |
| Proof and remaining uncertainty | `acceptance_criteria`, `validation.checks`, `validation.limitations` |

The task ledger retains history rather than overwriting it with a new chat summary. Planning an output, compiling an application, and passing user gameplay acceptance remain separate facts.


### Resource boundary checkpoint

ASSET-001/002 recovered original count/offset/length accessors and retained-pointer
lifetime evidence, then implemented a portable shared-storage bundle view. All
6,043 raw entry hashes were checked; 2,168 root and 28 nested structural
candidates yielded 9,622 slots, including 555 absent slots. AOT and bounded
interpretation each completed 25,832 metadata queries with matching CPU/memory
state. The 24 core tests, six manifest checks and sanitizer run passed.
Signatures remain annotations: 11 candidate nodes have only bounds evidence,
and no child semantics or live loader replacement is claimed. Implementation `903b0f3` is published in draft PR [#20](https://github.com/etsusei/Yakumo/pull/20). See
[INDEXED_RESOURCE_VIEWS.md](INDEXED_RESOURCE_VIEWS.md). Continue ASSET-003 while
NAT-003/VEC-003 await user operation; preserve all delivered apps and raw assets.


### Encoded TMH checkpoint

ASSET-003/004 completed the static TMH consumer contract and portable owned
encoded views. Discovery includes 2,244 indexed children and 12 standalone
entries; all 8,866 descriptors match the production AOT and bounded interpreter.
The 27 core checks, five structural checks, two CTests and sanitizers pass.
Padding, standalone tails and unknown words remain intact. No pixels or game
were rendered. During review, the GE C2 branch was corrected before publication:
this builder selects swizzle1 for indexed4/5 and0 for DXT formats. A8 keeps the
raw width while B8 uses a verified ceil(log2) table. ASSET-005 must preserve the
stride/extent distinction and explicit context instead of guessing file layout.
See [OWNED_TMH_VIEWS.md](OWNED_TMH_VIEWS.md). Both user test pairs stay unchanged.


The encoded TMH implementation `ccdb564` is published in draft PR
[#21](https://github.com/etsusei/Yakumo/pull/21). See
`out/testing/tmh-native-validation.json` for source/executable identities.
