# Native module contracts and evidence

This inventory implements the boundaries in [the development plan](DEVELOPMENT_PLAN.md). Task status is in [tasks.json](tasks.json). Portable data functions and guest ABI adapters are distinct: a native helper behind an adapter still depends on the guest runtime. Call totals are not a whole-game migration percentage.

## Architecture inventory (ITER-001)

This inventory distinguishes existing host implementations from migrated game behavior. The source references below identify the boundary being assessed; they do not imply that all of the original game's callers or data semantics have been recovered. Baseline remains the source-frozen behavior reference. Current live evidence comes from the first user pair at observation/candidate commit `75ca5ff`.

| Area and source anchor | Current implementation | Residual dependency and next useful boundary |
| --- | --- | --- |
| Nine math/data operations: `host/native/` | Portable arithmetic/word operations with separately registered adapters | Adapters still consume GPR/FPR/VFPU state, guest addresses and return PC. Unsupported inputs retain an original-code reference. Group related operations into a bounded module, then migrate callers; do not count hooks as an entire transform engine. |
| ISO and archive directory: `host/kernel/iso_image.*`, `host/mods/mhp3rd_data_bin.*` | Native file/range reader, archive directory, deobfuscation and mod view | File HLE still presents PSP handles and writes guest buffers. Raw extraction is complete; typed model/animation/quest semantics are not supplied by the archive parser. Reuse these codecs rather than rewriting them. |
| Asset consumers: `host/hle/hle_io.cpp`, `generated/`, `host/overlays.cpp` | Native byte delivery plus a separately validated shared-storage indexed-bundle API in `host/resources/` | The original consumers own resource interpretation and lifetimes in guest memory. The new bundle view validates relative index spans and retains parent storage; child types and the source-ID-to-consumer edge still need recovery before replacing game consumers. See [INDEXED_RESOURCE_VIEWS.md](INDEXED_RESOURCE_VIEWS.md). |
| Texture data: `host/gpu/texture_decode.*`, `texture_pack*`, `replacement_textures.*` | Native format decoding, cache/replacement infrastructure | Original game/GE state supplies PSP addresses, palette formats, layouts and texture commands. Test decoded samples offline; material/scene correctness requires the corresponding live case. |
| Geometry and rendering: `host/gpu/ge_state.*`, `vulkan_renderer.*` | Native GE command interpretation and Vulkan backend | GE and render targets still reference guest memory and PSP command semantics. Vulkan through MoltenVK is not removal of the GE dependency. A future scene renderer requires native draw/scene ownership and geometry/resource interpretation. |
| Sound: `host/audio/sas_core.*`, `atrac_decoder.*`, `audio_sink.*` | Native mixing, envelope/decoder state and host output | `SasCore::render` and its source decoders still take GuestMemory and guest voice addresses; HLE and game scheduling drive them. A source-buffer interface can isolate that dependency, but reworking this host service is not migration of original combat/animation logic. |
| Movies: `host/movie/psmf_demuxer.*`, `avc_decoder.*`, `host/hle/hle_mpeg.cpp` | Native demux/codec handling | Guest ring buffers, PSP MPEG structures, timestamps and scheduling remain at the adapter. Synthetic parser checks do not prove playback/synchronization. |
| Save persistence: `host/save_data/`, `host/hle/hle_savedata.cpp` | Native crypto, SFO/container I/O, isolated writable saves | PSP utility structs and guest buffers still connect the original game serializer. Retain save format compatibility; a future native gameplay model must own its serializer contract before dropping that adapter. |
| Input devices: `host/input/`, `host/hle/control_delivery.*`, `host/hle/hle_media.cpp` | Native device mapping and recorded final pad delivery | The final samples still enter PSP control buffers; original game code decides motion, attacks and state transitions. Native input mapping is not native player behavior. |
| Camera integration: `host/camera/game_camera.*` | Host mouse/stick adaptation with fingerprinted hooks | Hardcoded camera/hunter fields and a return address connect to the original rotation helper. Original transforms and terrain/wall collision still execute. Keep the complete camera/collision dependency visible when selecting a future camera batch. |
| Font rendering: `host/fonts/game_font.*`, `host/hle/hle_font.cpp` | Native glyph selection/rasterization; restored Chinese font configuration | PSP font structures, glyph-cell metrics and the original game's atlas/layout remain. Font configuration is part of paired prerequisites; interface language is a separate setting. |
| Networking: `host/adhoc/`, `host/hle/hle_adhoc.cpp` | Native sockets/server/discovery and PSP ad hoc compatibility | The original game protocol and guest buffers/timing still define session behavior. Modern authoritative game replication requires a native game-state contract; a room-code frontend alone does not provide it. |
| Runtime, kernel and overlays: `include/psprecomp/runtime.hpp`, `host/kernel/kernel.*`, `host/overlays.*` | Native implementation of the PSP execution environment | Architectural CPU/memory state, PSP threads/waits/interrupts and original code overlays remain essential. Remove these incrementally only after their game consumers have migrated. |
| Player, monsters, combat, quests and animation: `generated/`, loaded overlay corpora | Original logic compiled to host code; selected fields observable through `host/testing/state_observation.cpp` | No recovered native authoritative subsystem is established by the current host modules. Read-only state readers and debug writers are observations/tools, not replacement gameplay logic. Contracts, resource meaning, timing and side effects need separate evidence. |
| Test/diagnostic UI: `host/testing/`, `host/ui/test_session_screen.*` | Native case/record/report infrastructure | Records known behavior and coverage; it does not implement the game. Keep tool readiness, helper verification and user acceptance separate. |

