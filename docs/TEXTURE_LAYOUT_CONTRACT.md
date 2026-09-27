# Texture layout and pixel-decode boundary (ASSET-005)

This contract joins the owned encoded TMH views in
[OWNED_TMH_VIEWS.md](OWNED_TMH_VIEWS.md) to an explicit, portable pixel input.
It describes one statically identified original texture-command builder and
the **current Yakumo software decoder**. It does not establish a universal TMH
file flag, physical PSP sampling behavior, a live source-ID-to-object mapping,
or visual/gameplay acceptance. The existing applications and renderer are not
changed by this contract.

The original consumer and source identities are recorded in
[TMH_RESOURCE_CONTRACT.md](TMH_RESOURCE_CONTRACT.md). The relevant builder is
`0x0889E5C0..0x0889E7D0` in the supported ELF. Its `0xB8` exponent lookup
at `0x089CED28` is the independently checked 1,025-word table with SHA-256
`c9a6501e384b7f7670e8c5c48c90f1b70b5d981dd6fc7fc37a0b7c5d566bf849`.
The branch at `0x0889E6D0` and conditional move at `0x0889E6E4` emit
`0xC2000001` for indexed formats 4/5 and `0xC2000000` for format 8. The
builder emits `0xC500FF00 | palette_format` for indexed textures. These are
properties of this caller, not fields stored in TMH. Another caller at
`0x08845AD0` writes the `0xC2` low bit unconditionally; its resource mapping
must be established before applying this profile there.

## Three extents and two pixel views

`TmhDescriptor.width` and `.height` are the raw image dimensions in the first
subblock header. Its `image` is the entire bounded payload, including padding;
it is **not** a promise that a renderer read ends at the payload boundary.
This builder writes raw width to GE texture buffer-width command `0xA8`, while
`0xB8` receives `ceil(log2(width))` and `ceil(log2(height))`. The local GE state
turns `0xB8` into a power-of-two **raster width and height**. The raster width
is independent of the buffer stride. The visible part of a draw is further
controlled by UV coordinates, transform, filtering, wrapping, and geometry;
neither raw dimensions nor raster dimensions alone specify it.

| Quantity | Builder-profile value | Purpose |
| --- | --- | --- |
| Raw image dimensions | TMH first-block `u16 width, u16 height` | Bounded source/payload preview and provenance. |
| Buffer stride | `width` texels from `0xA8` | Row addressing for current indexed decoding. |
| GE raster extent | `2^ceil(log2(width)) × 2^ceil(log2(height))` from `0xB8` | Current decoder's output grid and read height. |
| Encoded window | Bytes starting at the image payload pointer, length calculated below | May cross the image block, TMH child, or decoded root entry. |
| Palette window | Bytes starting at the selected palette payload pointer | Indexed colors; its declared TMH count is separate from decoder reads. |

A **payload preview** decodes only the raw TMH dimensions from the retained
image payload, with the builder's layout profile supplied explicitly. It can
help inspect resource data; it is not the GE image. A **GE canvas decode** uses
the `0xB8` raster extent and exact encoded window at the address emitted by
the builder. A full sampled draw needs the canvas plus the other GE state and
its UVs. In the measured corpus the top-left raw rectangle of the two software
decodes is structurally comparable: raw widths are even, indexed raw heights
are multiples of eight except the four records where the current unswizzle
helper skips transformation in both views, and non-power-of-two DXT1 widths
do not occur. This is a conditional software-path observation, not proof of
live presentation or hardware equivalence.

## Current software read and color rules

The GE parser in `profiles/mhp3rd/host/gpu/ge_state.cpp:667..687` maps
`0xA8`, `0xB8`, `0xC2`, `0xC3`, and `0xC5` into `TextureState`. The decoder in
`profiles/mhp3rd/host/gpu/texture_decode.cpp:347..455` uses those fields:

| TMH format | Renderer enum | Encoded window for GE canvas | Layout under this builder |
| --- | --- | --- | --- |
| 4 | `Clut4` | `floor(stride_texels × 4 / 8) × raster_height` bytes | `0xC2` requests 16-byte × 8-row swizzle; low nibble is even `x`, high nibble odd `x`. |
| 5 | `Clut8` | `stride_texels × raster_height` bytes | Same swizzle; each byte is one index. |
| 8 | `Dxt1` | `ceil(raster_width / 4) × ceil(raster_height / 4) × 8` bytes | Linear sequence of PSP-order 4×4 blocks; this decoder ignores buffer stride and swizzle for DXT. |

The indexed formulas describe the current software's `row_bytes =
floor(stride_texels × bits_per_texel / 8)` and `total = row_bytes ×
raster_height`, with checked arithmetic required in a portable implementation.
The software copies that **whole** window before unswizzling. It transforms
16-byte-wide, 8-row tiles only when `row_bytes % 16 == 0` and
`raster_height % 8 == 0`; otherwise it leaves the bytes in original order.
That skip is an existing software policy, not an assertion about PSP hardware.
For each output `(x,y)`, indexed addressing starts at `y × row_bytes` and adds
`floor(x/2)` or `x`. Therefore `x` past buffer stride reads bytes assigned to
the following row, as long as the address is still inside the copied window.
Once a texel's address is past the copied window, the current software leaves
the output pixel opaque black `0xFF000000`. Missing DXT1 blocks instead make
`decode_texture()` fail. Its block path clips the last block's pixels to the
raster width and height.

The DXT1 block's first four bytes are the four rows of 2-bit selectors,
leftmost pixel in the low bits. Bytes 4..5 and 6..7 are little-endian RGB565
endpoints with red in the high five bits; these precede neither selectors nor
an assumed PC DXT block layout. The current decoder uses the endpoint order
for the four-color/one-bit-alpha choice and outputs a `uint32_t` pixel with
red in its low byte and alpha in its high byte. Those algorithm details are in
`texture_decode.cpp:108..180`. The indexed color formats observed here are
palette ID 1 (`RGBA5551`, two little-endian bytes per entry) and ID 3
(`RGBA8888`, byte order R,G,B,A). TMH has no direct-color image in this corpus;
the existing decoder's other format routines are reusable separately only
when their caller state is established.

The GE `0xC5` parser sets palette format from bits 0..1, shift from bits
2..6, mask from bits 8..15, and offset from bits 16..20. For raw index `i`,
the current decoder reads entry
`((i >> shift) & mask) | (offset << 4)`. Under this builder's
`0xC500FF00 | palette_format`, shift and offset are zero and mask is `0xFF`;
format 4 can name entries 0..15 and format 5 can name 0..255. The decoder
reads entries from the CLUT base directly; it does not clamp to the TMH
declared palette count or to `GeState::clut_load_bytes`. The builder's `0xC4`
load count and the GE parser's 32-byte-block accounting
(`ge_state.cpp:851..859`) must not be substituted for the decoder's actual
entry-read set. Every observed indexed TMH block provides at least the
builder-profile upper bound (16 entries for format 4; 256 for format 5), even
though some declared counts are much larger, including one 8,448-entry
format-5 palette. Preserve the full owned block and its count; a decoder may
read only the entries named by actual indices.

The decoder has three pathways with different acquisition rules. Its direct
fast path copies the encoded window and lazily caches palette entries by the
mapped index. The selectable slow path reads each encoded byte and palette
entry through `GuestMemory`, using the same addressing and black fallback;
they are comparable only while guest memory is stable. `snapshot_texture()`
supports uncompressed formats only and requires a contiguous host pointer for
the complete encoded window **and the first 512 palette entries**. It copies
512 even when this builder can address only 16 or 256, so snapshot failure is
not evidence that a TMH palette payload is too short for the pixels. The
snapshot decoder unswizzles its stored copy in place; a portable span decoder
should treat its source as immutable. DXT1 is decoded by the direct path.
The texture cache key uses a separate nominal-size sampling calculation
(`texture_decode.cpp:462..525`); it is not an encoded-window size oracle.

## Portable input and failure boundary

A standalone pixel core can use the planned value contract in
`profiles/mhp3rd/host/resources/pixel_decode.hpp`:

```cpp
PixelDecodeResult decode_pixels(
    const PixelDecodeSpec &spec,
    std::span<const std::uint8_t> texels,   // begins at image data address
    std::span<const std::uint8_t> palette); // begins at selected CLUT data address
