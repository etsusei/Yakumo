# Shared session journal

This is OBS-001 in [the development plan](DEVELOPMENT_PLAN.md). It supplies a portable journal and background writer for both Baseline and candidate. [Game observers](GAME_OBSERVATION.md), [certified probes](CERTIFIED_PROBES.md), and [local packaging/comparison](RUN_COMPARISON.md) build on this core. The Chinese case panel and paired launcher remain subsequent tasks. The core does not start the game or read game assets.

## Version 1 framing

All integers are little-endian. No native C++ struct layout is written to disk.

| File header offset | Size | Meaning |
| --- | --- | --- |
| 0 | 8 | ASCII `YKMJNL1` followed by a zero byte |
| 8 | 2 | Schema version, currently 1 |
| 10 | 2 | Header size, currently 16 |
| 12 | 4 | Reserved; must be zero |

Each record has this header followed by its payload:

| Record offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `YKE1` |
| 4 | 4 | Payload byte length, at most 65,536 |
| 8 | 8 | Sequence number, contiguous from 1 |
| 16 | 8 | Host monotonic timestamp in nanoseconds |
| 24 | 2 | Event kind |
| 26 | 2 | Reserved; must be zero |
| 28 | 4 | IEEE CRC32 of header bytes 0–27 followed by payload |

Event kinds are RunBegin (1), RunEnd (2), CaseBegin (3), CaseEnd (4), Checkpoint (5), Anomaly (6), Input (7), State (8), Probe (9), Performance (10), Error (11), and RecordingLoss (12). Unknown versions or event kinds stop recovery rather than being interpreted speculatively.

Payloads produced by the recorder are UTF-8 JSON objects. The field builder supports strings, signed/unsigned 64-bit integers, finite doubles, booleans and null. It rejects empty/duplicate keys, malformed UTF-8, non-finite numbers and oversized output. Module-specific schema and required run identities are validated by the later observer/manifest layer.

The framing reader checks boundaries, checksums, sequence, UTF-8 and RunBegin/RunEnd ordering. It does not establish JSON grammar, run-manifest compatibility or case acceptance for externally constructed payloads. Semantic readers must validate those separately. CRC detects accidental corruption; it is not an authenticity signature.

## Persistence and health

SessionRecorder writes RunBegin before any caller events. Producers enqueue serialized events into limits on both event count and bytes. The writer owns sink calls; producers do not wait for disk writes. Normal events are flushed at least once per configured interval, which cannot exceed one second while I/O makes progress. Case boundaries, checkpoints, anomalies and errors request prompt background flushing. An explicit timed flush is a barrier for events accepted before the call, including known overflow.

Overflow increments health counters and produces an aggregated RecordingLoss record with `dropped_total` and `dropped_since_last`. That control record does not compete for user queue space. A sequence without gaps can therefore still contain lost observations; always check the loss records and counters.

Health distinguishes accepted/written caller events, dropped/invalid events, all written/flushed records, queued bytes/events, closed state and sticky I/O failure. The generated run/loss records do not count as caller events. A partial write, failed flush or failed sink finalization is sticky and cannot be reported as a successful close. File close errors are checked explicitly.

The file sink creates a new file exclusively. It never truncates an existing file or replaces a symlink. The later launcher is responsible for choosing a fresh, isolated run directory and passing complete build/input identities. Flush makes data visible to the operating system; it does not promise power-loss durability. A blocking filesystem operation can delay the writer or graceful close.

## Closure and recovery

Explicit close drains accepted events, records pending loss, emits RunEnd with stop reason, completion intent and counters, then flushes. Closing again is idempotent. A destructor without explicit close records `recorder_destroyed` and `completed=false` when I/O still works.

Recovery stops at the first invalid record and retains the valid prefix. It never skips corruption to find a later plausible marker. Exact frame boundaries without RunEnd are open journals; partial headers or payloads are truncated. Extra bytes after RunEnd are corruption. Payload, record-count and file-size limits bound recovery; the file convenience reader defaults to 256 MiB.

A complete frame sequence is not proof that the user's case passed, that RunEnd marked completion, or that the last flush succeeded. Package validation must also inspect payload semantics, loss/error health, the supervising process result and case outcomes. If the process exits abruptly, unwritten queue contents may be lost; the supervisor must mark that run incomplete even when its valid prefix is readable.

The application returns exit code 5 when final recorder closure fails. In particular, a sink-finalization error detected after writing RunEnd must not appear to the supervisor as a normal window-close exit. Ordinary window-close code 4 is accepted only with the corresponding complete journal and stop reason.

Caller events carry their enqueue-time monotonic timestamp. Generated run/loss events carry their creation timestamp. Sequence orders persisted records; timestamps across different producers and generated diagnostics are not a deterministic replay clock. Guest flip, virtual time, control-read ordinal and input-domain fields will be added by the observer layer.

## Offline verification

Build and run the core and independent process suites without game data:

```bash
cmake --build out/resource-validation --target mhp3rd_session_recorder_tests -j2
ctest --test-dir out/resource-validation \
  -R '^mhp3rd_session_(recorder|process)_tests$' --output-on-failure
```

The process suite requires Python 3.9 or newer at configure time. It parses actual child-process files independently using Python's struct, CRC32 and JSON implementations. Normal close writes one RunEnd; immediate exit and parent termination preserve the flushed prefix without fabricating RunEnd. Every child run is bounded.

On 2026-09-27 (Asia/Tokyo), Apple Silicon macOS, both integrated suites passed. They cover golden framing/CRC, every truncation boundary of a synthetic journal, malformed records, typed JSON, limits, exclusive creation, queue overflow, barriers, periodic/boundary flush, lifecycle closure, partial writes, flush/finalization errors and concurrent producers/closure. AddressSanitizer/UndefinedBehaviorSanitizer passed the core/process suites, and ThreadSanitizer passed the core/concurrency suite.

Local evidence is in `out/testing/session-recorder-validation.json` and its referenced logs. A Windows file-creation branch is present but was not compiled or run here. No game was launched for these checks. OBS-002 and OBS-003 subsequently added observer integrations with offline verification; user experience acceptance remains separate.