Paths in this table are relative to `profiles/mhp3rd/` unless they begin with `include/`. Local generated/overlay data remains ignored. Unnamed gameplay boundaries are explicitly unresolved; address proximity or a source filename is not sufficient to label a function as animation, AI or collision.

### Current live evidence and rollout boundary

The first user pair observed 511,126 scale and 9,772 matrix-copy verification calls inside the marked cases, with no recorded mismatch/fallback/incomplete scope. Both roles received normal user marks. Angle, translation and vector helpers were not called. Transient paused-menu rendering-scale changes and differing manual streams remain limits on route equality; see [FIRST_PAIRED_ACCEPTANCE.md](FIRST_PAIRED_ACCEPTANCE.md).

The candidate retained original results in `verify` mode. Moving a selected helper to `native` is a separate delivery contract: native call coverage, direct/chained dispatch interception, refusal/fallback behavior and explicit off switches must remain observable. `reference_verification=not_covered` is expected when native execution does not run an in-process reference; it must not be relabeled as same-input verification. Existing differential evidence and a native-mode user case provide different parts of the argument.

### Selection rules for the next finite batches

1. Select a coherent boundary with known inputs, outputs, memory ownership, side effects and callers. State which guest dependencies are removed and which remain.
2. Reuse existing portable host codecs/services. Adding tests to an already native component is assurance; it is not new game-logic migration.
3. Prefer raw-bit/data operations with independent local-original-code comparisons before floating-point, scheduling or stateful behavior whose oracle is weaker.
4. Keep untriggered leaves off or explicitly discovery-only. A static call site proves a possible call relationship, not a tested gameplay trigger.
5. Batch related changes behind independent switches, run the offline gate, then prepare the smallest relevant user cases. Existing recordings are reusable only under compatible font/configuration, catalog and observation identities.
6. Retain Baseline plus the common observation revision. Do not mutate or repackage the applications the user already tested as a substitute for new versioned evidence.

## Static callers and the next module candidate

The supported ELF and all 355 manifest-bounded overlay code spans were inspected without starting the game. The local derived evidence is `out/testing/inventory/uncalled_leaf_audit.json`; it contains addresses, counts and hashes, not instruction bytes.

| Existing uncovered leaf | Base executable JAL sites | Overlay code matches | Interpretation |
| --- | ---: | --- | --- |
| Angle `0x088775AC` | 17 | No direct J/JAL match | Referenced by original base code; first route still has zero observed calls. |
| Translation `0x08878B4C` | 1 | 6 JAL matches in 3 variants | Includes `game_task.ovl` and `em046m0/m1`; this does not identify a tested user trigger. |
| Vector constructor `0x08877818` | 2 | 683 JAL and 9 J-shaped matches across 68 variants | Broad static references; duplicated variants, embedded data and unresolved indirect calls prevent a dynamic-coverage claim. |

For example, the translation caller chain includes a data pointer to `0x089219A0`, a call at `0x08921BBC`, and an unresolved indirect call at `0x08921BF8`. A vector path runs through `0x0882C674` and `0x0882C3B4` to the call at `0x0882C52C`. These are evidence for follow-up analysis, not labels such as monster AI or animation inferred from location. Zero live calls remains uncovered; it does not justify deleting these functions.

The implemented **three-dimensional vector metric module** has these original boundaries:

| Candidate operation | Entry | Full span including return delay | Original-code SHA-256 |
| --- | --- | ---: | --- |
| three lane norm | `0x08877244` | 32 bytes | `1b1da8d38edcbeb7eb486e12977c6a667fa699639b6ef8d8d7d3f664219b12a0` |
| three lane norm squared | `0x08877264` | 28 bytes | `1fa5cdbdd2f96479f4cecb659eb7f68dd95a82ef853f99d7a299dc707c241cc1` |
| three lane distance | `0x08877280` | 40 bytes | `977da7d41ecfd722f43c497c0f4627bb4faa7d57126d1cefa8f92138115b02a1` |
| three lane distance squared | `0x088772A8` | 36 bytes | `05771b861950457e9e065384220c69cc255874d63fe09e62face44defea00d36` |

