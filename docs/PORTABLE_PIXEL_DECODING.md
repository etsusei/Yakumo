# Portable pixel decoding

The resource layer can decode an explicitly described byte window without a
guest CPU, guest address space, renderer, environment variables or game boot.
`host/resources/pixel_decode.hpp` accepts immutable texel/palette spans and
returns numeric RGBA pixels (red in the low byte), required byte counts and an
explicit error. This is an offline native resource boundary; the production
renderer still uses its existing decoder.

## Inputs and preserved behavior

- All eleven current software formats are supported: four direct color
  formats, four palette-index formats, and DXT1/3/5 in the existing PSP resource
  byte order. Direct RGB565 and DXT endpoint channel order differ intentionally.
- Width, height, row stride, layout, palette format and palette mapping are
  explicit. A zero stride selects width. Dimensions are bounded to 4096 per
  axis, stride to 65535, and callers can lower the default 16-million-pixel
  output budget. The legacy comparison covers its supported 1024-axis domain.
- A strict swizzle request requires 16-byte rows and eight-row blocks.
  `LegacySwizzleCompatibility` explicitly retains the current software's
  unaligned no-transform behavior and reports that bypass. It is not evidence
  of physical PSP behavior.
- The full row-stride window must be present. Sampling beyond one row can
  reach the next row within that window, matching the existing software;
  sampling beyond the copied window retains opaque black. Extra caller bytes
  are not silently added to the window. Missing required source bytes cause
  failure rather than zero padding.
- Palette mapping is `((index >> shift) & mask) | (offset << 4)`. Only used
  entries must be present. A missing-palette error reports a lower bound
  through the first missing entry, not a complete scan of later pixels.
- DXT uses block-rounded dimensions and ignores row stride and swizzle after
  an explicit layout is selected. Partial edge blocks are clipped to the
  requested output extent.

Errors return no output pixels. Inputs are unchanged on success or failure;
allocation failure retains the standard exception behavior.

## TMH builder profile

`host/resources/tmh_pixel_plan.hpp` makes a separate, explicit choice of the
recovered model builder profile from [the layout contract](TEXTURE_LAYOUT_CONTRACT.md).
It does not infer layout from a TMH marker or assert that every file reaches
that builder during gameplay.

The encoded rectangle keeps the file's dimensions. The model-builder canvas
rounds each axis to the next power of two, while retaining the file's original
row stride. Each plan retains the image/palette offsets and distinguishes
encoded payload length from the required decoder window. Some full canvases
therefore need bytes from outside the image block, TMH child or decoded entry.
The adapter does not read or synthesize those bytes.

## Verification layers

1. Synthetic core tests cover known color/alpha values, palette transforms,
   block order, swizzle, cross-row behavior, truncated windows and rejected
   state. Plan tests include actual observed dimension shapes using synthetic
   payloads. Both also run under AddressSanitizer and UndefinedBehaviorSanitizer.
2. `mhp3rd_pixel_decode_legacy_tests` links the unchanged production decoder
   as a test-only oracle. Separate fresh fast/slow processes each pass 212
   comparisons across all eleven formats and 176 snapshot comparisons. These
   include 36 compatibility bypasses, 77 cross-row cases and three explicit
   rejection cases. The test-only performance hook selects the current
   software path; no game or graphics device is created.
3. Corpus validation passed for all 2,256 TMH inputs: 8,866 encoded rectangles
   and 8,856 file-backed model-builder canvases, in both fresh fast and slow
   legacy processes. There were zero pixel differences and zero differences
   in the 8,856 comparable top-left rectangles. Four records use the explicit
   unaligned compatibility behavior and are rejected by strict swizzle.
   The available canvas comparisons cover 220,404,480 pixels per route.
4. The remaining ten canvases require bytes beyond the decoded root entry and
   are reported as missing context, with no fabricated output: entry/record
   pairs `4074/2`, `4074/3`, `4078/7`, `4078/8`, `4079/26`, `4080/1`,
   `4080/2`, `4081/1`, `4083/15`, and `4084/4`. Their encoded rectangles
   still passed. All 83 image-boundary extensions and 34 TMH-boundary
   extensions are retained in the report. A separate root audit checked every
   reported extent, stride, palette hash and source window against the prior
   structural inventory, including the exact missing-context identities.
5. Six synthetic executable-level tests verify missing-context reporting,
   real parent bytes beyond a child, corrupt hashes, duplicate rows, actual
   fast/slow mode identity and no overwrite of existing reports.

The corpus tool records source/binary and manifest hashes and runs 70 shards
twice, with a 60-second cap on each oracle process. Generated reports remain
under the ignored `out/testing/` directory. Reproduce with a new output path:

```sh
cmake --build out/mhp3rd --target mhp3rd_tmh_pixel_oracle -j2
python3 profiles/mhp3rd/tools/check_tmh_pixels.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --layout-report out/testing/tmh-layout-complete-inventory.json \
  --oracle out/mhp3rd/bin/mhp3rd_tmh_pixel_oracle \
  --output out/testing/tmh-pixel-gate.json
python3 profiles/mhp3rd/tests/test_tmh_pixel_oracle.py \
  --oracle out/mhp3rd/bin/mhp3rd_tmh_pixel_oracle -v
```

Software equality does not establish visible UVs, animation, gameplay, hardware
fidelity or performance improvement. Production integration, actual source-to-
draw observations and user acceptance remain separate tasks. Existing paired
applications and their recorded identities remain unchanged.
