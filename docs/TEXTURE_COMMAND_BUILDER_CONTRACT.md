# Original texture-command builder boundary (ASSET-009)

This is an original-consumer audit and bounded execution contract for the builder at
`0x0889E5C0..0x0889E7D0` for the supported executable. It identifies the
guest ABI, command words, and memory/CPU effects needed by a bounded original
execution gate. It does not certify a native replacement, connect a particular
TMH source ID to a live object, or establish rendered appearance. The existing
[TMH resource](TMH_RESOURCE_CONTRACT.md) and
[texture layout](TEXTURE_LAYOUT_CONTRACT.md) contracts describe the resource
and current software-decoder sides of this boundary.

## Evidence and coordinate spaces

**Observed:** `profiles/mhp3rd/game/EBOOT.ELF` is little-endian MIPS ELF32,
SHA-256 `55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c`.
Its file-backed load segment begins at guest `0x08804000`, file `0x2CB4`.
The complete 528-byte builder span is file `0x9D274..0x9D484`, SHA-256
`c4f5b737eb673e30b7d21425ef67ed9d2c3bab11228aadcbb389e25f1519694b`.
Its only call is the descriptor helper at `0x08876A10..0x08876AFC`, file
`0x756C4..0x757B0`, SHA-256
`2da1298722400450aba70d5585e98b9b2a2ca3c0dd2057089700aa9375a76874`.
The table at guest `0x089CED28`, file `0x1CD9DC`, has 1,025 little-endian
words, SHA-256
`c9a6501e384b7f7670e8c5c48c90f1b70b5d981dd6fc7fc37a0b7c5d566bf849`.
The generated, ignored source maps were used to trace control flow; the
identities and decisive branches, loads, stores, and call were checked against
the original words. The associated metadata is in the ignored local file
`out/testing/texture-command-static-contract.json`.

**Validated from the table bytes:** `table[0] = 0` and, for every index
`1..1024`, `table[n] = ceil(log2(n))`. These are the checked table entries,
not a rule for unbounded indexes. The builder also reads guest `0x08AB3668`
once per positive iteration. That address is in the ELF load segment's
memory-only range, so it has no initialized file word. The value is placed in
`r4` immediately before the helper call, but the checked helper never reads
`r4`; the read can still fault if the guest address is not mapped.

## Guest ABI and control flow

The entry inputs are register values, not a C++ structure inferred from a
caller. All guest addresses below are 32-bit and arithmetic wraps modulo
`2^32`. The state pointer must name at least the observed nine-byte prefix.

| Input | Observed use |
| --- | --- |
| `r4` | State pointer. The builder writes `u32 +4 = r5` immediately, then finishes with `u32 +0 = r6` and `u8 +8 = low8(load32(r6+8))`. It rereads state `+4` for every command slot; it never advances this field. |
| `r5` | Command-buffer base stored at state `+4`. In a nonaliasing run, slot `s` begins at `r5 + 36*s`. |
| `r6` | Child/resource base. The builder reads its `u32 +8` header count at exit and at entry when `r9 == 0`, and passes this base to the descriptor helper. It does not check a TMH marker or length. |
| `r7` | Low eight bits select the first **output slot**: `r7 & 0xFF`. Higher bits do not affect slot placement. |
| `r8` | Low eight bits select the first **source record**: `r8 & 0xFF`. Higher bits do not affect the initial record. |
| `r9` | Optional loop count. Zero selects `load32(r6+8)`; any nonzero value is used directly. The resulting word is tested as signed `> 0`. A negative override skips commands; zero itself means use the header. |
| `r29`, `r31` | Stack pointer and return address. The builder allocates an 80-byte frame and returns to the original `r31`. |

For a positive effective count `N`, iteration `i` (`0 <= i < N`) calls
`0x08876A10` with descriptor destination `new_sp+0`, child base `r6`,
record index `(r8 & 0xFF) + i`, and subblock selector zero. It then rereads
`load32(state+4)` and writes 36 bytes at
`load32(state+4) + 36*((r7 & 0xFF) + i)` under the ordinary nonaliasing
interpretation. Both the record and slot counters continue past 255 without
another byte mask. The original performs no count, output-capacity, or
record-bounds check. A positive override can exceed the child header count.

For a nonpositive effective count, the helper, global read, table reads, and
command writes are skipped. The builder still saves/restores its frame,
writes state `+4` at entry, reads child `+8` at exit, and writes state `+0`
and `+8`. In either branch the return PC is the entry `r31`.

## Descriptor and nine-command output

