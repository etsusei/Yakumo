# Baseline registration and raw resource preparation

This is the offline preparation step of [the development plan](DEVELOPMENT_PLAN.md). It does not start the game, change its resource loader, or establish animation/AI/visual compatibility.

## Baseline identity

Baseline B0 is the tracked source at `4292eb6`, with the Chinese interface and all native replacements disabled. Freeze its source archive/tree, the supported executable, original image and starting-save fingerprints, and the build/dependency evidence in ignored local manifests.

An existing executable with an older or dirty embedded build label is historical evidence only. It is not proof of a reproducible B0 build. The paired test applications are built and validated in the later delivery milestone.

Starting saves are content-addressed copies. Existing snapshots must be verified before reuse; a mismatch must never cause the source save to be overwritten. Copying a snapshot into a run must require a new destination, so an active or previous run cannot be reset in place.

Register a local reference without starting the game:

```bash
python3 profiles/mhp3rd/tools/register_baseline.py \
  --repo . --commit 4292eb6 \
  --iso /path/to/game.iso --elf profiles/mhp3rd/game/EBOOT.ELF \
  --save-dir /path/to/ULJM05800 \
  --output out/testing/baselines/B0 \
  --snapshots-root profiles/mhp3rd/analysis/testing/start-saves \
  --build-dir out/mhp3rd --overlays /path/to/compatible/overlays
```

`--build-dir`, `--overlays`, and `--save-archive` are optional historical evidence inputs. The command pins B0 and the supported decrypted executable; it hashes the ISO without executing it. Repeating the same registration verifies the archived files and save snapshot and returns the existing manifest. Different inputs or corrupt outputs are refused. Full disc structure validation belongs to the resource-preparation step.

The intended configuration is the pinned source's defaults with Chinese UI and all native switches off. The manifest separately records observed historical build flags and dependency versions. A future paired test run records its full effective runtime configuration rather than assuming historical settings are current.

Run the synthetic registration checks with:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s profiles/mhp3rd/tests -p 'test_baseline_registration.py' -v
```

The first local registration and hash-verified reuse both passed. Eleven synthetic tests covered input preservation, supported identity checks, snapshot reuse/corruption, fresh-only run copies, symlink/overlap refusal, and failed-stage cleanup. No game was launched. The current historical executable remains explicitly unverified as a reproducible B0 build.

## Workspace contract

Keep the original image in place. The ignored `profiles/mhp3rd/analysis/resources/` workspace holds:

| Path | Meaning |
| --- | --- |
| `manifest.json` | Source identities, file/entry index, lengths, hashes, transforms and known type metadata |
| `raw-disc/` | ISO filesystem files exactly as stored, including the obfuscated DATA.BIN |
| `raw-entries/` | Entries extracted from DATA.BIN, named by numeric entry ID; deobfuscated except entries the game stores verbatim |
| `derived/` | Later format-specific conversions, each retaining its source entry identity |
| `working/` | Mutable experimental copies; never a replacement for raw reference files |

The stored block span and extracted size are different fields. An entry without a size-table row has an extracted block span; this does not establish where its semantic payload ends. Preserve empty entries, padding uncertainty, and unknown types in the manifest. Overlay header names are metadata, not trusted output paths.

Preparation must publish only a fully checked staging directory. Reuse requires matching input and output content hashes and sizes. Corrupt or changed workspaces must not be accepted merely because a manifest or file exists. Do not overwrite derived/working edits while recovering a raw workspace.

## Validation requirements

The guarded path must validate ISO volume/record extents, mirrored numeric fields, names, case-insensitive collisions, directory cycles, exact reads, and unsupported record features. The inspected supported image does not use multi-extent files; a tool that does not assemble them must reject them explicitly.

DATA.BIN handling must bound reads to the archive and each entry, preserve ordered block/size metadata and opaque trailer bytes, and avoid interpreting a short or empty entry's neighbor as its header. Large entries must be decoded in bounded, offset-aware chunks rather than building a full-entry keystream and integer in memory.

Synthetic tests cover malformed/truncated structures, unsafe paths, cycles, duplicates, empty entries, padding/exact sizes, verbatim data, overlay headers, chunk-boundary deobfuscation, interrupted publication and tampered reuse. Fixtures must not depend on copyrighted game bytes.

## Extraction and verification commands

Prepare the full local workspace using the ISO SHA-256 recorded in the B0 manifest:

```bash
python3 profiles/mhp3rd/tools/prepare_resources.py /path/to/game.iso \
  --output profiles/mhp3rd/analysis/resources \
  --expected-iso-sha256 <registered-iso-sha256>
