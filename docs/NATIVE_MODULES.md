# Native module contracts and evidence

This inventory implements the boundaries in [the development plan](DEVELOPMENT_PLAN.md). Task status is in [tasks.json](tasks.json). Portable data functions and guest ABI adapters are distinct: a native helper behind an adapter still depends on the guest runtime. Call totals are not a whole-game migration percentage.

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

Errors may overlap fallback counts, and installation errors may exist with zero calls. These counters describe helper boundaries; they are not counts or timings for arbitrary gameplay functions. The later observational recorder will attach stable probe identities and explicit coverage domains.

## Leaf inventory

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

A matrix copy must preserve row-by-row load/store order. Prefetching all nine words, `memcpy`, and `memmove` do not generally reproduce overlap semantics. Memory comparisons must account for guest RAM aliases and unaligned word access without changing surrounding bytes. The existing guest-memory implementation defines the offline oracle's memory behavior.

## Evidence requirements

The existing angle/scale history is in [NATIVE_EXPERIMENT.md](NATIVE_EXPERIMENT.md). New translation/vector/copy contracts currently have static ELF/generated-code inspection evidence only; differential execution and later user-led coverage remain pending until their task records say otherwise.

The offline gate must run each test with the local ELF, because no-argument CTest examples do not establish original-code equivalence. Test all CPU state and surrounding memory, raw-bit edge cases, seeded random inputs, aliases/overlap, rejected prefixes, code fingerprint changes, bounded exits and evidence counter semantics. Keep synthetic tests available without game data.

Animation, AI, collision, quests, networking, rendering and full resource semantics remain outside these leaves. They require later bounded contracts, observations and paired user cases. No replacement becomes a default based solely on this inventory.
