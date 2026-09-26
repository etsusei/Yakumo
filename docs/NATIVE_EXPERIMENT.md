# Native helper experiments

These small, opt-in experiments preserve original behavior while replacing recompiled functions with independently written native C++. They do not replace the animation system, monster AI, combat, or the PSP runtime.

| Helper | Purpose | Switch |
| --- | --- | --- |
| Scale matrix | Construct the 4x4 scale matrix used by the game's graphics code | `MHP3RD_NATIVE_SCALE_MATRIX` |
| Angle step | Move a circular angle towards a target with a bounded step | `MHP3RD_NATIVE_ANGLE_STEP` |

Both switches accept `off` (the default), `verify`, and `native`. Enable one at a time when measuring behavior.

## Scale matrix

The supported executable's 36-byte leaf at `0x08878B28` takes the output address in `a0` and x/y/z scale values in scalar floating-point registers 12/13/14. It writes a 4x4 diagonal scale matrix to memory and leaves an identity matrix in VFPU matrix M000. Its return instruction has a memory-writing delay slot, which the reference includes.

The portable `scale_matrix()` builder only manipulates IEEE-754 bit patterns and has no PSP dependencies. The adapter preserves all other CPU state. Signed zeros and NaN payloads are copied exactly; there is no rounding or approximate arithmetic. Only the standard VFPU prefix state uses the native path. Unusual prefixes fall back to the original instructions, and the fallback count is reported.

`verify` compares all CPU registers and all 64 output bytes for each accepted call, restoring inputs and retaining the original result. `native` uses the replacement for standard prefixes. Either mode checks the entire function's fingerprint before installing the hook. A mismatch disables native use for the rest of the run. Logs start with `[native-scale]` and include `calls`, `verified`, `native`, `fallbacks` and `mismatches`.

The original-code test covers 100,512 inputs, including arbitrary floating-point bit patterns, signed zero, infinities, quiet/signaling NaNs, scratch-memory canaries, RAM aliases, and 10,000 unusual-prefix fallbacks. No game instructions are embedded in the test:

```bash
cmake --build out/mhp3rd --target mhp3rd_native_scale_tests -j2
out/mhp3rd/bin/mhp3rd_native_scale_tests profiles/mhp3rd/game/EBOOT.ELF
```

A bounded dispatch sample of the read-save / character-select / village-walk route observed 47,872 entries at this address, making it suitable for live validation. The same route did not call the angle-step helper; its isolated tests must not be mistaken for in-game coverage.

## Angle step

### Boundary

The supported NPJB-40001 executable contains a bounded circular-angle helper at `0x088775AC`, ending at `0x08877610`. Inspection of the user's executable established these facts:

- `a0` points to a 32-bit current-angle accumulator; `a1` points to a target word; `a2` is a signed step limit.
- The target is normalized to its low 16 bits and written back **before** the current angle is read. The pointers can alias.
- Distance is computed on a 65536-unit circle. A half-turn tie goes backwards.
- Movement is limited by the signed step limit. Negative limits and 32-bit wraparound retain their original behavior.
- The current accumulator is not truncated to 16 bits after the move.
- The moved amount is returned in `v0`. The adapter also preserves every other register output, both memory writes, and the return PC.

The portable calculation is in `host/native/angle_step.cpp`. It has no guest-memory, register, renderer, or operating-system dependency. `angle_step_bridge.cpp` maps its input and output to the original ABI. A SHA-256 fingerprint of the complete 100-byte function refuses a different executable implementation. Only the fingerprint and behavioral description are committed; the original instructions remain in the user's local ELF.

There are 17 direct call sites in the inspected main executable and no direct calls in the inspected 355 overlays. Those call sites have not been assigned complete gameplay meanings. Do not describe this experiment as a replacement for monster turning or attack logic without tracing those callers.

### Modes

Set `MHP3RD_NATIVE_ANGLE_STEP` before starting a source build:

| Value | Behavior |
| --- | --- |
| unset, `off`, or `0` | Original generated code; no hook is installed |
| `verify` | Predict the native result, restore inputs, execute the original instructions in bounded interpreter slices, and compare the entire CPU context and both output words. The original result drives the game |
| `native` | Execute the new native implementation through the ABI adapter |

Verification reports call, comparison, and mismatch counts under `[native-angle]`. A mismatch keeps the original result and uses the reference for the remainder of the run. An unexpected reference control-flow exit stops the test rather than running arbitrary caller code. Verification is deliberately slower and is not a performance mode. The registration can also disable an AOT unit's direct-call shortcut, so this experiment makes no speedup claim.

Choose the mode at process startup; switching it in a running game is not supported. Restart without the variable to restore the original implementation. Other mods that patch this function while the game runs are outside this experiment's tested scope.

## Verification workflow

Build the targeted tests:

```bash
cmake --build out/mhp3rd --target mhp3rd_native_angle_tests mhp3rd_native_scale_tests mhp3rd_savedata_tests -j2
ctest --test-dir out/mhp3rd -R 'mhp3rd_(native_angle|native_scale|savedata)_tests' --output-on-failure
out/mhp3rd/bin/mhp3rd_native_angle_tests profiles/mhp3rd/game/EBOOT.ELF
```

The last command requires the user's supported executable. It compares 806,432 cases against its original machine instructions: every 16-bit relative angle across twelve step limits, plus seeded random cases with high accumulator bits, negative limits, equal pointers, partial overlap, and cached/uncached aliases. All general-purpose and floating-point/vector registers, control registers, the return PC, and the surrounding scratch memory are compared. The test also checks the runtime verifier and rejects a changed code fingerprint.

For an in-game check, use a separate data directory and a **copy** of the user's save. Start a bounded run with `verify`, reach the intended scene, and require nonzero `calls` and zero `mismatches`. Repeat the route with `native`, then with `off`. Merely booting with the hook installed does not prove that it was called. Compare runtime results under the same inputs; screenshots from separate runs are not exact evidence unless the guest clock, save, input timing, and connected controllers are controlled too.

Keep the ELF, ISO, saves, instruction dumps, generated source, logs, and captures in ignored local directories. Reusing release overlay libraries requires unchanged runtime headers and sources **and** matching header/code fingerprints for the user's overlays.

Passing these experiments establishes equivalence only for these small helpers under the tested inputs. It does not establish an entire game's compatibility, combat timing, multiplayer correctness, or performance improvement.

## Recorded local result

On 2026-09-26, macOS 27 / Apple M5 / MoltenVK, code commit `f60e77f`:

- The graphical source build and the angle, scale-matrix and save-data tests passed.
- The user's supported HD image matched the expected executable. All 355 overlay header/code fingerprints matched the published libraries; runtime headers and sources matched the release too.
- An independent copy of the user's save passed SFO/data hashes, decryption and round-trip checks. The original archive and the untouched source copy were verified unchanged afterward.
- The scale-matrix differential test matched 100,512 cases, including 10,000 prefix-fallback cases. The angle test matched 806,432 cases.
- A bounded read-save, character-select, village-load and walking route completed in the baseline, verification, and native runs.
- Live scale verification: **48,365 calls, 48,365 comparisons, zero mismatches and zero fallbacks**. The original result remained authoritative in this mode.
- Native scale execution: **48,405 calls used the native implementation, zero fallbacks**. Captures of the village and walking route were inspected; the sampled village section ran at about 30 fps / 100% game speed. This was not a benchmark or a full-game regression test.
- The angle helper had **zero calls** on this route. Its isolated equivalence checks passed, but its in-game behavior has not been exercised.

Separate runs used the same initial save and frame-indexed input script, but the guest wall clock was not pinned. Their images and call counts are therefore not claimed to be bit-identical. The live verifier compares original and replacement on the same actual input within a single call.

Audio output was disabled for these bounded runs. Combat, quests, multiplayer, nonstandard mods, other platforms and long-session stability were not verified. No whole-game rewrite or measured speed improvement is claimed. Both replacements remain off by default.
