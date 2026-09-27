# Vector metric leaf contract (VEC-001)

This records the VEC-001 bounded interpreter contract for four leaves in the registered supported executable. The subsequent VEC-002 production AOT findings are recorded below and in [VECTOR_METRICS_MODULE.md](VECTOR_METRICS_MODULE.md). It describes the behavior of the current software interpreter on Apple Silicon macOS. It does not certify a native replacement or physical PSP floating-point results. The local evidence is `out/testing/vector-metrics-contract.json`; the reproducible probe is `profiles/mhp3rd/tests/vector_metrics_contract_probe.cpp`. Neither contains executable bytes or game data.

## Identity and execution bound

The probe requires ELF SHA-256 `55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`, loads it through `Elf32Image`, and rechecks each complete in-memory span before interpreting. Every span includes `jr $ra` and its stack-restoring delay slot.

| Operation | Entry | Bytes | Return instruction | Delay slot | Full-span SHA-256 |
| --- | --- | ---: | --- | --- | --- |
| Three-lane norm | `0x08877244` | 32 | `0x0887725C` | `0x08877260` | `1b1da8d38edcbeb7eb486e12977c6a667fa699639b6ef8d8d7d3f664219b12a0` |
| Three-lane norm squared | `0x08877264` | 28 | `0x08877278` | `0x0887727C` | `1fa5cdbdd2f96479f4cecb659eb7f68dd95a82ef853f99d7a299dc707c241cc1` |
| Three-lane distance | `0x08877280` | 40 | `0x088772A0` | `0x088772A4` | `977da7d41ecfd722f43c497c0f4627bb4faa7d57126d1cefa8f92138115b02a1` |
| Three-lane distance squared | `0x088772A8` | 36 | `0x088772C4` | `0x088772C8` | `05771b861950457e9e065384220c69cc255874d63fe09e62face44defea00d36` |

Each interpreter call is limited to the certified span and at most 12 one-instruction slices. The observed slice counts were exactly 7, 6, 9 and 8 respectively; the last slice executes the return and its delay slot together. The test includes 30 returns to each leaf's own entry address, so reaching that address does not prematurely skip the delay slot. No kernel, HLE, SDL, game loop, `Runtime::run`, or generated game function was invoked.

## Inputs, memory and final state

`$a0` addresses the first vector for all four leaves. The distance leaves also read a second vector at `$a1`. Each vector load reads **four** little-endian 32-bit words at offsets 0, 4, 8 and 12, even though only the first three lanes enter the arithmetic. The fourth word remains observable in VFPU scratch state. Source and stack ranges use the current `GuestMemory` address aliases; the software model accepts unaligned addresses when all bytes are mapped. Physical PSP alignment and exception behavior were not measured.

At entry, each leaf sets `$sp := $sp - 16` with 32-bit wraparound. After all vector loads and arithmetic, it writes one 32-bit scalar word to `[$sp]`, loads that same word into raw `FPR0`, and restores `$sp := $sp + 16` in the return delay slot. Thus the only final memory write is four bytes at original `$sp - 16`, through its guest alias. The original order is important: a stack write overlapping either source occurs **after** both vector loads. The source vectors may also alias or partially overlap each other. The probe checked the full 512-byte surrounding window, including unchanged canaries, in 76 stack/source-overlap cases per leaf.

Final `$pc` equals the entry `$ra`. Every GPR, including `$a0`, `$a1`, `$sp` and `$ra`, has its entry value again; HI, LO and FCR31 are unchanged. FPR0 has the exact stored word; FPR1–31 are unchanged. VFPU control fields 0 and 1 become `0xE4`, field 2 becomes zero, and fields 3–15 are unchanged. The changed VFPU scalar storage indices are:

| Leaves | Final VFPU storage indices |
| --- | --- |
| Norm and norm squared | 0–3 hold the four raw words loaded from `$a0`; index 4 holds the final result word. All other indices are unchanged. |
| Distance and distance squared | Indices 0–2 hold the three post-prefix subtraction lanes; index 3 retains `$a0` word 3; index 4 holds the final result word; indices 5–7 retain `$a1` words 1–3. All other indices are unchanged. |

