# Observational game event integration

OBS-002 connects the [session journal](SESSION_RECORDING.md) to actual host observation points. OBS-003 supplies separately certified function probes; source labels and dispatch durations alone are not certified call/timing evidence. The initial workflow remains offline-first and user-led for gameplay acceptance.

## Runtime transport

Recording is opt-in. With `MHP3RD_RECORD_DIR` unset or empty, no recorder is created. The forthcoming paired launcher supplies these fields:

| Variable | Contract |
| --- | --- |
| `MHP3RD_RECORD_DIR` | New run directory; parent must exist, existing paths and symlinks are rejected |
| `MHP3RD_RECORD_ROLE` | `baseline` or `candidate` |
| `MHP3RD_RECORD_RUN_ID` | Nonempty safe ASCII identifier, at most 96 characters |
| `MHP3RD_RECORD_BATCH_ID` | Nonempty safe ASCII identifier, at most 96 characters |
| `MHP3RD_RECORD_BASELINE_ID` | Reference identity, default `B0` |
| `MHP3RD_RECORD_BASELINE_COMMIT` | Reference source commit, default `4292eb6` |

Start/stop belong to the application main thread. A new process needs a fresh run directory; restarting into an existing recording directory is not a valid new run. The application rejects an enabled Baseline recording if any native replacement is requested. The configured role is intent, not proof of binary provenance; paired packaging must verify source/build/binary identities independently.

`RunBegin` carries role/run/batch/reference identities, build version, binary hash, native mode requests, compiled renderer availability, relevant diagnostic-environment presence and an observer-source revision. CMake computes the revision from the journal/observer sources, the observed control-delivery adapter and the probe instrumenter. It describes the recording implementation, not equality of the two gameplay builds. The observer stream schema remains `observers-v1`.

Recording starts before locating assets so setup UI events can be observed. A later `runtime.inputs` event records the actual ELF hash and supported-executable result. Complete ISO/starting-save/package identity validation is still a paired-delivery requirement; current metadata explicitly leaves those identities pending. These development logs are not yet a ready manual acceptance package.

Startup validates metadata before creating a fresh directory and flushes RunBegin before publishing its observer. Shutdown detaches routing, emits `observer.health`, drains the recorder and reports errors. A normal window/menu close is recorded by stop reason; the existing nonzero window-close exit code is not automatically a crash. Exceptions and unfinished scope destruction remain explicit. A valid footer does not override lost events, failed health or missing input identities.

## Event semantics

All observer records include event name, effective input domain, guest-frame ordinal, virtual microseconds, vblank ordinal, successful control-read ordinal, observation ordinal and focus. Virtual time/vblank are null until known. The observer serializes submissions so observation ordinals preserve accepted order; the recorder separately numbers all framed records.

- **Frames and time:** `present_frame` advances the observer's game-frame ordinal at the guest framebuffer flip. Interpolated renderer presents do not add game frames. The kernel vblank hook updates virtual time without adding frames.
- **Applied pad:** the controller hook records buttons, left/right axes and sample count after the HLE has performed camera/aim rewrites and written the guest buffer. The isolated delivery helper preserves the original 16-byte sample layout, including the HD second stick, timestamp truncation and reserved bytes; partial failed delivery emits no successful sample. One ordinal describes one successful read call; it is not a count of window events or rendered frames.
- **Window input:** SDL normalization runs after the UI event hook so `ui_consumed` reflects routing. Window IDs and focus gate controls. Gamepad controls have no SDL window ID and are routed only while the game's window is focused. Device connection and process-wide quit metadata are explicit global events. Owned-window geometry changes remain observable without focus; their raw SDL event type distinguishes resize/pixel sizes from display IDs and other event data. Other-window controls are excluded.
- **Domains:** setup, game, paused UI and over-game UI remain distinct. Text input temporarily overrides the underlying domain. Nested UI restoration preserves the underlying state without reopening an already closed text domain.
- **Camera:** actual rate/motion submission, effective time advance, aggregate/source consumption and discard/reset operations are observed. Non-finite observation values are explicitly unavailable. Recording never consumes camera input a second time.
- **Scripts:** injected actions have a script-frame marker. Safe control arguments are retained; text, dropped paths and capture paths are not copied into the journal. `scripted_mode` identifies an active override, not proof that every window event came from the script. Existing stdout diagnostics are separate from the structured journal.
- **Settings:** changed effective settings are recorded with a canonical SHA-256. Names, addresses and paths are fingerprinted rather than copied into structured events. A full settings hash is diagnostic; a future case comparator must select relevant prerequisites rather than treating unrelated UI/history differences as gameplay defects.

SDL source timestamps are labeled separately from host monotonic record timestamps. Manual runs are not deterministic replay; input events, applied pad samples and camera consumption describe different stages and cannot be collapsed into a single event count.