```

The caller retains backing storage for both spans for the full decode. An
owned `TmhDescriptor` supplies immutable payload slices, source offsets, and
palette metadata; it does not choose `layout` or raster extent. A separate
builder-profile adapter supplies `PixelDecodeSpec` and a window resolver supplies
the **actual bytes** beyond the payload when required. That resolver can use a
checked containing decoded root entry when the original image address is
known to refer to a contiguous copy, or a bounded guest-memory capture in a
later user-controlled case. It must record provenance. Adjacent bytes are
never synthesized as zero, silently truncated to the TMH block, or borrowed
from an unrelated allocation. A payload preview uses its own raw dimensions
and bounded payload; it cannot be labeled a full GE canvas.

`PixelDecodeSpec` names the format, output width/height, stride in pixels,
layout, palette format and CLUT shift/mask/offset. Zero stride deliberately
selects the output width for general callers; this builder's adapter should
pass its known raw width explicitly. `PixelLayout::Swizzled16x8` requires
alignment. `PixelLayout::LegacySwizzleCompatibility` reproduces the existing
unaligned skip and exposes that choice in the result. DXT1 uses the linear
layout selection plus its format-specific PSP block order. The core can also
carry the other eight legacy renderer format IDs, but the TMH builder profile
here only establishes 4, 5 and 8.

The core validates format/layout pairs and CLUT fields and uses checked
products before indexing or allocation. The builder adapter applies the
current renderer's 1,024-per-axis limit (at most 1,048,576 output pixels);
the generic core has its own configurable pixel budget. Under this adapter,
even a four-byte direct format with stride at most 1,024 needs at most 4 MiB
of encoded bytes, and this TMH scope needs at most 1 MiB for CLUT8. Palette
access is validated **per mapped entry** against the supplied span, with no
512-entry or declared-count shortcut. The result reports required texel and
palette byte counts, and returns a typed failure for invalid extent or state,
unsupported format/layout, budget, short encoded window, or missing palette
entry. Failure yields no partially valid image; allocation failures can throw.
The input/output contract itself has no guest address, Runtime, GE state,
renderer object, or global file-layout guess.

## Corpus scope and remaining evidence

The full local inventory has 2,256 TMH inputs and 8,866 records: 5,063 format
4, 3,750 format 5, and 53 format 8. Under the identified builder profile,
85 records have a rounded GE extent; 83 GE decoder windows pass the bounded
image payload. Of those, 34 pass the TMH child boundary and ten standalone
records pass the decoded root entry. The ten require an explicit capture of
neighboring bytes for an exact GE canvas; they remain in scope. Four CLUT4
records have a row length not divisible by 16 and therefore hit the current
unswizzle skip: two 68×68 records (row length 34, GE extent 128×128) and two
132×64 records (row length 66, GE extent 256×64). The former also retain eight
bytes beyond their nominal image size. A 160×168 CLUT4 record illustrates a
different case: row length 80 is aligned, while its 256×256 GE canvas read
extends 7,040 bytes past its 13,440-byte image payload. A 128×216 DXT1 record
has a 128×256 GE canvas and needs 2,560 bytes after its 13,824-byte block.

These are byte-footprint results, not pixel comparisons. The ignored local
metadata reports are `out/testing/tmh-layout-complete-inventory.json`,
`out/testing/texture-layout-footprints-initial.json`, and
`out/testing/texture-layout-contract.json`. The source payloads stay in the
ignored resource workspace. Pixel equality with a standalone core, original
guest-memory windows for the ten cases, alternate callers, live asset mapping,
actual sampled regions, and visual acceptance remain separate checks. Later
gameplay cases are run by the user under the paired workflow in
[DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md).