Index 4 is the encoded VFPU scalar destination used for the dot result and optional square root. It initially receives `$a1` word 0 in the distance leaves, then is overwritten. Under a destination-prefix mask in the norm-squared leaf, the incoming index 4 value can survive the dot; the norm leaf then applies square root to that surviving value. The ordinary result alone is therefore insufficient to model unusual prefixes.

## Arithmetic and prefix order

With standard incoming VFPU prefixes (S and T each `0xE4`, D zero), the software interpreter computes the three-lane self-dot for norm, or subtracts `$a1` from `$a0` lane by lane before the self-dot for distance. The two unsquared leaves then apply the interpreter's `fabs(sqrt(x))` VFPU unary behavior. The dot helper starts with positive zero, processes a four-lane view in order, and zero-fills its fourth operand lane. The loaded fourth words do not enter the arithmetic, though they remain visible in VFPU scratch lanes.

The initial S/T/D prefixes are consumed by the first arithmetic instruction: VDOT for the norm leaves, VSUB for the distance leaves. Each arithmetic instruction resets S/T/D to `0xE4`/`0xE4`/zero. In the norm leaves, S/T can independently swizzle, take absolute values, negate, or replace lanes with constants. VDOT applies prefixes through a four-lane view, so a constant can contribute in lane four even for a three-lane instruction. D can saturate or mask the scalar dot destination. The following square root, if present, sees reset prefixes. In the distance leaves, the incoming S/T/D prefixes affect only VSUB's three input/output lanes; VDOT and square root then see reset prefixes. A VSUB destination mask retains the preceding first-vector lane. These are software-context semantics from `AllegrexContext`, confirmed by bounded prefix probes; they are not a physical VFPU characterization. Four controlled S/T/D changes produced distinct raw outputs for every leaf in the local report.

The 2,048 ordinary finite vectors per leaf were also compared with separately written scalar calculations. On the tested Apple arm64 interpreter object, a left-to-right `std::fma` accumulation matched all 2,048 raw results per leaf. A separate multiply followed by add differed in 240 norm, 466 norm-squared, 234 distance and 424 distance-squared cases. The observed match is **specific to this compiler/object and sample**; the source-level `sum += product` is contracted by this host build. It does not establish that the PSP VFPU, another compiler, or generated AOT uses the same rounding. FCR31 was randomized and stayed unchanged; this software path does not consult its rounding bits for these VFPU operations.

Representative standard-prefix single-lane raw-bit observations, with the other arithmetic lanes zero, were identical across the four operations (the distance second vector was zero): `-0` (`0x80000000`) and the smallest positive subnormal (`0x00000001`) returned `+0`; largest finite (`0x7F7FFFFF`) and either signed infinity returned `+infinity`; quiet NaN `0x7FC01234` returned the same payload; signaling NaN `0x7FA05678` returned quieted `0x7FE05678`. The output samples are not a blanket NaN payload-order rule. The squared leaves can overflow before a square root would be taken; the unsquared leaves apply host `std::sqrt` after the dot. Cancellation, nonfinite cross-products, prefix-injected constants, and host flush/rounding modes require direct differential coverage in VEC-002.

## Supported domain and next gate

The **observed reference domain** is this exact ELF, mapped software guest-memory ranges containing all 16 bytes of each required source and all four scratch bytes, and bounded execution through the recorded return slot. The probe exercised arbitrary source/stack address aliases, unaligned pointers, source overlaps, standard prefixes and 757 nonstandard-prefix cases per leaf. It did not install an adapter: native calls, adapter rejects and actual fallbacks are all zero. A changed ELF/span fingerprint is refused by the probe before execution.

For an initial replacement, use a conservative admission check: the registered full-span fingerprint, mapped source and stack ranges, and standard S/T/D prefixes. Let nonstandard prefixes and unproven numerical cases execute the original path. If mapped ranges touch executable code or another side-effectful region, retain the original path until ordered writes can be verified. Reject before any speculative write. No production-native support set has been certified yet.