The helper fills all 24 bytes of a temporary descriptor. It writes an image
payload address at `+0` (`u32`), image format at `+4` (`u32`), image width and
height at `+8/+10` (`u16` each), palette payload address at `+12` (`u32`),
palette format at `+16` (`u32`), and a sign-extended `i16` palette count at
`+20` (`u32`). For formats **1, 3, 8, 9, and 10**, the helper sets the three
palette fields to zero. For every other format, it follows the record's
`+8` subblock count with selector zero and reads a palette candidate; it
does not require the observed TMH formats 4/5. It does not inspect the
record `+12` word or either subblock tag. Its record walk and palette walk
are unchecked. The existing TMH corpus supports formats 4, 5, 8 and
palette formats 1, 3 under the cited resource contract.

Let `I`, `F`, `W`, `H`, `P`, `PF`, and `C` denote the descriptor's image
address, format, width, height, palette address, palette format, and signed
count. All expressions use 32-bit OR/shift/add semantics. The following table
is in **logical command order**; the original store order is `+12`, `+0`,
`+4`, `+8`, `+16`, `+20`, `+24`, `+28`, `+32`. The order matters for aliasing.

| Slot byte offset | Command word | Source operation |
| --- | --- | --- |
| `+0` | `0xC2000000 | (F in 8..10 ? 0 : 1)` | `0x0889E6D0` unsigned `(F - 8) < 3`, then conditional move at `0x0889E6E4`. |
| `+4` | `0xC3000000 | F` | GE texture format. |
| `+8` | `0xA0000000 | (I & 0x00FFFFFF)` | Image address low 24 bits. |
| `+12` | `0xA8000000 | ((I >> 8) & 0x00FF0000) | W` | Image address high eight bits and raw width, combined by OR. |
| `+16` | `0xB8000000 | (table[H] << 8) | table[W]` | Rounded GE raster exponents; table reads are four bytes each. |
| `+20` | `0xC500FF00 | PF` | CLUT format/mask command. |
| `+24` | `0xB0000000 | (P & 0x00FFFFFF)` | Palette address low 24 bits. |
| `+28` | `0xB1000000 | ((P >> 8) & 0x00FF0000)` | Palette address high eight bits. |
| `+32` | `0xC4000000 | (((uint32_t)C + 7) >> 3)` | CLUT load word; unsigned shift follows 32-bit addition. |

**Observed:** all nine words are emitted for *every* positive iteration,
including format 8. For format 8 the helper supplies zero palette fields, so
the last four palette commands are `0xC500FF00`, `0xB0000000`,
`0xB1000000`, and `0xC4000000`. This supplements the indexed-format facts in
the earlier layout contract. The builder makes commands only; it does not
read encoded image or palette pixels.

## Memory and CPU footprint

The following footprint assumes successful execution and disjoint state,
child, output, stack, table, and global regions. The original has no alias
guard. Overlap can alter later reads and output addresses, so a simple typed
core must either reject it or preserve the original operation order.

| Region | Reads and writes |
| --- | --- |
| State | `u32 +4` written on entry, read once per positive iteration; `u32 +0` and `u8 +8` written on exit. No other state byte is directly touched. |
| Child | `u32 +8` read on exit, and also at entry only if `r9 == 0`. The helper walks size-prefixed records from child `+16`, reading each preceding record's `u32 +0` for a positive index. At the selected record it reads first-subblock format `u32 +24` (twice), width/height `u16 +28/+30`, and, on its palette path, record `u32 +8`, preceding subblock `u32 +0` sizes, palette format `u32 +8`, and palette count `i16 +12` relative to the selected palette subblock. These offsets describe reads, not a checked parse. |
| Global/table | One `u32` read at `0x08AB3668` per positive iteration; then `u32` reads at `0x089CED28 + 4*H` and `+ 4*W`. No writes. |
| Output | Nine `u32` stores per positive iteration at the slot offsets above; no command-buffer cursor update. |
| Stack | New `sp..sp+23` is written by the helper and read by the builder on positive iterations. `sp+32..sp+71` is written with saved `r16..r23`, `r30`, and `r31`, then read back on both paths. `sp+24..31` and `sp+72..79` are untouched. The frame is 80 bytes and `r29` is restored. |

