# User-requested pause checkpoint

Paused on 2026-09-27 at the user's explicit request to save a stage checkpoint,
report completed and remaining work, and stop execution. Do not resume the
goal, development, tests, agents or game launches without a new user request.
The authoritative checklist and statuses are in DEVELOPMENT_PLAN.md and
tasks.json. ASSET-014 and ongoing ITER-001/002 are paused, not completed.

## Completed foundations

- Local ISO/raw resource preparation and indexed resource provenance are
  established. Original image/save inputs remain unchanged and untracked.
- Chinese interface and the game-font regression were addressed; the user
  confirmed normal in-game text.
- Separate Baseline/Candidate delivery, input/selected-function observations,
  bounded recording and paired comparison workflows exist. The user owns
  gameplay acceptance; assistant screenshot navigation is not the test method.
- Native helper, resource-view, texture decoding and texture-command components
  have scoped offline evidence. The latest accepted render-discovery pair
  recorded132 matching texture decodes and three observed vector metrics.
  The native-data resolution rerun was waived by the user; its old strict
  configuration mismatch remains in the immutable report.
- PR #32 added owned non-mutating command plans. PR #33 connected original
  AOT entry/return boundaries, with32 boundary fixtures and18 actual caller
  tails plus the4,512-call corpus. These are not whole-game native acceptance.

## Current saved stage

Branch: `codex/texture-lifecycle-observation` (based on PR #33).

Implemented14 actual allocation/factory/caller/reset/free checkpoints and a
bounded `TextureLifetimeTracker`. It derives owner/command leases from real
original execution and issues a caller receipt once per observed frame.
Free/reset and reused addresses invalidate old generations. The receipt is
copyable metadata, not independent execution permission.

Last executed evidence:34 AOT/interpreter calls,8 original factory branches,
7 observed owner leases,7 caller tails,4 consumed receipts, ordinary/uncached
aliases, free/reuse/reset and3 loss cases. Full CPU/RAM/VRAM matches; maximum
193,515 interpreter slices. Loss cases cover missing constructor observation,
code changes and injected thread switch-away/back. No-command remains original.

The new metadata components and harness passed ASan/UBSan; runtime/AOT/DSO
objects retained production flags. Seven registry cases passed CTest; eight
lifetime Python tests,14 command Python tests and eight Baseline staging
tests passed. The unowned2,256-input/4,512-call corpus also passed with all
lifetime seams compiled. Independent review found no blocking scoped defect.

**Pending recheck:** after those runs, the test was tightened from any denied
source permit to exact `AuthorityError::NotReady` with healthy authority.
Both normal and sanitizer targets compiled successfully, but these latest
binaries were not rerun before the pause. Existing reports bind the preceding
source version and must not be represented as testing this final assertion.

## Remaining work, in order

1. Only after explicit resume: rerun normal and sanitized lifecycle gates for
   the final assertion and publish fresh source-bound reports.
2. Connect actual load requests, descriptors, read/copy/transform completion,
   terminal/cancellation observations and pending/late writers to source
   authority. Allocation or readable bytes alone cannot establish readiness.
3. Add coherent code epochs, complete observation coverage, serialization and
   exclusive writer checks. Then connect the authority-backed Off/Verify/Native
   controller and matched recording, including original fallback on lost proof.
4. Build a related combined user test batch after offline prerequisites pass.
   No additional manual test is currently requested.
5. Broader animation, actions, collision, AI, combat, quests and networking
   migration remain future work; they are not completed by these resource gates.

## Preserved state and stopped work

No new application was delivered or enabled. The game was not launched during
this work; accepted user recordings and old app bundles were not changed.
All spawned agents were confirmed completed. The remaining known build and
corpus-test handles returned successful completion during pause cleanup; no
new test or development run was started after the pause request.
The pre-existing untracked `.c2cignore` is left untouched.

Local evidence: `out/testing/texture-lifetime-observation-gate.json`,
`out/testing/texture-lifetime-sanitized.json`,
`out/testing/texture-lifetime-sanitizers.log`, and
`out/testing/texture-lifetime-unowned-corpus-gate.json`.
The ledger records artifact hashes and actual agent attribution.
