# Portable vector metric module

VEC-002 separates four data operations from the guest CPU: three-lane norm,
squared norm, distance, and squared distance. The core accepts and returns raw
IEEE-754 words and has no runtime, guest memory, instruction decoder or generated
code dependency. It returns the distance components as well as the scalar so a
guest adapter can preserve observable scratch state.

The VEC-003 source now supplies opt-in startup registration, certified observations
and versioned mode metadata; see [VECTOR_METRICS_OBSERVATION.md](VECTOR_METRICS_OBSERVATION.md).
The existing native-data test applications are unchanged. A later user-led
acceptance batch remains pending demonstrated gameplay triggers. This milestone establishes a
small reusable numerical module, not migrated animation, AI or collision.

## The numerical reference

The original generated AOT object is the compatibility reference. The bounded
interpreter is an independent diagnostic, but it is **not bit-identical** to that
object. On the tested Apple Silicon Release build, its dot product starts at
zero and accumulates in lane order. The production AOT object instead rounds
`y*y`, fuses `x*x` into that value, then fuses `z*z` and the zero-filled fourth
lane. Both unsquared operations finish with `fabs(sqrt(sum))`.

The explicit core uses `std::fma` at those boundaries and disables implicit
contraction in its own translation unit. Four fixed synthetic witnesses in the
asset-free core test catch a switch back to interpreter order. Rounding and NaN
payload differences remain visible in the oracle report; they are not removed
by loosening equality or excluding the differing finite inputs. The core is
portable C++, but matching another platform's generated AOT object requires its
own differential gate. This is not a claim about physical PSP arithmetic.

## Guest boundary and modes

`VectorMetricBridge` is an owned object. It starts off and does not install a
runtime hook or read an environment variable. Configuration takes a runtime,
one operation, mode, original generated-unit callback, and copied physical
exclusion ranges. The caller must include all executable sections and overlay
arenas; a range list that does not cover the certified leaf is refused.

The complete leaf fingerprint includes the return and delay slot. Configuration
and every invocation check it. Unknown or changed code, a different runtime,
an incorrect entry PC, or a stopped runtime is refused. Refusal is not silently
treated as a successful original call. A failed reconfiguration leaves the
previous valid configuration intact and increments its error counter.

Native admission requires standard VFPU prefixes, nearest host rounding, and
mapped RAM source/scratch ranges outside the supplied exclusions. All four
source words are read before the scratch write, including the non-arithmetic
fourth word. Aliases, unaligned RAM accesses, and source/stack overlaps are
preserved. The core owns the arithmetic; the adapter owns raw register placement,
prefix reset, the single four-byte stack write, and return PC.

- **Off:** execute the original AOT leaf.
- **Verify:** predict without speculative guest writes, execute the original,
  and compare the full raw context and captured input/scratch bytes. Retain the
  original result. A mismatch latches subsequent invocations to the original,
  including after reconfiguration. A fresh adapter object is required to clear
  that latch; resetting per-configuration counters cannot clear it.
- **Native:** commit the predicted state and scratch word for admitted inputs.
- **Fallback:** use the original AOT callback for an unsupported input or a
  previously detected mismatch. Prefixes and nonfinite values are not conflated:
  nonstandard prefixes fall back; standard-prefix NaNs/infinities are tested.

Calling the original through the runtime lookup would recurse after future
registration. The adapter instead calls the supplied unmodified generated-unit
wrapper directly. These four fingerprinted leaves read RA only at return. It
temporarily replaces RA with zero, requires the wrapper to return there, then
restores the caller's RA and PC. This bounds the call even when the real return
address is inside the same generated unit. An exception restores RA and is
re-thrown; a bad return stops the runtime. The callback contract is specific to
these audited leaves and must not be generalized to arbitrary game functions.

## Validation and reproduction

The asset-free target is `mhp3rd_vector_metrics_core_tests`. The optional local
ELF targets are `mhp3rd_vector_metrics_aot_probe` and
`mhp3rd_native_vector_metrics_tests`; both link the existing production generated
objects. Build them through `cmake --build out/mhp3rd --target <target> -j2`.
The local ELF programs accept `profiles/mhp3rd/game/EBOOT.ELF` and require the
registered full-file hash. Bound each invocation to 60 seconds and preserve its
output under ignored `out/testing/`. They do not launch SDL, the game loop,
kernel, rendering, or audio.

The oracle compares raw CPU/VFPU state and all 512 surrounding bytes between
interpreter and actual AOT; differences are reported, not declared a failed
replacement. The adapter suite compares Off/Verify/Native directly to AOT and
checks numerical edges, aliases, overlaps, unusual prefixes, rejection before
mutation, and mismatch fallback. See the task ledger for current executed
checks and evidence paths. A successful oracle process alone is not a passing
native implementation or live gameplay acceptance.

No live callers, frame performance, recording overhead, physical PSP behavior,
other host platforms, or whole-game compatibility are certified by these
offline tests. Native admission outside default host floating-point settings
has not been established. All four modes remain off by default; no new user test application has been delivered.

## Executed VEC-002 gate

On Apple Silicon macOS Release, all 12,060 main adapter comparisons passed:
4,020 inputs in each of Off, Verify and Native. Verify completed 3,720 admitted
comparisons and 300 original fallbacks; Native committed 3,720 results and used
300 original fallbacks. Unexpected bridge mismatches and errors were zero.
The independent core matched AOT in 3,756 standard-prefix samples. This suite
also recorded 280 interpreter scalar/scratch disagreements and zero component
disagreements; these are distinct from replacement failures.

The additional rejection and injected-failure cases passed, including a changed
return delay slot, unmapped inputs, an invalid original return, a thrown original,
and preserving a mismatch latch after switching to Native. The asset-free core
CTest passed. The separate AOT oracle passed its 5,516 wrapper/canary checks.
Local evidence is `out/testing/vector-metrics-module.json`,
`out/testing/vector-metrics-adapter-final.log`, and
`out/testing/vector-metrics-aot-final.json` with its `.hashes.json` sidecar.