The builder and helper contain no floating-point, VFPU, multiply/divide, or
HI/LO operations. **Observed static CPU result:** `r0/r1`, `r11..r15`,
`r16..r31`, all FPRs and FP control, HI/LO, and VFPU data/control are
unchanged on normal return; `r29` and `r31` are restored. On the no-command
path, `r2 = load32(child+8)` at exit and `r3 = entry_r7 & 0xFF`; `r4..r10`
retain their entry values. On a positive path, the helper and builder use
`r2..r10` as scratch. At final return, `r2` is the final `load32(child+8)`, `r3` is
`0xC4000000`, `r4` the final `B1` word, `r5` the final `B0` word, `r6` the
final command-slot address, `r7 = table_base + 4*H`, `r8 = table_base + 4*W`,
`r9` the final `C3` word, and `r10` the final `A0` word. These are guest
register effects, not a proposed public C++ return value.

## Checked domain and next boundary

The original accepts raw words without validation. Invalid or unsupported
inputs may read or write arbitrary guest memory, fault, or run for unbounded
time: an out-of-range record index, zero/overflowing record or subblock
stride, count beyond a bounded child, excessive/negative palette count,
`W`/`H` past the 1,025-word table, unmapped global, undersized output,
unaligned word/halfword access, or 32-bit address wrap all need an explicit
rejection or bounded fallback in a
portable implementation. The observed TMH dimensions are at most 1,024,
but that corpus fact is not a check inside this function. The `C4` expression
does not validate negative `C`, and raw `F`/`PF` values are ORed into command
words without sanitizing their high bits.

A useful portable boundary has an owned, checked span of TMH descriptors plus
explicit `first_record`, `first_slot`, header count, optional signed override,
and 32-bit image/palette **address tokens** supplied by a guest adapter. Its
result is a bounded sequence of nine-word command blocks and the three
observed state fields. The core should validate indexes, extents, address
arithmetic, output capacity, and descriptor format before producing a complete
result. The address tokens remain necessary for command-byte comparison;
owned image/palette byte spans belong to the separate pixel-consumer boundary
and are not read by this builder. An adapter can then reproduce the guest
register/stack effects where required. No native hook is justified by this
static audit alone.

## Bounded original execution gate

**Validated:** `tests/texture_command_oracle.cpp` links the existing compiled
original builder and separately interprets only the two certified code spans.
It compares both against an independently expressed command/state model.
Every run compares the full GPR/FPR/VFPU/control/HI/LO/PC context, object and
command-buffer canaries, the complete 80-byte frame inside a larger guarded
stack region, unchanged source bytes, all 1,025 table words and the global
read word. This is selected-region verification supported by the static
store audit, not a claim that every RAM byte is scanned.

The complete local inventory passed: 2,256 TMH inputs, 8,866 descriptors,
4,512 original builder calls per execution path, and 11,122 nine-word command
slots. Each input uses the default full count and a selected one-record
override. The maximum interpreted call required 19,168 single-instruction
slices. Twenty-four additional synthetic cases passed with 36 command slots and
a maximum of 3,420 slices. They cover zero and signed-negative counts,
positive overrides, high input bits discarded by the initial byte masks,
source/destination indexes continuing past 255, low-byte state-count
truncation, compressed zero-palette commands, 1,024-axis table limits, and
cached/uncached address mirrors in disjoint physical regions.
Separate synthetic metadata exercises formats 1, 3, 9 and 10 with zero palette
fields, covering the helper's remaining observed no-palette branches. This
does not widen the production TMH reader's file-format domain.

These tests do not cover overlapping regions. In particular, a cached and an
uncached address can still alias the same physical RAM; the tested mirror
cases preserve distinct source/object/command/stack regions. Out-of-range
positive walks are rejected before the original is invoked. Original malformed
resource behavior is not a safety guarantee for a portable replacement.

The reproducible metadata-only runner is:

```sh
cmake --build out/mhp3rd --target mhp3rd_texture_command_oracle -j2
python3 profiles/mhp3rd/tools/check_texture_commands.py \
  --workspace profiles/mhp3rd/analysis/resources \
  --layout-report out/testing/tmh-layout-complete-inventory.json \
  --elf profiles/mhp3rd/game/EBOOT.ELF \
  --oracle out/mhp3rd/bin/mhp3rd_texture_command_oracle \
  --output out/testing/texture-command-original-gate-final.json
```

The runner binds ELF, inventory, resource, source and binary hashes; verifies
all referenced parent/child identities before and after execution; uses 128
bounded corpus shards plus the synthetic process; and refuses to replace an
existing report. Each process has a 60-second timeout and each interpreted
call has a 2,000,000-slice ceiling. Source payloads and reports remain local.

**Unverified:** malformed-input behavior, overlapping memory, a live
source-ID-to-object map, other command builders, sampled pixels, presentation,
and performance. No production hook was installed. Later paired gameplay remains user-led under
[the development plan](DEVELOPMENT_PLAN.md).
