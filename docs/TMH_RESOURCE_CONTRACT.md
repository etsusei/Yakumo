# TMH-marked resource child contract (ASSET-003)

This contract describes the repeated `.TMH` child layout in the supported
`NPJB-40001` resource corpus. It separates direct byte observations from
meanings established by the original MIPS consumer. The game was not started;
no image was decoded or judged visually. A bounded native reader may return
owned views of encoded image and palette bytes, but this evidence does not
certify an RGBA conversion, a universal swizzle rule, or a runtime loader
replacement.

## Inputs and coordinate spaces

The read-only executable is `profiles/mhp3rd/game/EBOOT.ELF`, SHA-256
`55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
It is little-endian MIPS ELF32. For the code below, guest address `0x08804000`
maps to ELF file offset `0x2CB4`. Every code span uses guest addresses and an
exclusive end. Its file offset is `0x2CB4 + (guest address - 0x08804000)`.

The inputs are immutable, decoded DATA.BIN parent entries under
`profiles/mhp3rd/analysis/resources/raw-entries/`. The indexed-bundle inventory
in `out/testing/indexed-resource-bundle-audit-final.json` identifies each child
by source entry ID and child path. A direct child offset is relative to its
decoded parent entry. A nested child offset in the inventory is relative to
its immediate decoded parent; add each ancestor offset before addressing the
root raw entry. All offsets in the layout below are relative to the start of
the **TMH child**, never to the ISO, obfuscated DATA.BIN, or guest address.
The parent index and shared-storage lifetime contract are in
[RESOURCE_BUNDLE_CONTRACT.md](RESOURCE_BUNDLE_CONTRACT.md) and
[INDEXED_RESOURCE_VIEWS.md](INDEXED_RESOURCE_VIEWS.md).

The ignored local evidence file `out/testing/tmh-consumer-contract.json`
records code fingerprints, corpus counts, selected child identities, and the
two nominal payload-size exceptions. It contains metadata only, not resource
bytes. The whole-corpus byte check independently re-read and hashed every
selected child from its original decoded parent.

## Original consumer evidence

| Guest span | File span | SHA-256 | Relevant operation |
| --- | --- | --- | --- |
| `0x088D150C..0x088D1670` (356 bytes, including return delay) | `0xD01C0..0xD0324` | `37c3e13b3655ad2451dd52b8b4e12f2151c21283cd447697b2d404a560a9d81b` | Requests indexed child 2 and passes it to the model-object constructor. |
| `0x08876A10..0x08876AFC` (236 bytes, including both return delay slots) | `0x756C4..0x757B0` | `2da1298722400450aba70d5585e98b9b2a2ca3c0dd2057089700aa9375a76874` | Selects one variable-size record and writes a 24-byte image/palette descriptor. |
| `0x0889E5C0..0x0889E7D0` (528 bytes, including return delay) | `0x9D274..0x9D484` | `c4f5b737eb673e30b7d21425ef67ed9d2c3bab11228aadcbb389e25f1519694b` | Uses the header count, calls the descriptor helper, and builds GE texture commands. |

The first caller stores child 2 at model object `+0x160` (`0x088D872C`).
Within the constructor, one path adds a model-command offset to that stored
base (`0x088D8D44..0x088D8D54`) and passes the resulting pointer to the
texture-command builder (`0x088D8D7C`). This establishes that some model
commands refer into the retained child. It does **not** show that a particular
source ID reaches this caller, nor that every offset points to the start of
the `.TMH` header. The texture-command builder is independently called with
other resource pointers elsewhere in the ELF.

The small descriptor helper at `0x08876A10` takes a destination in register
`r5`, a child base in `r6`, a zero-based record index in `r7`, and an additional
subblock-selection value in `r8`. Register `r4` is not read as an input. The
observed texture builder sets `r8 = 0`. The helper reads the record at child
`+0x10`, then adds each record's first word to reach the requested index. It
does not compare the index with the child header count. It writes only the
24-byte destination (`+0`, `+4`, `+8`, `+10`, `+12`, `+16`, `+20`), does not use
the stack, and returns through `r31`. It clobbers `r2` through `r7` and `r9`;
`r8`, `r29`, `r31`, FPRs and VFPU registers are unchanged. The palette path
returns at `0x08876AE0` with delay slot `0x08876AE4`; the palette-free path
returns at `0x08876AF4` with delay slot `0x08876AF8`.

For a future bounded original-code oracle, first verify the exact ELF and
complete helper span above. Supply a fully bounded child and a separate
24-byte output with canaries; restrict `r7` to `0..count-1`, set `r8 = 0`,
and cap steps/record count. The original has no child-length argument or
internal bounds checks. A negative index selects the first record, and an
out-of-range positive index can read beyond the child. The local corpus has
at most 75 records per TMH child; that measured maximum is not a universal
format limit. This static contract was followed by the ASSET-004 bounded descriptor gate; see [OWNED_TMH_VIEWS.md](OWNED_TMH_VIEWS.md) for its results.

## Child layout and field meanings

The whole-corpus survey found 2,244 children whose first eight bytes are
`.TMH0.14`. All have a 16-byte header and end exactly after the number of
variable-size records at `+8`. Header word `+12` is zero in this corpus.
The consumer uses `+8` as the texture-command loop count when no caller
override is supplied. It does not check the marker, version, reserved word,
or child length.

| Child-relative field | Evidence and interpretation |
| --- | --- |
| `+0..+7` | **Observed:** ASCII `.TMH0.14` in all 2,244 children. The traced helper does not validate it. |
| `+8` `u32` | **Validated for this path:** loop count in `0x0889E5C0`; 8,782 records exactly consume the observed child spans. |
| `+12` `u32` | **Observed:** zero in all children; meaning unknown. |
| Record `+0` `u32` | **Validated:** total byte stride from this record to the next. `0x08876A24..0x08876A2C` repeatedly adds this value for nonzero indices. It includes the 16-byte record header. |
| Record `+4` `u32` | **Observed:** zero in all 8,782 records; meaning unknown. |
| Record `+8` `u32` | **Validated for the palette path:** number of preceding subblocks to skip before the palette candidate, plus `r8`. The helper starts at record `+16` and advances by each subblock's first word. It is 1 in every observed record. Calling it a mip count would exceed the evidence. |
| Record `+12` `u32` | **Observed:** 1 for 8,736 indexed records and 0 for 46 format-8 records, matching the presence of a second block. The traced helper does not read it; its intended role remains a hypothesis. |
| Subblock `+0` `u32` | **Validated on the palette path:** total byte stride to the next subblock, including its 16-byte header. |
| Subblock `+4` `u32` | **Observed:** 1 on each first block and 2 on each second block. The helper does not inspect this tag. “Image” and “palette” describe the consumer's use of those positions, not a validated tag enum. |
| First subblock `+8` `u32` | **Validated:** copied to descriptor `+4` and emitted as GE texture-format command `0xC3`. Observed values are 4, 5, and 8. The local GE renderer interprets these IDs as CLUT4, CLUT8, and DXT1. |
| First subblock `+12/+14` `u16/u16` | **Validated:** copied to descriptor `+8/+10` as width and height; the command builder uses them for GE buffer-width `0xA8` and size `0xB8`. Their observed maximum is 1,024 each. |
| First subblock `+16..` | **Validated:** descriptor `+0` points here; the command builder emits that address through GE texture-address commands `0xA0/0xA8`. No bytes are transformed by this helper. |
| Second subblock `+8` `u32` | **Validated for formats 4/5:** copied to descriptor `+16` and emitted as GE CLUT-format command `0xC5`; observed values 1 and 3. The local renderer interprets these as RGBA5551 and RGBA8888. |
| Second subblock `+12` `i16` | **Validated for formats 4/5:** sign-extended into descriptor `+20`; the builder emits GE CLUT-load command `0xC4` using `(count + 7) / 8`. Values are 16 or 256 in this corpus; upper 16 bits are zero in the observed word. |
| Second subblock `+16..` | **Validated for formats 4/5:** descriptor `+12` points here; the builder emits this address through GE CLUT-address commands `0xB0/0xB1`. |

For first-subblock format IDs 1, 3, 8, 9, or 10, the descriptor helper sets its
palette pointer, palette format, and palette count outputs to zero. In this
corpus, only format 8 takes that path. For the indexed formats 4 and 5, it
follows record `+8` size-prefixed blocks to the second block. This is a
consumer-defined positional rule. The helper itself neither checks the
subblock tags nor verifies the observed record `+12` value.

## Corpus checks and examples

All 2,244 child SHA-256 values matched the indexed inventory after re-reading
the decoded parent files. All 8,782 records and their subblocks fit their
advertised child spans, with no leftover bytes under this layout. There are
8,736 records with one image-position block and one palette-position block,
and 46 with only the image-position block. The GE format counts are 5,025
format 4, 3,711 format 5, and 46 format 8. Palette formats are 1 in 1,816
records and 3 in 6,920 records. Every palette payload length equals the
observed count times two bytes for format 1 or four bytes for format 3.

| Decoded source entry / child path | Child offset and length in decoded entry | Child SHA-256 | Representative layout |
| --- | --- | --- | --- |
| `00131 / [2]` | `0x5B390`, `0x309E0` | `1f538d40c5bd76f4986a16017e01d47916aacb3c04b317f5a371b54991923245` | 31 records; first at child `+0x10`, size `0x4230`; format 5 image block at `+0x20`, size `0x4010`, 128×128; format 1 palette follows. |
| `01489 / [2]` | `0x3530`, `0x2240` | `087cb32d3809055d2c60b1689abc138441fab9d67b308d6bf8d395c975f84c9a` | One record, size `0x2230`; format 5 image block size `0x2010`, 64×128; format 1 palette follows. |
| `04699 / [0]` | `0x150`, `0x3500` | `2ddcd9f81048b5cab66e4ec12375fb573cdc1666e18121df3b9fa42ba31a7991` | One record, size `0x34F0`; format 4 image block size `0x3490`, 160×168; palette follows. |
| `00245 / [2]` | `0x3FC0`, `0x4030` | `06b805577bec4380e9db8e116d4b9f6755ea8c84846160aff06a459e2e91f05b` | One format 8 record; no palette block. |

For the observed records, nominal encoded image byte counts match
`width × height / 2` for formats 4 and 8, and `width × height` for format 5,
except two 68×68 format-4 records (entries 4913 and 5077, child path `[3]`,
record 15): they contain 2,320 bytes where the nominal count is 2,312.
Both are eight bytes longer, consistent with 16-byte payload alignment.
The native reader must use the bounded block length and preserve any extra
bytes; it must not truncate the payload to a formula.

## Boundary for the next native reader

A safe reader can consume a shared, immutable TMH child view and expose
record views, the encoded image block and optional palette block, the raw GE
format IDs, dimensions, counts, source ID, child path, and checked offsets.
It should bound every count, stride, and nested span before creating a view,
retain the source parent through all returned slices, preserve unknown words
and padding, and reject unsupported variants without changing raw bytes.
The current corpus supports first-block formats 4/5/8 and palette formats 1/3
under the positional rule above. A malformed input need not mimic the
original consumer's unchecked reads.

The descriptor helper does not read a stored swizzle flag. A branch-condition
review of the command builder establishes that it writes `0xC2000001` for
indexed formats 4/5 and `0xC2000000` for formats 8–10. At `0x0889E6D0` the
predicate is unsigned `(format - 8) < 3`; the conditional move at `0x0889E6E4`
selects the value with low bit 1 only when that predicate is false. Thus this
builder selects swizzled indexed textures and an unset swizzle bit for DXT1.
Another caller at `0x08845AD0` writes the low bit 1 unconditionally. These are
path-specific command facts, not a stored file flag or proof of every caller.
The encoded bytes may require a swizzle or compressed-block transform before
presentation; metadata alone does not certify its result. No palette color, RGBA pixel,
visual appearance, mipmap hierarchy, exact source-ID-to-live-object mapping,
or gameplay behavior is certified here.


## Standalone entries found by the primary audit

The indexed-child inventory is not the entire discovery scope. A separate
header scan of all 6,043 raw entries found 12 standalone `.TMH0.14` resources:
1888, 1889, 4074, and 4076 through 4084. They add 84 records (38 format 4,
39 format 5, seven format 8), with 75 format-3 and two format-1 palettes.
Thus the combined scope contains 2,256 TMH inputs and 8,866 records.

Each standalone file has an unindexed outer tail after its counted records,
ranging from 544 to 2,016 bytes in this set. These bytes belong to the retained
raw entry, not the last texture block; they must not be discarded or assumed
zero. The indexed children's record chains still consume their advertised
child spans exactly. The reproducible `inspect_tmh_layout.py` tool now rehashes
every raw entry, discovers both roots and indexed children, and records origins,
child paths, checked subblock spans, hashes, unknown words and trailing lengths.
The primary independently rechecked the ELF and descriptor/builder span hashes.


## Buffer stride and GE extent

The same builder emits raw descriptor width as GE buffer stride in command
`0xA8`. It derives command `0xB8` from a lookup table at `0x089CED28` (file
`0x1CD9DC`): 1,025 little-endian words, SHA-256
`c9a6501e384b7f7670e8c5c48c90f1b70b5d981dd6fc7fc37a0b7c5d566bf849`.
The primary independently verified `table[0]=0` and
`table[n]=ceil(log2(n))` for 1 through 1,024. The B8 payload is
`table[width] | (table[height] << 8)`.

For example, 132x64 uses a stride of 132 and a GE extent of 256x64; 68x68 uses
a stride of 68 and a GE extent of 128x128. Therefore the file dimensions alone do not
establish visible extent or sampling behavior. Native pixel work must retain
this distinction and resolve row/block addressing explicitly, especially for
the nonstandard dimensions. The earlier reversed reading of the C2 condition
was corrected during review before this contract was published; no parser or
pixel behavior depended on it.
