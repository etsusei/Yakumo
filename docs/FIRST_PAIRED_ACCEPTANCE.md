# First user-led paired acceptance

The user completed both delivered applications and reported completion on 2026-09-27. The original packages are retained locally. All four cases in both roles have explicit normal user outcomes, all five checkpoints are present, and both recordings closed cleanly through the menu. This establishes the first live recording/analysis workflow and useful helper coverage; it does not establish whole-game equivalence.

## Evidence and findings

The candidate gameplay and observation source is `75ca5ff`; Baseline gameplay is the frozen `4292eb6` plus the same observation revision. Catalog hash: `99f868d6cf0a9a465c1720b8f752a73555b645c7f6e4fc1f08f9691de568a48a`. The independent package reader found matching prerequisites, complete CRC/sequence framing, and zero dropped/invalid events or structured diagnostics. Checkpoint samples were fresh and showed a loaded character.

| Helper | Baseline calls inside cases | Candidate verified calls inside cases | Result |
| --- | ---: | ---: | --- |
| Scale matrix | 310,936 | 511,126 | No mismatch, fallback, incomplete scope or uncertified return |
| Nine-word matrix copy | 5,864 | 9,772 | Same |
| Angle step | 0 | 0 | Not covered in live play |
| Translation matrix | 0 | 0 | Not covered in live play |
| Vector constructor | 0 | 0 | Not covered in live play |

NATIVE-01 alone contains 106,022 Baseline scale calls and 174,690 candidate scale verifications, exceeding its required coverage. Candidate whole-run totals are 588,894 scale and 20,595 copy verifications; these include startup and activity before the marked cases. Totals were computed per counter epoch, not by adding cumulative snapshots or interpreting the last snapshot as the entire run. All candidate modes were `verify`, with zero committed native-mode calls. These checks validate supported same-input leaf results, not native-mode performance or all callers/scenarios.

The original automated report remains `inconclusive`: REC-01/03 have different manually generated input streams, while REC-02 and NATIVE-01 contain transient settings changes. No report outcome has been rewritten into a pass. The setting was internal rendering scale: Baseline changed 2 to 3 to 2 during REC-02, and candidate changed 2 to 1 to 2 during NATIVE-01. Both changes and restorations occurred in paused UI at an unchanged guest frame. They require a limit on fixed-settings comparison but do not create evidence of a native-helper mismatch.

The main gameplay-case samples were near 30 frames/s and 100% emulation speed. Both applications were launched together at the user's request and were operated manually; elapsed times, input counts and verification overhead differ, so no speedup is inferred. Ten bounded probe-detail gaps per role reflect the 32-detail retention window, not journal loss. Both roles also logged the same 23 missing-font-glyph notices and a duplicate MoltenVK class warning; these are not established regressions in the candidate. The candidate interpreter counts are its original-code verification oracle, while helper fallback counters remained zero.

## Recorder follow-up

Live evidence exposed a validation gap: the case controller checked settings only at markers, allowing a change restored before the next marker to retain a normal UI outcome. The offline comparator correctly retained the settings events and requested review. The fix observes settings saves/snapshots immediately and keeps an affected case interrupted even after restoration; excluded navigation/instance history does not interrupt the controller. Direct transient mutations with no save/snapshot remain observable only at later markers.

When an active case reopens the menu, the updated UI selects Test session initially instead of Video. This reduces repeated section navigation and exposure to the resolution control. An explicit Chinese message explains that restoring a setting does not restore an interrupted case's validity. These changes are for a future observation build; the delivered apps and submitted records are preserved.

The regression is tested offline using actual settings saves/snapshots and the C++ case/journal pipeline, including change-and-restore and excluded navigation fields. No repeated user game session is needed to establish that guard behavior. Live usability of the updated menu remains pending a later related batch. The original comparator conservatively requests review for configuration snapshot events, including excluded navigation changes; it never promotes the interrupted scenario to a pass.

## Local artifacts and next scope

- Baseline package: `out/testing/runs/packages/run-8257f90ededf45aeab8ce60b126361c5`.
- Candidate package: `out/testing/runs/packages/run-4393c2b8a01440558f76164235cdaeef`.
- Original machine/HTML comparison: `out/testing/reports/initial-user-acceptance-1/`.
- Reviewed epoch-aware analysis: `out/testing/reports/initial-user-acceptance-1/analysis.json`.

The user's normal observations and the two helper verification results are retained as scoped evidence. The three untriggered helpers stay explicitly uncovered; native replacements are not enabled by default. Combat, monster AI, quests, networking, long sessions and broader resource behavior remain outside this route. The next migration inventory must distinguish this verified leaf coverage from guest-independent subsystems and prepare finite related batches with their own evidence obligations.

## Subsequent visual defect report

The user subsequently reported that Chinese game text was missing in both roles.
The shared missing-glyph notices were evidence of a real delivery problem: the
isolated settings had dropped the previously selected Chinese game font. The
normal case marks remain the original observations; they do not override this
later report. [The font correction](GAME_FONT_REGRESSION.md) restores that
configuration and verifies the actual missing glyphs. The two same-input helper
results remain valid, but no blanket visual-compatibility pass is claimed.