## Overlay identity and validation epochs

Overlay snapshots preserve base address, declared image/code sizes, observed name, header fingerprint and header-plus-code fingerprint. Fingerprints are explicitly labeled FNV-1a-64, matching the runtime corpus matching scheme; they are not presented as cryptographic authenticity guarantees. Mutable overlay data is excluded from code identity.

An observed guest instruction-cache invalidation starts a new code-validation epoch. Snapshots are refreshed then, including identical images, so an address is not silently reused under its old identity. Generations are conservative validation boundaries, **not a measured count of loads**. A same-image cache invalidation may advance generation even without a reload. Missing/malformed/unmatched identities cannot authorize attributed state reads; overlapping active ranges are ambiguous and return no identity.

The observer hashes code on validation/install boundaries, not every rendered frame. Its 8 MiB per-image code limit bounds observation work; the registered image's largest overlay code is 1,163,800 bytes. This is an observer limit, not a universal file-format claim. Observation does not alter corpus installation, guest memory or execution.

## Verified offline integration

On 2026-09-27 (Asia/Tokyo), Apple Silicon macOS:

- Twelve headless CTests passed: observer/runtime, actual camera/settings hooks, controller delivery, overlay identity, existing camera/settings/localization regressions, and the probe-instrumenter suite.
- The renderer-enabled SDL decoder/forwarding suite passed without SDL initialization or a window.
- AddressSanitizer/UndefinedBehaviorSanitizer passed five observer/delivery/identity suites; ThreadSanitizer passed the observer/runtime suites.
- The full renderer-enabled application compiled and linked without launch.

The controller test compares enabled/disabled delivery byte for byte, includes RAM aliases, timestamp truncation and canaries, and ensures partial failed writes do not create a successful read record. Real camera methods preserve their returned turns with active, disabled and closed recording. Settings tests verify change detection and structured-event redaction. Overlay tests use synthetic guest memory, including unchanged-header code edits and mutable-data edits.

No full game was started. Actual gameplay hook coverage and user acceptance remain pending. ISO/starting-save/package identities still need paired delivery, and restarting requires a fresh run directory. Observer metadata does not turn manually played sessions into deterministic replays.

Local evidence is recorded in `out/testing/game-observation-validation.json` and its referenced logs. OBS-002 implementation is complete; OBS-003 certification remains in progress.

The generated-source instrumenter for OBS-003 has eight passing synthetic tests and accepted the current ignored units in a read-only inspection. Its callbacks, runtime fingerprint gates, state/performance readers and compiled AOT validation remain pending. No timing certification is claimed from source labels alone.

## OBS-003 implementation handoff

The audited direct AOT path bypasses some runtime chain observers and can continue inside one generated unit after a leaf returns. Outer-dispatch duration is therefore not a leaf duration. The current source-shape instrumenter inserts entry calls after the certified entry label and exit calls after the return delay-slot effect, immediately before `local_pc = jump_target`. It passes the entry and target explicitly because `ctx.pc` can be stale during local dispatch.

| Leaf | Current unit | Entry | Return labels |
| --- | --- | --- | --- |
| Angle | 0028 | `0x088775AC` | `0x088775F4`, `0x08877608` |
| Vector | 0028 | `0x08877818` | `0x08877828` |
| Scale | 0029 | `0x08878B28` | `0x08878B44` |
| Translation | 0029 | `0x08878B4C` | `0x08878B68` |
| Copy | 0029 | `0x08879D08` | `0x08879D50` |

CMake must discover units from the original corpus before substitution, including its existing camera-helper lookup. Substitute build-local copies of only affected units, retain their generated compile options and original generated include path, and add the probe header path only for those copies. Changing target-wide include flags would unnecessarily rebuild every large AOT unit. No generated code is committed.

Remaining work before certification:

1. Implement the noexcept callbacks declared by the instrumenter in `testing/probes.hpp/.cpp`, with full loaded-ELF/span fingerprint gates, logical entry/return counts, duration summaries and explicit incomplete scopes. Preserve guest state and original AOT chaining.
2. Observe candidate native bridges separately and label AOT, native, verification and interpreter fallback paths. Verification timings are not native performance evidence.
3. Compile and test instrumented copies against bounded original-code expectations, including both angle returns, delay-slot ordering, same-unit chaining and abnormal exits. Source-label tests alone are insufficient.
4. Use `debug/game_state.hpp` pure const-Ram readers, not the debug tool's mutable runtime/cheat path. Base fields require the supported ELF and valid character state; quest fields require matching active overlay code identity and validation generation, beyond the existing header/name-only `on_quest` check.
5. Record each new valid `perf::last_second()` summary once using its second ID. Keep observer overhead and recording modes explicit. Full live coverage remains a later user case.
