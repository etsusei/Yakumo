# Native angle-step experiment

This is a small, opt-in experiment in preserving original game behavior while replacing a recompiled function with independently written native C++. It does not replace the animation system, monster AI, combat, or the PSP runtime.

## Boundary

The supported NPJB-40001 executable contains a bounded circular-angle helper at `0x088775AC`, ending at `0x08877610`. Inspection of the user's executable established these facts:

- `a0` points to a 32-bit current-angle accumulator; `a1` points to a target word; `a2` is a signed step limit.
- The target is normalized to its low 16 bits and written back **before** the current angle is read. The pointers can alias.
- Distance is computed on a 65536-unit circle. A half-turn tie goes backwards.
- Movement is limited by the signed step limit. Negative limits and 32-bit wraparound retain their original behavior.
- The current accumulator is not truncated to 16 bits after the move.
- The moved amount is returned in `v0`. The adapter also preserves every other register output, both memory writes, and the return PC.

The portable calculation is in `host/native/angle_step.cpp`. It has no guest-memory, register, renderer, or operating-system dependency. `angle_step_bridge.cpp` maps its input and output to the original ABI. A SHA-256 fingerprint of the complete 100-byte function refuses a different executable implementation. Only the fingerprint and behavioral description are committed; the original instructions remain in the user's local ELF.

There are 17 direct call sites in the inspected main executable and no direct calls in the inspected 355 overlays. Those call sites have not been assigned complete gameplay meanings. Do not describe this experiment as a replacement for monster turning or attack logic without tracing those callers.

## Modes

Set `MHP3RD_NATIVE_ANGLE_STEP` before starting a source build:

| Value | Behavior |
| --- | --- |
| unset, `off`, or `0` | Original generated code; no hook is installed |
| `verify` | Predict the native result, restore inputs, execute the original instructions in bounded interpreter slices, and compare the entire CPU context and both output words. The original result drives the game |
| `native` | Execute the new native implementation through the ABI adapter |

Verification reports call, comparison, and mismatch counts under `[native-angle]`. A mismatch keeps the original result and uses the reference for the remainder of the run. An unexpected reference control-flow exit stops the test rather than running arbitrary caller code. Verification is deliberately slower and is not a performance mode. The registration can also disable an AOT unit's direct-call shortcut, so this experiment makes no speedup claim.

Choose the mode at process startup; switching it in a running game is not supported. Restart without the variable to restore the original implementation. Other mods that patch this function while the game runs are outside this experiment's tested scope.

## Verification

Build the targeted tests:

```bash
cmake --build out/mhp3rd --target mhp3rd_native_angle_tests mhp3rd_savedata_tests -j2
ctest --test-dir out/mhp3rd -R 'mhp3rd_(native_angle|savedata)_tests' --output-on-failure
out/mhp3rd/bin/mhp3rd_native_angle_tests profiles/mhp3rd/game/EBOOT.ELF
```

The last command requires the user's supported executable. It compares 806,432 cases against its original machine instructions: every 16-bit relative angle across twelve step limits, plus seeded random cases with high accumulator bits, negative limits, equal pointers, partial overlap, and cached/uncached aliases. All general-purpose and floating-point/vector registers, control registers, the return PC, and the surrounding scratch memory are compared. The test also checks the runtime verifier and rejects a changed code fingerprint.

For an in-game check, use a separate data directory and a **copy** of the user's save. Start a bounded run with `verify`, reach the intended scene, and require nonzero `calls` and zero `mismatches`. Repeat the route with `native`, then with `off`. Merely booting with the hook installed does not prove that it was called. Compare runtime results under the same inputs; screenshots from separate runs are not exact evidence unless the guest clock, save, input timing, and connected controllers are controlled too.

Keep the ELF, ISO, saves, instruction dumps, generated source, logs, and captures in ignored local directories. Reusing release overlay libraries requires unchanged runtime headers and sources **and** matching header/code fingerprints for the user's overlays.

Passing this experiment establishes equivalence for one small integer helper under the tested inputs. It does not establish an entire game's compatibility, combat timing, multiplayer correctness, or performance improvement.
