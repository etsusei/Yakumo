# MHP3 Native Migration and Paired Validation Plan

Plan version: **1.1**. Decision date: **2026-09-26**. Task ledger: [tasks.json](tasks.json).

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

### What already exists

- Chinese interface work: commit `e5a5d9a`, draft PR [#1](https://github.com/etsusei/Yakumo/pull/1).
- Native helper experiments: implementation commits `26398fe` and `f60e77f`; evidence recorded in `4292eb6`, draft PR [#2](https://github.com/etsusei/Yakumo/pull/2).
- The supplied image passed supported executable checks. Its archive has **6,043 entries and 355 code overlays**. All overlay header/code fingerprints matched the release libraries.
- The supplied save passed hashes, decryption, and round-trip checks. Testing used independent copies and verified the source archive/copy unchanged.
- Angle helper: **806,432 offline differential cases passed**, but **zero calls** were observed on the tested village route. In-game coverage remains absent.
- Scale-matrix helper: **100,512 offline cases passed**, including 10,000 prefix fallbacks. Live verification compared **48,365 calls with zero mismatches**; a separate native run used **48,405 calls with zero fallback**.
- The tested route was read-save, character selection, village entry, and a short walk. Audio output, combat/quests, multiplayer, other platforms, and long-session stability were not verified.
- Full raw resource extraction, the paired recorder, case panel, and comparison pipeline **have not been implemented**. Existing input scripts inject input; they are not a complete recorder.

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
- [ ] **RES-002** — Build and test a guarded resource-preparation command around the existing ISO/DATA.BIN readers.
- [ ] **RES-003** — Extract the full raw image contents and DATA.BIN entries locally; generate and validate the manifest.

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

- [ ] **OFF-001** — Standardize helper contracts, feature switches, fallback behavior, and evidence counters.
- [ ] **OFF-002** — Add the translation-matrix leaf at `0x08878B4C` (36 bytes including the return delay slot).
- [ ] **OFF-003** — Add the four-word vector constructor at `0x08877818` (24 bytes including the delay slot).
- [ ] **OFF-004** — Add the nine-word matrix-layout copy at `0x08879D08` (80 bytes including the delay slot).
- [ ] **OFF-005** — Add independent synthetic ISO-reader and PSMF-demuxer regression coverage.
- [ ] **OFF-006** — Run the complete offline gate and produce a versioned coverage report before manual handoff.

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

- [ ] **OBS-001** — Implement the versioned event journal, bounded buffering, crash-prefix recovery, and offline lifecycle tests.
- [ ] **OBS-002** — Connect observational input, timeline, camera, and overlay-identity sources.
- [ ] **OBS-003** — Connect certified probes, read-only state observations, performance summaries, and errors.
- [ ] **OBS-004** — Implement record packaging and the offline comparison/report command.
- [ ] **PAIR-001** — Add the Chinese in-game case panel without exposing state-changing cheat controls.
- [ ] **PAIR-002** — Package separate Baseline/candidate Mac applications with the same recorder revision and isolated run data.
- [ ] **PAIR-003** — Validate preparation/export behavior using synthetic child processes and publish the first case pack.

Build and test the tooling using constructed events, fake clocks, temporary files, synthetic overlays, and subprocesses that exit or crash. Real-game hook coverage and UI usability remain pending until user acceptance; an offline test of the recorder must not be presented as validation of its live integration.

**Exit:** both applications, the case pack, automatic recording/export, and the comparison command are ready for the user. This is readiness for manual testing, not a manual pass.

### Milestone 3 — First user-led acceptance batch

- [ ] **CASE-001** — User completes the initial Baseline/candidate batch, totaling 15–30 minutes.
- [ ] **CASE-002** — Analyze the records, resolve defects, identify missing coverage, and publish the acceptance report.

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
7. Log reassignment and preserve history. Future subagents use `fork_turns=none`, `model=gpt-6-sol`, `reasoning_effort=max`, with complete task context. Earlier audits used a different model under the then-current instruction; preserve that historical fact.

### Resume here

Once `PLAN-002` is completed, the next task is **RES-001**, followed by **RES-002** and **RES-003**. Do not skip directly to animation/AI rewriting, broad game navigation, or a tools-only first handoff. Completing this documentation task does not start those implementation tasks automatically.
