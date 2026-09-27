# Owned encoded TMH views (ASSET-004)

`host/resources/tmh.hpp` turns a retained TMH byte slice into checked image and
palette descriptors. It owns no guest address, CPU context, Runtime or GE state.
The parser reuses `SharedBytes` from [indexed resource views](INDEXED_RESOURCE_VIEWS.md),
so returned payload slices remain valid after the enclosing view is destroyed.
This establishes a resource-data boundary for future native consumers; the
game's loader and renderer are unchanged.

The original field/ABI evidence is in [TMH_RESOURCE_CONTRACT.md](TMH_RESOURCE_CONTRACT.md).
The interface returns **encoded bytes**, not RGBA pixels. No universal swizzle,
mipmap hierarchy, sampler behavior or rendered appearance is inferred from a
file marker or from a successful metadata comparison.

## Supported API and limits

`TmhView::parse(SharedBytes, max_records)` accepts `.TMH0.14`. The hard limits
are 4,096 records, 32 subblocks per record, and 4,096 pixels per axis. A caller
can lower the record budget. Parent length and public offsets are limited to
the 32-bit file-offset domain. Counts, record strides, block strides and
payload spans are checked before creating views. Unsupported or malformed
inputs throw `invalid_argument`; a record index outside the table throws
`out_of_range`.

`records()` exposes immutable record/block metadata. `reserved_word()` and
the record/block raw words retain fields whose semantics are unknown. Tags are
preserved rather than used as an invented authoritative type enum. `parent()`
keeps the entire source slice; `consumed_bytes()` identifies the end of the
counted records while preserving any unindexed outer tail.

`descriptor(record_index, palette_selector=0)` exposes TMH-relative image and
optional palette offsets, raw format IDs, width/height and the sign-extended
palette count. Palette selection follows the original positional rule:
`preceding_subblocks + palette_selector`, checked against the record's blocks.
For format 8, the palette fields are zero/absent as in the original helper.

The current supported image formats are 4, 5 and 8; palette formats are 1 and
3. These cover all discovered inputs. Encoded length must accommodate a minimum
bit/byte/block count: CLUT4 uses `ceil(width*height/2)` only as a lower bound,
CLUT8 uses the pixel count, and DXT1 uses whole 4×4 blocks. The lower bound does
not prescribe row packing or swizzle. Palette payloads must accommodate their
positive entry count. The full payload, including extra padding, is retained.
Raw unknown words, nonzero tags and outer tails are not required to be zero.

## Whole-corpus and original-code evidence

The reproducible inventory rehashes all 6,043 raw entries and discovers both
2,244 indexed TMH children and 12 standalone entries. These 2,256 inputs contain
8,866 records: 8,813 have palettes and 53 are palette-free format-8 records.
Standalone entries retain their outer tails; the two 68×68 CLUT4 records retain
eight bytes beyond their nominal pixel count.

`mhp3rd_tmh_descriptor_oracle` links production AOT objects and requires the
supported full ELF plus the complete 236-byte descriptor-helper fingerprint.
For each record it compares native offsets/formats/dimensions/counts with the
original 24-byte descriptor, using separate input/output buffers and palette
selector zero. The same input executes in the bounded interpreter. Full CPU
state, 64-byte output including canaries, and unchanged source bytes must agree.
Both original paths completed all 8,866 records; the maximum interpreter path
was 336 slices, below the 8,192-slice bound. No game, allocator, texture-command
builder or GPU runs in this harness.

The native core also passed 27 authored synthetic checks under strict C++20
warnings and AddressSanitizer/UndefinedBehaviorSanitizer. Tests cover selectors,
padding/tails, unknown words, owned nested lifetime, dimensions, counts,
strides and truncated image/palette payloads. Five structural-inventory checks
separately cover malformed boundaries and decoded-parent offset composition.

## Reproduction

```sh
cmake --build out/mhp3rd --target mhp3rd_tmh_tests mhp3rd_tmh_descriptor_oracle -j2
ctest --test-dir out/mhp3rd -R '^mhp3rd_tmh_tests$' --output-on-failure
python3 profiles/mhp3rd/tools/inspect_tmh_layout.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --inventory out/testing/indexed-resource-bundle-audit-final.json \
  --output out/testing/new-tmh-layout.json
python3 profiles/mhp3rd/tools/check_tmh_views.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --layout-report out/testing/new-tmh-layout.json \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --oracle out/mhp3rd/bin/mhp3rd_tmh_descriptor_oracle \
  --output out/testing/new-tmh-descriptor-gate.json
```

The wrapper limits the original-code subprocess to 60 seconds and each copied
TMH fixture to 16 MiB. It binds the source manifest, layout report and oracle
executable hashes, refuses existing outputs, and publishes completed metadata
atomically outside immutable resource folders. Payload copies remain local;
none are exported by these commands.

Final local evidence is `out/testing/tmh-layout-complete-inventory.json`,
`out/testing/tmh-owned-view-original-gate.json` and `out/testing/tmh-sanitizers.log`.
The next boundary is pixel decoding with an explicit, evidenced layout choice
or original GE state; file-only guesses must not silently select swizzle.
Visual/gameplay compatibility, full original object ownership and the live
source-ID-to-object mapping remain unverified. Both delivered user pairs stay
unchanged.