These four local spans use three-lane VFPU operations, optional subtraction/square root, stack scratch and FPR0 output, with no nested call/HLE site in the inspected spans. The primary agent independently recomputed all four span hashes from the ELF program headers. The bounded software-interpreter contract is now recorded in [VECTOR_METRICS_CONTRACT.md](VECTOR_METRICS_CONTRACT.md), with 11,604 original-instruction runs and explicit prefix, scratch, alias and numeric evidence. VEC-002 subsequently passed production-AOT/native differential gates, and VEC-003 connected all four metrics to owned dispatch and recording. A separate signed discovery pair awaits user operation. The interpreter and AOT differ in floating-point order; the native core follows the measured AOT. See [VECTOR_METRICS_MODULE.md](VECTOR_METRICS_MODULE.md) and [VECTOR_DISCOVERY_CASE.md](VECTOR_DISCOVERY_CASE.md). Live coverage remains unclaimed.

## Finite next batches

[The next-batch contracts](NEXT_NATIVE_BATCHES.md) separate the existing scale/copy native-execution pilot from the new vector-metric module. The ledger tracks initial inventory work as INV-001 beneath the continuing ITER-001 workstream. Neither workstream is complete merely because its first batch is defined.

## Common contract

Each replacement has an independent startup-only `off`, `verify`, or `native` switch. Off is the default and installs no hook. Installation must match the entire registered code span, including return delay slots. Unsupported code is refused. A successful installation does not establish call coverage; zero calls remains not covered.

Inputs and outputs are raw bit patterns where no arithmetic is required. Preserve signed zero, NaN payloads, memory ordering, aliases, untouched register lanes, return PC and surrounding bytes. Reject unsupported inputs before speculative writes. Scope each memory contract explicitly; matching the host's guest-memory behavior does not establish physical PSP exception semantics.

Verification compares predicted state with bounded execution of the user's original instructions. The original state and memory remain authoritative. Prediction must not create externally visible writes or consume non-restorable side effects. A failed comparison disables replacement use for subsequent calls in that run; a reference that leaves its certified leaf stops the bounded operation rather than executing unknown caller code. Verification mode is not a performance measurement mode.

The shared evidence counters have these meanings:

| Counter | Meaning |
| --- | --- |
| `calls` | Entry into an installed helper hook |
| `verified` | Completed supported-input comparison after original execution, including comparisons that found a mismatch |
| `native` | Calls that committed the replacement result |
| `fallbacks` | Hooked calls using original behavior for unsupported inputs or an earlier mismatch |
| `mismatches` | Completed comparisons whose state or output differed |
| `errors` | Failed installation, bounded reference, or execution |

Errors may overlap fallback counts, and installation errors may exist with zero calls. These counters describe helper boundaries; they are not counts or timings for arbitrary gameplay functions. The observational recorder attaches stable probe identities and explicit coverage domains; cumulative counters must be interpreted per epoch and case boundary.

## Original five-leaf inventory

All code fingerprints below were recomputed from the locally registered supported ELF. Source instructions and generated code remain local.

| Helper | Full span | Switch | Portable behavior | Guest boundary |
| --- | --- | --- | --- | --- |
| Angle step | `0x088775AC`, 100 bytes | `MHP3RD_NATIVE_ANGLE_STEP` | Bounded circular-angle step | Original target normalization/write precedes current read; preserve overlap, register outputs and return PC |
| Scale matrix | `0x08878B28`, 36 bytes | `MHP3RD_NATIVE_SCALE_MATRIX` | Raw-bit 4x4 scale matrix | Standard prefixes only; identity in VFPU lanes 0–15; 64-byte output |
| Translation matrix | `0x08878B4C`, 36 bytes | `MHP3RD_NATIVE_TRANSLATION_MATRIX` | Identity with raw x/y/z at words 12/13/14 | Standard prefixes only; preserve VFPU identity and return-delay-slot store |
| Four-word vector | `0x08877818`, 24 bytes | `MHP3RD_NATIVE_VECTOR_CONSTRUCT` | Raw x/y/z and literal zero | 16-byte output; no prefix dependency; all registers except PC unchanged |
| Nine-word copy | `0x08879D08`, 80 bytes | `MHP3RD_NATIVE_MATRIX_COPY` | Three ordered groups of three word reads then writes | Offsets 0/4/8, 16/20/24, 32/36/40; padding untouched; final VFPU lanes 0/1/2 contain the final row's loaded bits |

