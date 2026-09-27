# Vector metric observation and startup integration

VEC-003 connects the VEC-002 mathematical core to actual runtime dispatch and
the shared recorder. All switches remain off by default. Existing delivered
native-data applications and user records are unchanged. Offline execution of
a game function is not evidence that a particular gameplay action triggers it;
a new finite user case remains pending that evidence.

| Entry | Operation / probe selection | Startup switch |
| --- | --- | --- |
| `0x08877244` | `norm` | `MHP3RD_NATIVE_VECTOR_NORM` |
| `0x08877264` | `norm_squared` | `MHP3RD_NATIVE_VECTOR_NORM_SQUARED` |
| `0x08877280` | `distance` | `MHP3RD_NATIVE_VECTOR_DISTANCE` |
| `0x088772A8` | `distance_squared` | `MHP3RD_NATIVE_VECTOR_DISTANCE_SQUARED` |

## Dispatch and lifetime

CMake resolves the original unit from all four registrations in the generated
corpus, rejecting missing, duplicate or split-unit registrations. It does not
assume that unit numbering is stable after regeneration. A scoped
`VectorMetricRuntime` is constructed after the Runtime and destroyed before
it. All requested enabled entries are certified before any hook is installed.
Off installs no hook. Registration is startup-only; duplicate owners and a
second installation on the same owner are refused. Each Runtime owns distinct
modes, counters and mismatch state.

Ordinary and compiled cross-unit calls use registered hooks. Same-unit local
gotos need an additional seam at the actual leaf label: the generated copy
offers the owned dispatcher first refusal, then continues original AOT if the
entry is off or unowned. A handled call resumes the generated unit's existing
bounded local-dispatch protocol at the returned PC. The original wrapper's
zero-RA reference call suppresses this seam as well as its nested AOT probes,
so verification cannot recursively replace its own oracle.

The shared dispatch callback defaults null. The sealed B0 build links no metric
installer, rejects non-off values for all nine switches, and therefore executes
original AOT at these labels. Removing an owner restores original exact-entry
dispatch and chainability. The runtime's conservative whole-unit fast-path
invalidation remains; this is not a claim of restoring its prior optimization
state. Installation and destruction must happen outside guest execution.

Native mode is currently admitted only on Apple Silicon, where the numerical
oracle has been checked. Other hosts may run Verify while keeping original AOT
results. Enabled metrics require compiled entry seams; building without
certified instrumentation must not silently enable only cross-unit replacements.

## One observed invocation

The original five bits stay at positions 0–4. Metrics use positions 5–8; `all`
selects all nine and `legacy` selects the first five. Case-selected masks map
all nine entries explicitly. Each selected metric entry hashes the full span,
including its stack-restoring return delay slot. The AOT exit is observed after
that stack restoration and before continuing local dispatch.

A bridge owns one outer `NativeProbeScope`. A thread-local scoped suppression
guard matches only its Runtime, context and entry during the direct original
callback. It does not hide unrelated entries, contexts, or runtimes. Thus
Verify and Fallback count once, with their actual variants, instead of recording
both the bridge and its internal original call. Exceptions restore the guard;
failed or uncertified invocations do not become successful timing samples.

## Explicit record and execution versions

New run-begin metadata and preflight results declare
`native_mode_schema: yakumo-native-modes-v2`. Run-begin metadata must include
all nine scalar mode fields, even for disabled helpers. A v2 launch manifest
must declare the same schema and exactly nine modes. Missing, unknown or extra
mode fields are rejected rather than inferred as disabled.

Historical records without the schema remain exactly five-field records. The
frozen B0 registration is unchanged. Schema mismatches make comparisons
incompatible; a legacy record cannot establish coverage of a new metric. Old
application binaries can still use their matching legacy launcher manifests.
New binaries require an explicit matching v2 execution profile; an old profile
is never silently expanded into a new one.

`yakumo-native-batch-v1` retains the original scale/copy pilot policy.
`yakumo-native-batch-v2` keeps those five helpers off and permits controlled
metric off/verify/native values. Its sorted required-native entries must equal
the metric entries in Native mode and appear in its hash-bound catalog. A
Verify-only profile has no required-native entries and cannot be reported as
successful native execution. Zero calls remain not covered.

## Offline checks and remaining acceptance

Build `mhp3rd_vector_metrics_runtime_tests` with `cmake --build`; run it against
the registered local ELF with a 60-second subprocess limit. It checks full CPU
and 512-byte state, all four entries in Off/Verify/Native, prefix fallback,
one-call probe accounting, internal returns, compiled chain dispatch, owner
isolation/restoration, and unknown or changed fingerprints.

The real caller at `0x08877610` provides a bounded same-unit test. Its complete
72-byte span has SHA-256
`88228481838b5eded0af56d387b06d33b275cc849e60c6a86da6c54d94751cd6`.
It calls norm with a local goto. Verify and Native each produce exactly one
certified completed norm scope, their corresponding variant, and zero duplicate
AOT calls, fallbacks, or incomplete scopes. Its full CPU and memory match
original AOT. This establishes an actual compiled call path, not a live scene.

The rebuilt 12,060-case metric gate and 1,280-call legacy AOT gate also passed.
Synthetic probe/suppression, case controller, diagnostics, instrumentation and
versioned-tool tests cover the surrounding contracts. The task ledger records
executed commands, current evidence paths and any pending checks.

No new gameplay acceptance, performance improvement, physical PSP arithmetic,
or non-Apple native behavior is claimed. Do not create required user actions
from static call-site counts. Finish the current native-data user batch and
retain its evidence independently of this observation integration.

## Published build checkpoint

Implementation `399754a` is published in draft PR
[#18](https://github.com/etsusei/Yakumo/pull/18). Both the candidate and independently
prepared B0 build passed 33 real-binary preflight checks, including rejection of
Verify/Native/invalid for all nine Baseline switches. The observation revision is
`source-sha256:00f4fc9833674ae01024e378d5c4c4b11e364c9cdb1517771e124d117b145551`.
Their generated metric-unit object hashes are identical. The B0 AOT gate also
passed its 1,280 bounded original calls. Archived build manifests are under
`out/testing/observed-builds/vector-observation-399754a/`; the consolidated local
report is `out/testing/vector-observation-validation.json`.

VEC-003 remains in progress because live trigger discovery and its finite user
case are not yet complete. These builds are not a new delivered application pair.
