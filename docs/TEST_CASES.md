# First paired case pack

This is the PAIR-003 case pack for the first user-led Baseline/candidate run. The
catalog is [initial_batch.json](../profiles/mhp3rd/testing/cases/initial_batch.json).
It defines four finite cases in order: `REC-01`, `REC-02`, `NATIVE-01`, and
`REC-03`. The target is approximately **8–15 minutes per application**, or
**15–30 minutes for both** including launch and collection. This is a planning
estimate, not a measured play time. Live usability, helper coverage, and game
behavior remain unverified until the user performs the two runs.

## Before either run

Run **Baseline first, then Candidate**. The two launchers prepare independent
copies of the same registered starting save; they do not modify the original.
Choose the **first occupied hunter slot** in each copy and keep its starting
equipment. Use the same physical keyboard and mouse, or the same controller,
for both roles. Do not switch devices between roles, change settings, create a
character, take a quest, fight, or enter a network hall. If the first occupied
slot is missing or cannot load, stop this run and report the screen reached;
there is no need to create a replacement hunter or retry repeatedly.

The instructions use the shipped controls. `W/A/D` move forward/left/right,
and `J` asks the village camera to move left; the equivalent gamepad controls
are the left and right sticks. `Esc` or pressing both sticks (`L3+R3`) opens
Yakumo's menu. `Q/W` or `L1/R1` changes its section, and the **Test session**
section is the last one. Use the **Choose case** row's left/right control to
select the next case. The menu's confirm button activates the focused row; on
the shipped layout it is the right face button on a controller. Its footer
shows the current controls. Keep the same control layout for both roles.

At the title screen, select the first occupied slot using the game's prompts
and wait until the hunter is standing in the village. If selection or loading
cannot reach that state within about two minutes, close the window normally
and report the point where it stopped. The case panel cannot cover guest
startup before it is available: loading events belong to the **whole-run
record**, while `REC-01` begins after loading. Do not start `REC-01` on the
title or loading screen.

## Panel actions used by every case

Open **Test session**, select the named case, read its steps, then choose
**Begin this case**. The panel closes automatically. At each named checkpoint,
open the menu again and choose **Record next checkpoint**; this also returns
to the game. Reopen the panel after the last checkpoint and choose **Mark
normal**, **Mark abnormal**, **Mark uncertain**, or **Skip this case**. Normal
is available only after every checkpoint. Closing the menu by itself keeps the
case active. **Mark a problem** records an anomaly and returns to the game;
reopen the panel to finish with the appropriate outcome. A problem mark does
not end the case.

Use *normal* for the stated actions and observations with no apparent problem,
*abnormal* for a visible fault, *uncertain* when the result cannot be judged,
and *skipped* when the actions cannot be carried out. If a wall, door, or
unexpected transition interrupts a movement segment, release the input and
stop that segment. Do not force passage or repeat the route. A harmless
obstacle can be reported as uncertain if it prevented the intended observation;
a clear fault can be marked abnormal. The assistant will analyze the records
and any concise observation after both runs. The user does not need to inspect
raw logs or run commands.

## Finite case sequence

| Case | Start and operations | Checkpoint and stop condition |
| --- | --- | --- |
| `REC-01` | After loading the first occupied hunter into the village, leave controls idle for two seconds. Open Test session and begin. Check that the same hunter and village scene remain visible; do not move or alter equipment. | Record **Village visible**. Reopen the panel and mark an outcome. If a loading screen remains or the hunter is absent, mark abnormal or uncertain and stop this case. |
| `REC-02` | Begin with that hunter standing in the village. Hold forward for two seconds, release for two; hold left for one second and release. Stop early at a wall or door. Hold camera-left for one second and release. | Record **Movement and camera done**. Open Yakumo's menu once more, press Down then Up once with arrow keys or D-pad, and close it with Esc or L3+R3. Reopen Test session, record **Menu round trip**, then mark an outcome. Menu navigation should not move the hunter. Village camera response may vary, so report what was visible instead of assuming a turn. |
| `NATIVE-01` | Begin where `REC-02` ended, with the same hunter and starting equipment. Inspect the visible hunter model, outfit or weapon. Hold forward for two seconds, release for two; hold right for one second, release, then wait three seconds. Stop early at a wall or door. Do not enter it, start a quest, or retry the route. | Record **Native route finished**, then mark an outcome. This is a bounded discovery route. A normal human mark does not establish helper coverage. |
| `REC-03` | Begin only after `NATIVE-01` has an explicit outcome. Stay in the village without moving or changing settings. | Record **Ready to close**, reopen Test session and mark an outcome. Only after `REC-03` has ended, close the game window normally with its close control or `Cmd+Q` and wait for the launcher to collect the record. Do not force quit. |

