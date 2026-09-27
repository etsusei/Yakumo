# Portable texture renderer dispatch

ASSET-007 connects the portable pixel core to both production renderer decode
sites: immediate cache-miss decoding and the existing background snapshot
worker. It is a resource architecture change, not an additional replacement
of original PSP game instructions. The legacy decoder source remains unchanged.

## Fixed session policy

`MHP3RD_PORTABLE_TEXTURE_DECODE` accepts `off` (default), `verify`, or `native`.
An empty or unknown value is rejected. Baseline B0 and builds without the
renderer reject every non-off mode during startup/preflight. B0 excludes the
new dispatcher/core linkage and compiles the existing renderer routes.

Off keeps the legacy immediate and worker paths. Verify captures owned bytes,
decodes them independently with both implementations, and returns the legacy
pixels even when they differ. Native returns portable output for supported
captures and retains explicit legacy fallback for unsupported input. This
experiment remains off by default; no existing test application is replaced.

The supplemental `yakumo-texture-decode-v1` schema records
`texture_decode_schema` and `texture_decode_mode`. It is separate from the
five/nine-field native-helper schemas. Historical configurations and journals
with neither field mean off. Launchers explicitly set off for those historical
configurations so an inherited environment variable cannot enable the
experiment. New fields must be present together and agree with preflight.
Existing helper-only comparison profiles reject an active texture experiment;
a renderer-specific acceptance policy is a separate delivery requirement.

## Ownership and fallback

`PortableTextureDispatcher` fixes the mode at construction and owns atomic
counters. Each background packet owns an immutable snapshot of texture and
palette bytes; it contains no reference or pointer into guest memory. Workers
can finish after the source memory is mutated or destroyed. The unchanged
legacy snapshot decoder receives its own mutable copy for comparison/fallback.

Background eligibility remains the existing snapshot contract. DXT stays on
the immediate path. Verify captures its complete block window and supplies
exactly those bytes to the unchanged decoder in private comparison memory.
Native DXT decoding does not construct comparison memory. A rejected capture
returns to the original immediate decoder; missing palettes are not padded.

Unsupported immediate state falls back before capture. This matters when a
noncanonical palette offset addresses entry 512 or later: the original
immediate path can read those guest bytes, while its snapshot path only copied
the first 512 entries. Each fallback preserves its original route's behavior.

## Observations and current evidence

The renderer emits bounded, cumulative `texture_decode.counters` records at
most once per host second, scoped to cache-miss decode requests rather than
all draws. It records modes, immediate/worker activity, portable success,
verification, native execution, rejection/fallback/mismatch/failure counts,
and decoder CPU durations. Periodic counters can include in-flight jobs;
final counters follow draining the owned jobs before the recorder closes.
Durations are decoder measurements, not a gameplay speedup claim.

The candidate executable builds on Apple Silicon macOS. The dispatcher/core
CTests pass. Independent dispatcher tests cover all eleven formats, unchanged
Off behavior, Verify/Native output and counters, released source ownership,
concurrent jobs, malformed windows and the immediate/snapshot palette-boundary
distinction. Paired-tool compatibility tests preserve historical records and
reject inconsistent policy fields.

The observed B0 build and complete paired preflight remain pending at this
implementation checkpoint. Live Vulkan presentation, asynchronous scheduling
under gameplay load, performance, exact source-to-draw mapping and user visual
acceptance are not yet verified. Do not infer those from the offline core or
dispatcher tests.