| Helper | SHA-256 of full code span |
| --- | --- |
| Angle step | `c80198f08479038b9d5e0a26516fa8c180f60f11e11bf7521f21208efe63ee49` |
| Scale matrix | `d9ad67fd4b7e26ea8b213c8297489c288c39161f9190bb267f9f5ade35a799b6` |
| Translation matrix | `5b4fa38cc0f789cbf2339d55400eabeb849a4ab5bfc505f169bfb7704fe57038` |
| Four-word vector | `c0c7dc6c33d91c46b68700e1454930520debad811da2d7629a88a4e7d5b3349d` |
| Nine-word copy | `e918aeb6363b81be6cab6ddb2c2605f2179d24bcfa418ea3f24dc06ef393f6c9` |

Translation's return instruction is at entry + `0x1C`, vector's at + `0x10`, and copy's at + `0x48`. The bounded reference executes that return and its delay slot before comparing PC with the saved return address, even when the return address points inside the leaf.

The scale and translation native adapters write their final matrices directly; their write-watch diagnostic event sequences differ from the original identity writes followed by scalar overwrites. Final guest state is compared, and raw write-watch event counts are not an equivalence oracle. Verification itself does not emit prediction writes.

A matrix copy must preserve row-by-row load/store order. Prefetching all nine words, `memcpy`, and `memmove` do not generally reproduce overlap semantics. Memory comparisons must account for guest RAM aliases and unaligned word access without changing surrounding bytes. The existing guest-memory implementation defines the offline oracle's memory behavior.

## Evidence requirements

The common contract implementation passed the local ELF suites again: 806,432 angle cases and 100,512 scale cases (10,000 prefix fallbacks), plus off/fingerprint/counter/bounded-reference checks. Root separately verified that portable math sources compile without PSP headers. No new live gameplay coverage is claimed.

The existing angle/scale history is in [NATIVE_EXPERIMENT.md](NATIVE_EXPERIMENT.md). New translation/vector/copy implementations passed 100,512 / 100,512 / 100,348 local-ELF differential cases respectively. Translation included 10,000 unusual-prefix fallbacks. Vector tests preserve arbitrary prefixes; copy tests preserve ordered alias/overlap behavior. These are offline results. The first user pair subsequently established verify-mode scale/copy coverage; the other three remain untriggered, as detailed above. The combined OFF-006 gate passed on clean commit `832d000`, including all five ELF suites and eleven CTests; the full application also compiled and linked without launch. See [OFFLINE_VALIDATION.md](OFFLINE_VALIDATION.md).

The offline gate must run each test with the local ELF, because no-argument CTest examples do not establish original-code equivalence. Test all CPU state and surrounding memory, raw-bit edge cases, seeded random inputs, aliases/overlap, rejected prefixes, code fingerprint changes, bounded exits and evidence counter semantics. Keep synthetic tests available without game data.

Animation, AI, collision, quests, networking, rendering and full resource semantics remain outside these leaves. They require later bounded contracts, observations and paired user cases. No replacement becomes a default based solely on this inventory.

## Native infrastructure regression coverage

OFF-005 adds 18 synthetic ISO-reader assertions and 33 synthetic PSMF-demuxer assertions. Tests exposed narrow bounds defects: malformed ISO records/extents could be read beyond their declared boundaries, and incomplete PES packets or absent timestamp fields could be consumed as valid payload. The fixes passed the integrated CMake targets on Apple Silicon macOS, plus an independent AddressSanitizer/UndefinedBehaviorSanitizer build. A read-only check matched all 14 supported-image file spans with the validated extraction manifest.

These existing native components are infrastructure assurance, not newly migrated game logic. Synthetic demuxing does not validate video decoding, audio output or movie fidelity; no full game was started.

## Indexed resource boundary

ASSET-001 recovered the count, relative offset, advertised length and null-slot
behavior of three original accessors, plus evidence that downstream objects
retain child pointers. ASSET-002 implements shared immutable parent/child slices
and bounds-checked indexed views without guest CPU/memory dependencies. The
whole local resource manifest and bounded original accessors are the offline
gate; signatures remain annotations, not semantic or live-coverage proof.
See [RESOURCE_BUNDLE_CONTRACT.md](RESOURCE_BUNDLE_CONTRACT.md) and
[INDEXED_RESOURCE_VIEWS.md](INDEXED_RESOURCE_VIEWS.md).

ASSET-003/004 subsequently recovered TMH record and descriptor semantics and
implemented owned encoded image/palette views. All 2,256 discovered TMH inputs
and 8,866 descriptors pass the bounded original-code gate. See
[OWNED_TMH_VIEWS.md](OWNED_TMH_VIEWS.md). Pixel layout/swizzle remains contextual;
the next boundary must use explicit layout evidence before reusing the native
texture decoder or claiming render compatibility. Neither reader is a model,
skeleton, animation or collision decoder.