VEC-002 must compare a candidate against both this bounded instruction interpreter **and the actual generated AOT leaf**, including full CPU/VFPU state and the whole touched memory window, on the same input. In particular, it must resolve the fused arithmetic observation and verify raw results for extrema, subnormals, infinities/NaNs, cancellation, nonstandard-prefix fallbacks, overlap and alignment. Generated AOT was inspected as local context here but **was not executed as an oracle**. No physical PSP run or live gameplay call was observed. The static call references in [NATIVE_MODULES.md](NATIVE_MODULES.md) remain possible call sites, not a demonstrated user trigger.

## Probe evidence

The probe ran on Apple Silicon macOS (`Darwin arm64`) with Apple LLVM 21.0.0 (`clang-2100.3.34.2`). Its source was built with `-std=c++20 -O2 -ffp-contract=off` and linked to the existing `out/mhp3rd` Release core objects. The observed source HEAD was `77579fb9058461e073c2407e5adca1eef74aa372`; the local report records hashes of the interpreter source, context source, interpreter object, probe source and executable. Source hashes identify this audit without embedding game bytes. The probe's `-ffp-contract=off` makes the unfused scalar comparison deliberate; it does not change the already built interpreter object.

| Per leaf | Count |
| --- | ---: |
| Bounded completed runs | 2,901 |
| Standard / nonstandard prefix runs | 2,144 / 757 |
| Stack/source overlap runs | 76 |
| Unaligned-address runs | 2,688 |
| Aliased-address runs | 2,836 |
| Internal return-address runs | 30 |
| Ordinary finite scalar-model comparisons | 2,048 |

The four-leaf totals are 11,604 bounded runs and 8,192 finite numerical comparisons. All registered state, memory-canary and return invariants passed. Only the fused scalar model matched every finite comparison on this machine; that result remains a software-oracle observation until the VEC-002 AOT differential gate.

## Reproducing the audit target

On the currently scoped macOS build, build
`mhp3rd_vector_metrics_contract_probe` through `cmake --build out/mhp3rd --target
mhp3rd_vector_metrics_contract_probe -j2`, then run the resulting binary with
`profiles/mhp3rd/game/EBOOT.ELF` as its sole argument and preserve stdout in a
new local JSON file. The CMake target applies `-ffp-contract=off` only to the
probe; the interpreter retains its normal production flags. Root independently
reran the agent binary and the CMake-built target; both reproduced all four
reported result sets. These reruns still do not execute generated AOT.

## VEC-002 production AOT follow-up

The generated production object has now been executed on identical synthetic
inputs. The four-leaf audit contains 5,516 bounded calls with full raw CPU and
512-byte memory comparison. AOT differs from the interpreter in 90 norm,
173 norm-squared, 83 distance and 173 distance-squared cases. Every CPU
difference is confined to FPR0 and VFPU storage index 4; memory differences
are confined to the four scratch-result bytes. No unexpected write was seen.

The AOT arithmetic model rounds the second product first, fuses the first
product into it, then fuses the third and zero-filled fourth lanes. It matches
all 4,496 standard-prefix raw results, including the tested NaNs, subnormals,
infinities, extrema, cancellation and overlapping inputs. Ordinary finite
inputs also differ between AOT and interpreter, so excluding unusual values
would not repair the oracle mismatch. The native module targets the original
AOT behavior; the interpreter differences are retained as diagnostic evidence.

A direct call to the actual generated unit with temporary RA=0, followed by
restoring the caller's internal RA/PC, also matched all 5,516 isolated AOT cases
in full state and memory. This supplies the bounded original callback used by
the owned adapter. It is specific to these four fingerprinted leaves, which do
not consume RA before returning. It is not permission to substitute RA in
arbitrary functions. The source is `tests/vector_metrics_aot_probe.cpp` under
the profile; local reports and build identity hashes are listed in the ledger.