If a previous case cannot be completed, finish it as abnormal, uncertain, or
skipped before choosing another. `REC-03` can still capture a clean closure if
the village remains available. If the village is no longer available, mark
`REC-03` skipped and close normally. Never leave an active case running at
window close merely to claim a normal completion. If the panel says recording
needs attention and disables outcome buttons, close normally and report that
message; the interrupted case and incomplete record must remain visible.

The expected visible observations are deliberately narrow. In `REC-01`, the
hunter and village should appear without a stuck loading screen or missing
scene. In `REC-02`, unobstructed movement should start and stop with the input;
menu navigation should leave the hunter still. The village camera may respond
differently from a quest camera, so describe its response rather than treating
no turn as a failure. In `NATIVE-01`, the model and starting outfit or weapon
should remain whole while the hunter moves and stops. In `REC-03`, the panel
should show active recording before the case ends and the launcher should
report that the record was collected after normal window closure. If it says
collection needs review, report that result without opening raw logs.

## Evidence boundaries

All four cases ask for a human observation. The panel records case boundaries,
checkpoints, input and user marks. `REC-01`, `NATIVE-01`, and `REC-03` require a
non-null `character_loaded` checkpoint field; this checks the supported
reader's observation, not the hunter's visual identity. Loading before
`REC-01` and normal window closure after `REC-03` are checked at run level.
The panel defaults to pausing the game while open; either way its input is
consumed by the menu. Checkpoints are observations at their marker, not claims
that game actions occurred while the menu was open.

`NATIVE-01` requires at least one complete, certified scale-matrix call at
`0x08878B28` **in each role inside the case boundary**. This helper was
historically observed on a village route; the first live run still has to
establish coverage here. The Candidate launcher requests `verify` for all five
helpers, Baseline keeps every replacement off, and both record all five probes.
Angle, translation-matrix,
vector-constructor, and matrix-copy calls are discovery observations with no
minimum. Zero calls for any of them means **not covered**, not passed.
Candidate verification is a same-input comparison against the original leaf;
it does not prove a whole-game match or native speedup. Any incomplete,
uncertified, mismatched, missing, or unpaired evidence remains explicit in the
post-run comparison.

The short manual route does not validate quests, combat, networking, save
writing, long sessions, or deterministic replay. After both applications
finish, the assistant checks identities, run completeness, case boundaries,
probe deltas, available state and performance observations, and the user's
marks before publishing an acceptance result.

## Offline delivery gate

The candidate CTest suite includes the catalog/localization checks, the actual
catalog rendered through the headless panel, both case-controller pipelines,
and packaging/supervisor checks. The full-catalog pipeline uses synthetic
memory and input: it must keep NATIVE-01 `not_covered` and a midcase close
`incomplete`. It cannot supply gameplay acceptance.

After assembling both apps with this catalog and matching observation builds,
run `profiles/mhp3rd/tools/check_pair_readiness.py` with `--pair-dir`, `--catalog`,
`--fixture`, and a new `--output` JSON path. The fixture is the compiled
`mhp3rd_case_runtime_fixture` target. This gate checks signatures, launch
identities, the actual C++/Python catalog interpretation, both real native
launchers in headless preparation-only mode, independent save files, and shared
case prerequisites. It never invokes ordinary game launch. A ready report
means the pair is ready for the user's cases, not that those cases passed.