```

The command publishes a checked staging directory, writes read-only raw files, and keeps `derived/` and `working/` separate. On repeat execution it verifies the existing workspace against source spans and independently regenerated entry hashes, and preserves those editable directories. Changing a raw output and its manifest hash together is rejected. The source ISO is rehashed after verification to detect changes during the check. A mismatch is an error; it never silently rebuilds over existing work. `--verify-only` requires an existing workspace. Multi-extent/interleaved ISO records are not supported and are rejected explicitly.

The size-table row count is inferred from ordered valid rows followed by an opaque trailer. Some malformed rows can resemble trailer bytes; no universal corruption-detection guarantee is claimed. The manifest preserves the encrypted directory/trailer fingerprints and byte spans, and `raw-disc/` retains their original bytes.

Run the synthetic suites, which require Python 3.9 or newer:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s profiles/mhp3rd/tests -p 'test_resource_preparation.py' -v
ctest --test-dir out/mhp3rd \
  -R 'mhp3rd_(baseline_registration|resource_preparation)_tests' --output-on-failure
```

CMake registers both suites when testing is enabled and a suitable Python interpreter is available. The independent cross-check target compares every extracted entry byte with the existing C++ runtime decoder. Use it only after the preparation tool validates the ISO/archive span; read the offset and size from `manifest.json`:

```bash
cmake --build out/mhp3rd --target mhp3rd_resource_crosscheck -j2
out/mhp3rd/bin/mhp3rd_resource_crosscheck /path/to/game.iso \
  <archive-offset> <archive-size> profiles/mhp3rd/analysis/resources/raw-entries
```

Neither command launches gameplay. The C++ verifier is independently implemented relative to the Python extraction path, but both implement the same documented format; agreement does not establish the semantic meaning of every resource.

## Inspection evidence before full extraction

The registered local image was inspected without launching gameplay. Both the existing Python reader and an independent use of the project's C++ directory/deobfuscation code reported:

| Fact | Observed value |
| --- | --- |
| ISO logical block size | 2,048 bytes |
| ISO filesystem | 14 files, 4 subdirectories plus root; no multi-extent records |
| DATA.BIN source offset | 114,688,000 bytes |
| DATA.BIN stored size | 1,208,858,624 bytes |
| Archive entries | 6,043 |
| Exact-size rows | 1,289 |
| Empty entry spans | 3 |
| Verbatim entries | 16 |
| Code overlays | 355 |
| Sum of extracted entry lengths | 1,207,519,280 bytes |
| Largest entry | 140,812,288 bytes |

These counts describe this image, not universal parser constants. Directory agreement does not yet prove a full extraction. After extraction, compare every output entry against the independent C++ decode path and compare raw-disc file bytes with the original image. Record full-data results separately from this preflight evidence.

## Full local preparation result

On 2026-09-27 (Asia/Tokyo), Apple Silicon macOS:

- All 14 ISO files and 6,043 DATA.BIN entries were extracted locally. Three empty entries and all 355 overlay identities were retained.
- The C++ runtime path compared all **1,207,519,280 decoded entry bytes** with zero differences. A separate span reader compared all **1,323,335,431 raw-disc file bytes** with the original ISO.
- Source-anchored reuse passed without changing the manifest. Original ISO, ELF, save archive, and four source-save file hashes remained unchanged; raw reference files are read-only and ignored by Git.
- Seventeen synthetic preparation tests and eleven baseline-registration tests passed, including coordinated output/manifest tampering and source mutation during reuse. The C++ checker accepted an independent miniature fixture and rejected same-length payload corruption.
- Local evidence: `out/testing/resource-unit-tests.log`, `resource-ctest.log`, `resource-byte-crosscheck.json`, `resource-source-preservation.json`, `resource-reuse-report.json`, and `resource-inventory-report.json` under `out/testing/`. The full inventory is `profiles/mhp3rd/analysis/resources/manifest.json`.

Header classification leaves 4,535 nonempty entries unknown. Signature matches and overlay-header validation are byte-level evidence, not confirmation that animations, collision, AI, or models have been understood. No game was launched, and no new runtime resource-loader behavior was introduced.

## Limits

Successful preparation establishes byte provenance and reproducibility. It does not identify every model, skeleton, animation event, hitbox, or AI structure. Keep semantic classification confidence explicit and leave gameplay confirmation to the later user-led case workflow. The runtime continues to read the original ISO until an independently scoped loader change is accepted.
