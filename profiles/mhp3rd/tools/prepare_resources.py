#!/usr/bin/env python3
"""Prepare a checked, local-only copy of an ISO filesystem and DATA.BIN entries.

The ISO is always read in place. The published raw-disc files contain the bytes
stored on the disc; raw-entries contain DATA.BIN's deobfuscated entry bytes (or
the original bytes for entries that the game stores verbatim). No resource type
is decoded beyond its header.
"""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import sys
import tempfile

try:
    from . import databin
except ImportError:  # Direct invocation from the tools directory.
    import databin


SECTOR = 2048
MAX_DIRECTORY_BYTES = 1 << 20
MAX_ISO_DIRECTORY_BYTES = 64 << 20
MAX_ISO_RECORDS = 1_000_000
MAX_DIRECTORY_DEPTH = 32
MAX_PATH_BYTES = 4096
SCHEMA_VERSION = 1
DATA_BIN_PATH = "PSP_GAME/USRDIR/DATA.BIN"
_SHA256_RE = re.compile(r"[0-9a-fA-F]{64}\Z")


class PreparationError(ValueError):
    """The input or an existing resource workspace failed validation."""


@dataclass(frozen=True)
class _IsoRecord:
    identifier: bytes
    offset: int
    size: int
    directory: bool


@dataclass(frozen=True)
class _IsoFile:
    path: str
    offset: int
    size: int


@dataclass(frozen=True)
class _ArchiveDirectory:
    blocks: tuple[int, ...]
    sizes: dict[int, int]
    directory_blocks: int
    directory_sha256: str
    trailer_offset: int
    trailer_length: int
    trailer_sha256: str


def _read_exact_at(stream, offset: int, length: int, description: str) -> bytes:
    if offset < 0 or length < 0:
        raise PreparationError(f"negative source span for {description}")
    stream.seek(offset)
    data = stream.read(length)
    if len(data) != length:
        raise PreparationError(f"truncated source while reading {description}")
    return data


def _sha256_stream(stream, *, offset: int = 0, length: int | None = None,
                   chunk_size: int = 1 << 20) -> str:
    digest = hashlib.sha256()
    stream.seek(offset)
    remaining = length
    while remaining is None or remaining:
        count = chunk_size if remaining is None else min(chunk_size, remaining)
        chunk = stream.read(count)
        if not chunk:
            if remaining is not None:
                raise PreparationError("truncated source while hashing")
            break
        digest.update(chunk)
        if remaining is not None:
            remaining -= len(chunk)
    return digest.hexdigest()


def _sha256_file(path: Path, chunk_size: int) -> tuple[int, str]:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_size):
            total += len(chunk)
            digest.update(chunk)
    return total, digest.hexdigest()


def decrypt_chunk(data: bytes, block_address: int, offset: int) -> bytes:
    """Deobfuscate bytes at any offset within one DATA.BIN entry.

    Work is bounded by this chunk's length. The seed is advanced to the first
    requested word with modular exponentiation, matching the game's cipher.
    """
    if not 0 <= block_address <= 0xFFFFFFFF or offset < 0:
        raise PreparationError("invalid DATA.BIN cipher address or offset")
    if not data:
        return b""
    first_word, lane = divmod(offset, 4)
    words = (lane + len(data) + 3) // 4
    seeds = (block_address >> 16, block_address & 0xFFFF)
    states = [
        ((seeds[i] or databin.KEY_DEFAULT[i]) *
         pow(databin.KEY_MULTIPLIER[i], first_word, databin.KEY_MODULUS[i]))
        % databin.KEY_MODULUS[i]
        for i in range(2)
    ]
    mask_words = array("I")
    append = mask_words.append
    mult0, mult1 = databin.KEY_MULTIPLIER
    mod0, mod1 = databin.KEY_MODULUS
    state0, state1 = states
    for _ in range(words):
        state0 = state0 * mult0 % mod0
        state1 = state1 * mult1 % mod1
        append((state0 << 16) | state1)
    if sys.byteorder != "little":
        mask_words.byteswap()
    mask = memoryview(mask_words).cast("B")[lane:lane + len(data)]
    substituted = data.translate(databin.DECODE_TABLE)
    plain = int.from_bytes(substituted, "little") ^ int.from_bytes(mask, "little")
    return plain.to_bytes(len(data), "little")


def _iso_record(data: bytes, volume_bytes: int, image_bytes: int,
                description: str) -> _IsoRecord:
    if len(data) < 34 or data[0] != len(data):
        raise PreparationError(f"invalid ISO directory record length: {description}")
    name_length = data[32]
    minimum = 33 + name_length + (1 if name_length % 2 == 0 else 0)
    if name_length == 0 or minimum > len(data):
        raise PreparationError(f"invalid ISO identifier length: {description}")
    if data[1] or data[26] or data[27]:
        raise PreparationError(f"unsupported ISO extended attribute or interleaving: {description}")
    if data[25] & 0x80:
        raise PreparationError(f"unsupported ISO multi-extent record: {description}")
    lba = struct.unpack_from("<I", data, 2)[0]
    lba_mirror = struct.unpack_from(">I", data, 6)[0]
    size = struct.unpack_from("<I", data, 10)[0]
    size_mirror = struct.unpack_from(">I", data, 14)[0]
    if lba != lba_mirror or size != size_mirror:
        raise PreparationError(f"ISO extent/length endian fields disagree: {description}")
    offset = lba * SECTOR
    if offset > volume_bytes or size > volume_bytes - offset or offset > image_bytes or size > image_bytes - offset:
        raise PreparationError(f"ISO extent lies outside the image: {description}")
    return _IsoRecord(data[33:33 + name_length], offset, size, bool(data[25] & 2))


def _safe_iso_component(identifier: bytes) -> str:
    try:
        text = identifier.decode("ascii")
    except UnicodeDecodeError as error:
        raise PreparationError("non-ASCII ISO filename") from error
    if ";" in text:
        text, separator, version = text.partition(";")
        if not separator or not version.isascii() or not version.isdecimal():
            raise PreparationError("invalid ISO file version suffix")
    if (not text or text in (".", "..") or "/" in text or "\\" in text or
            any(ord(char) < 32 or ord(char) == 127 for char in text)):
        raise PreparationError("unsafe ISO path component")
    return text


def _read_iso_layout(stream, image_bytes: int) -> tuple[list[_IsoFile], list[str], int]:
    primary = _read_exact_at(stream, 16 * SECTOR, SECTOR, "ISO primary volume descriptor")
    if primary[0] != 1 or primary[1:6] != b"CD001" or primary[6] != 1:
        raise PreparationError("unsupported or invalid ISO primary volume descriptor")
    blocks = struct.unpack_from("<I", primary, 80)[0]
    mirror_blocks = struct.unpack_from(">I", primary, 84)[0]
    logical_block = struct.unpack_from("<H", primary, 128)[0]
    mirror_block = struct.unpack_from(">H", primary, 130)[0]
    if blocks == 0 or blocks != mirror_blocks or logical_block != SECTOR or mirror_block != SECTOR:
        raise PreparationError("invalid ISO volume size or logical block size")
    volume_bytes = blocks * SECTOR
    if volume_bytes > image_bytes:
        raise PreparationError("ISO volume extends beyond the source file")
    root_length = primary[156]
    if root_length < 34 or 156 + root_length > SECTOR:
        raise PreparationError("invalid ISO root directory record")
    root = _iso_record(primary[156:156 + root_length], volume_bytes, image_bytes, "root")
    if not root.directory or root.identifier != b"\0":
        raise PreparationError("invalid ISO root directory")

    files: list[_IsoFile] = []
    directories: list[str] = []
    used_paths: set[str] = set()
    record_count = 0
    queue = [(root, "", 0, frozenset())]
    while queue:
        directory, parent, depth, ancestors = queue.pop()
        if depth > MAX_DIRECTORY_DEPTH:
            raise PreparationError("ISO directory nesting limit exceeded")
        if directory.size > MAX_ISO_DIRECTORY_BYTES:
            raise PreparationError("ISO directory size limit exceeded")
        extent = (directory.offset, directory.size)
        if directory.size and extent in ancestors:
            raise PreparationError("ISO directory cycle")
        ancestors = ancestors | {extent} if directory.size else ancestors
        at = 0
        while at < directory.size:
            count = min(SECTOR, directory.size - at)
            sector = _read_exact_at(stream, directory.offset + at, count, "ISO directory sector")
            within = 0
            while within < count:
                record_length = sector[within]
                if record_length == 0:
                    if any(sector[within:]):
                        raise PreparationError("nonzero ISO bytes after directory sector terminator")
                    break
                if within + record_length > count:
                    raise PreparationError("ISO directory record crosses a sector or is truncated")
                record_count += 1
                if record_count > MAX_ISO_RECORDS:
                    raise PreparationError("ISO directory record limit exceeded")
                record = _iso_record(sector[within:within + record_length], volume_bytes,
                                     image_bytes, "directory entry")
                within += record_length
                if record.identifier in (b"\0", b"\1"):
                    continue
                component = _safe_iso_component(record.identifier)
                path = f"{parent}/{component}" if parent else component
                if len(path.encode("ascii")) > MAX_PATH_BYTES:
                    raise PreparationError("ISO path length limit exceeded")
                folded = path.casefold()
                if folded in used_paths:
                    raise PreparationError(f"duplicate or case-colliding ISO path: {path}")
                used_paths.add(folded)
                if record.directory:
                    directories.append(path)
                    queue.append((record, path, depth + 1, ancestors))
                else:
                    files.append(_IsoFile(path, record.offset, record.size))
            at += count
    files.sort(key=lambda item: item.path.casefold())
    directories.sort(key=str.casefold)
    return files, directories, blocks


def _find_archive(files: list[_IsoFile]) -> _IsoFile:
    for item in files:
        if item.path.casefold() == DATA_BIN_PATH.casefold():
            return item
    raise PreparationError("ISO has no PSP_GAME/USRDIR/DATA.BIN file")


def _read_archive_directory(stream, archive: _IsoFile) -> _ArchiveDirectory:
    if archive.size == 0 or archive.size % SECTOR:
        raise PreparationError("DATA.BIN length is not a nonzero block multiple")
    head = decrypt_chunk(_read_exact_at(stream, archive.offset, 4, "DATA.BIN directory head"), 0, 0)
    directory_blocks = struct.unpack("<I", head)[0]
    directory_bytes = directory_blocks * SECTOR
    if not 0 < directory_bytes <= MAX_DIRECTORY_BYTES or directory_bytes > archive.size:
        raise PreparationError("invalid DATA.BIN directory size")
    encrypted = _read_exact_at(stream, archive.offset, directory_bytes, "DATA.BIN directory")
    plain = decrypt_chunk(encrypted, 0, 0)
    words = struct.unpack(f"<{len(plain) // 4}I", plain)
    if words[0] != directory_blocks:
        raise PreparationError("inconsistent DATA.BIN directory head")
    end_block = archive.size // SECTOR
    if directory_blocks >= end_block:
        raise PreparationError("DATA.BIN has no entry data blocks")
    blocks = [directory_blocks]
    cursor = 1
    while cursor < len(words):
        value = words[cursor]
        cursor += 1
        if value < blocks[-1] or value > end_block:
            raise PreparationError("DATA.BIN block table is decreasing or out of bounds")
        blocks.append(value)
        if value == end_block:
            break
    if blocks[-1] != end_block:
        raise PreparationError("unterminated DATA.BIN block table")
    entry_count = len(blocks) - 1
    sizes: dict[int, int] = {}
    previous = -1
    while cursor + 1 < len(words):
        index, length = words[cursor], words[cursor + 1]
        if index >= entry_count:
            break
        span = (blocks[index + 1] - blocks[index]) * SECTOR
        if length == 0 or length > span or index <= previous:
            break  # The remaining bytes are an opaque trailer, not known table rows.
        sizes[index] = length
        previous = index
        cursor += 2
    trailer_offset = cursor * 4
    trailer = encrypted[trailer_offset:]
    return _ArchiveDirectory(tuple(blocks), sizes, directory_blocks,
                             hashlib.sha256(encrypted).hexdigest(), trailer_offset,
                             len(trailer), hashlib.sha256(trailer).hexdigest())


def _copy_source_span(stream, offset: int, length: int, destination: Path,
                      chunk_size: int) -> str:
    digest = hashlib.sha256()
    remaining = length
    stream.seek(offset)
    with destination.open("xb") as out:
        while remaining:
            chunk = stream.read(min(chunk_size, remaining))
            if not chunk:
                raise PreparationError(f"truncated source while writing {destination.name}")
            out.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
    destination.chmod(0o444)
    return digest.hexdigest()


def _entry_header_type(header: bytes) -> str:
    if not header:
        return "empty"
    for magic in (b"MWo3", b"~SCE", b"PSMF", b"RIFF", b"GIF8", b"Head", b"dbsT"):
        if header.startswith(magic):
            return magic.decode("ascii")
    return "unknown"


def _overlay_metadata(header: bytes, extracted_length: int) -> dict | None:
    if not header.startswith(databin.OVERLAY_MAGIC):
        return None
    if extracted_length < databin.OVERLAY_HEADER or len(header) < databin.OVERLAY_HEADER:
        raise PreparationError("truncated MWo3 overlay header")
    ident, load, text, data, bss, end, entry = struct.unpack_from("<7I", header, 4)
    declared = databin.OVERLAY_HEADER + text + data
    if declared > extracted_length:
        raise PreparationError("MWo3 overlay exceeds its DATA.BIN entry")
    raw_name = header[32:64].split(b"\0", 1)[0]
    return {
        "id": ident,
        "load": load,
        "text": text,
        "data": data,
        "bss": bss,
        "end": end,
        "entry": entry,
        "name": raw_name.decode("ascii", "replace"),
        "name_hex": raw_name.hex(),
        "declared_size": declared,
    }


def _extract_entry(stream, archive: _IsoFile, directory: _ArchiveDirectory,
                   index: int, destination: Path, chunk_size: int) -> dict:
    block = directory.blocks[index]
    stored_length = (directory.blocks[index + 1] - block) * SECTOR
    extracted_length = directory.sizes.get(index, stored_length)
    archive_offset = block * SECTOR
    source_offset = archive.offset + archive_offset
    head_length = min(extracted_length, databin.OVERLAY_HEADER)
    raw_head = _read_exact_at(stream, source_offset, head_length, f"DATA.BIN entry {index} header")
    transform = "verbatim" if raw_head[:4] in databin.VERBATIM_MAGICS else "deobfuscated"
    header = raw_head if transform == "verbatim" else decrypt_chunk(raw_head, block, 0)
    overlay = _overlay_metadata(header, extracted_length)
    header_type = _entry_header_type(header)
    confidence = ("validated_header" if overlay is not None else
                  "signature" if header_type not in ("unknown", "empty") else
                  "none")
    digest = hashlib.sha256()
    with destination.open("xb") as out:
        at = 0
        while at < extracted_length:
            count = min(chunk_size, extracted_length - at)
            raw = _read_exact_at(stream, source_offset + at, count, f"DATA.BIN entry {index}")
            content = raw if transform == "verbatim" else decrypt_chunk(raw, block, at)
            out.write(content)
            digest.update(content)
            at += count
    destination.chmod(0o444)
    return {
        "id": index,
        "output_path": f"raw-entries/{index:05d}.bin",
        "source_offset": source_offset,
        "archive_offset": archive_offset,
        "block_address": block,
        "stored_length": stored_length,
        "extracted_length": extracted_length,
        "length_source": "size_table" if index in directory.sizes else "block_span",
        "transform": transform,
        "sha256": digest.hexdigest(),
        "type": header_type,
        "confidence": confidence,
        "overlay": overlay,
    }


def _source_and_output(iso: Path, output: Path, expected_iso_sha256: str | None,
                       chunk_size: int) -> tuple[Path, Path, str | None]:
    if chunk_size <= 0 or chunk_size > 1 << 26:
        raise PreparationError("chunk_size must be between 1 and 67,108,864 bytes")
    if expected_iso_sha256 is not None:
        if not _SHA256_RE.fullmatch(expected_iso_sha256):
            raise PreparationError("expected ISO SHA-256 must be 64 hexadecimal characters")
        expected_iso_sha256 = expected_iso_sha256.lower()
    source = Path(iso).expanduser().resolve(strict=True)
    if not source.is_file():
        raise PreparationError("ISO source is not a regular file")
    target_input = Path(output).expanduser()
    if target_input.is_symlink():
        raise PreparationError("workspace path must not be a symlink")
    target = target_input.resolve(strict=False)
    if target == source or source.is_relative_to(target):
        raise PreparationError("workspace would overlap the original ISO")
    return source, target, expected_iso_sha256


def _write_manifest(path: Path, manifest: dict) -> None:
    with path.open("x", encoding="utf-8") as out:
        json.dump(manifest, out, indent=2, sort_keys=True)
        out.write("\n")
    path.chmod(0o444)


def _regular_file(path: Path, description: str) -> None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError as error:
        raise PreparationError(f"missing {description}") from error
    if not stat.S_ISREG(mode):
        raise PreparationError(f"{description} is not a regular file")


def _directory(path: Path, description: str) -> None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError as error:
        raise PreparationError(f"missing {description}") from error
    if not stat.S_ISDIR(mode):
        raise PreparationError(f"{description} is not a directory")


def _tree_contents(root: Path) -> tuple[set[str], set[str]]:
    files: set[str] = set()
    directories: set[str] = set()
    _directory(root, str(root))
    for current, names, filenames in os.walk(root, followlinks=False):
        current_path = Path(current)
        for name in names:
            child = current_path / name
            _directory(child, str(child))
            directories.add(child.relative_to(root).as_posix())
        for name in filenames:
            child = current_path / name
            _regular_file(child, str(child))
            files.add(child.relative_to(root).as_posix())
    return files, directories


def _expected_directory_names(paths: list[str]) -> set[str]:
    result: set[str] = set()
    for path in paths:
        parts = Path(path).parts
        for count in range(1, len(parts) + 1):
            result.add(Path(*parts[:count]).as_posix())
    return result


def _verify_output_tree(output: Path, manifest: dict, files: list[_IsoFile],
                        directories: list[str], archive: _IsoFile,
                        directory: _ArchiveDirectory, chunk_size: int, stream=None) -> None:
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise PreparationError("unsupported resource manifest schema")
    iso_manifest = manifest.get("iso")
    archive_meta = manifest.get("archive")
    if not isinstance(iso_manifest, dict) or not isinstance(archive_meta, dict):
        raise PreparationError("resource manifest has invalid ISO/archive sections")
    rows = iso_manifest.get("files")
    if not isinstance(rows, list) or len(rows) != len(files):
        raise PreparationError("manifest ISO file inventory differs from source")
    if iso_manifest.get("directories") != directories:
        raise PreparationError("manifest ISO directory inventory differs from source")
    for source_file, row in zip(files, rows):
        expected_path = f"raw-disc/{source_file.path}"
        if not isinstance(row, dict) or any(row.get(field) != value for field, value in (
                ("path", source_file.path), ("output_path", expected_path),
                ("offset", source_file.offset), ("size", source_file.size))) or not isinstance(row.get("sha256"), str):
            raise PreparationError("manifest ISO file provenance differs from source")
    archive_source_row = next(row for row in rows if row["path"] == archive.path)
    if (archive_meta.get("path") != archive.path or
            archive_meta.get("offset") != archive.offset or archive_meta.get("size") != archive.size or
            archive_meta.get("directory_blocks") != directory.directory_blocks or
            archive_meta.get("directory_bytes") != directory.directory_blocks * SECTOR or
            archive_meta.get("block_table_words") != len(directory.blocks) or
            archive_meta.get("entry_count") != len(directory.blocks) - 1 or
            archive_meta.get("size_table_rows") != len(directory.sizes) or
            archive_meta.get("directory_sha256") != directory.directory_sha256 or
            archive_meta.get("sha256") != archive_source_row["sha256"] or
            archive_meta.get("trailer_offset") != directory.trailer_offset or
            archive_meta.get("trailer_length") != directory.trailer_length or
            archive_meta.get("trailer_sha256") != directory.trailer_sha256):
        raise PreparationError("manifest DATA.BIN directory differs from source")
    entry_rows = manifest.get("entries")
    if not isinstance(entry_rows, list) or len(entry_rows) != len(directory.blocks) - 1:
        raise PreparationError("manifest DATA.BIN entry inventory differs from source")
    for index, row in enumerate(entry_rows):
        block = directory.blocks[index]
        stored_length = (directory.blocks[index + 1] - block) * SECTOR
        extracted_length = directory.sizes.get(index, stored_length)
        provenance = (
            ("id", index), ("output_path", f"raw-entries/{index:05d}.bin"),
            ("source_offset", archive.offset + block * SECTOR),
            ("archive_offset", block * SECTOR), ("block_address", block),
            ("stored_length", stored_length), ("extracted_length", extracted_length),
            ("length_source", "size_table" if index in directory.sizes else "block_span"),
        )
        if not isinstance(row, dict) or any(row.get(key) != value for key, value in provenance):
            raise PreparationError(f"manifest DATA.BIN entry {index} differs from source")
        if row.get("transform") not in ("verbatim", "deobfuscated") or not isinstance(row.get("sha256"), str):
            raise PreparationError(f"invalid manifest DATA.BIN entry {index}")
        if stream is not None:
            head_length = min(extracted_length, databin.OVERLAY_HEADER)
            raw_head = _read_exact_at(stream, archive.offset + block * SECTOR, head_length,
                                      f"DATA.BIN entry {index} header")
            transform = "verbatim" if raw_head[:4] in databin.VERBATIM_MAGICS else "deobfuscated"
            header = raw_head if transform == "verbatim" else decrypt_chunk(raw_head, block, 0)
            overlay = _overlay_metadata(header, extracted_length)
            header_type = _entry_header_type(header)
            confidence = ("validated_header" if overlay is not None else
                          "signature" if header_type not in ("unknown", "empty") else
                          "none")
            if (row["transform"] != transform or row.get("type") != header_type or
                    row.get("confidence") != confidence or row.get("overlay") != overlay):
                raise PreparationError(f"manifest DATA.BIN entry {index} header differs from source")

    for name in ("raw-disc", "raw-entries", "derived", "working"):
        _directory(output / name, name)
    expected_disc_files = {item.path for item in files}
    expected_disc_dirs = _expected_directory_names(directories)
    for item in files:
        expected_disc_dirs.update(_expected_directory_names([str(Path(item.path).parent)]))
    expected_disc_dirs.discard(".")
    disc_files, disc_dirs = _tree_contents(output / "raw-disc")
    if disc_files != expected_disc_files or disc_dirs != expected_disc_dirs:
        raise PreparationError("raw-disc contains missing or unexpected paths")
    entry_files, entry_dirs = _tree_contents(output / "raw-entries")
    if entry_files != {f"{index:05d}.bin" for index in range(len(entry_rows))} or entry_dirs:
        raise PreparationError("raw-entries contains missing or unexpected paths")
    for row in rows:
        path = output / row["output_path"]
        _regular_file(path, row["output_path"])
        size, digest = _sha256_file(path, chunk_size)
        if size != row["size"] or digest != row.get("sha256"):
            raise PreparationError(f"raw ISO output changed: {row['path']}")
    for row in entry_rows:
        path = output / row["output_path"]
        _regular_file(path, row["output_path"])
        size, digest = _sha256_file(path, chunk_size)
        if size != row["extracted_length"] or digest != row["sha256"]:
            raise PreparationError(f"raw DATA.BIN entry changed: {row['id']}")


def _verify_source_payloads(stream, manifest: dict, files: list[_IsoFile],
                            archive: _IsoFile, directory: _ArchiveDirectory,
                            chunk_size: int) -> None:
    """Anchor every stored output hash to source bytes, not only the manifest."""
    for item, row in zip(files, manifest["iso"]["files"]):
        digest = _sha256_stream(stream, offset=item.offset, length=item.size,
                                chunk_size=chunk_size)
        if digest != row["sha256"]:
            raise PreparationError(f"manifest ISO file hash differs from source: {item.path}")

    for index, row in enumerate(manifest["entries"]):
        block = directory.blocks[index]
        length = directory.sizes.get(index, (directory.blocks[index + 1] - block) * SECTOR)
        source_offset = archive.offset + block * SECTOR
        digest = hashlib.sha256()
        at = 0
        while at < length:
            count = min(chunk_size, length - at)
            raw = _read_exact_at(stream, source_offset + at, count,
                                 f"DATA.BIN entry {index} source verification")
            digest.update(raw if row["transform"] == "verbatim" else decrypt_chunk(raw, block, at))
            at += count
        if digest.hexdigest() != row["sha256"]:
            raise PreparationError(f"manifest DATA.BIN entry {index} hash differs from source")


def verify_workspace(iso: Path, output: Path, *, expected_iso_sha256: str | None = None,
                     chunk_size: int = 1048576) -> dict:
    """Check a published workspace against its source and every raw output."""
    source, target, expected = _source_and_output(iso, output, expected_iso_sha256, chunk_size)
    _directory(target, "resource workspace")
    _regular_file(target / "manifest.json", "resource manifest")
    try:
        with (target / "manifest.json").open("r", encoding="utf-8") as stream:
            manifest = json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PreparationError("cannot read resource manifest") from error
    if not isinstance(manifest, dict):
        raise PreparationError("resource manifest must be an object")
    source_section = manifest.get("source")
    iso_section = manifest.get("iso")
    if not isinstance(source_section, dict) or not isinstance(iso_section, dict):
        raise PreparationError("resource manifest has invalid source/ISO sections")
    with source.open("rb") as stream:
        source_bytes = source.stat().st_size
        source_hash = _sha256_stream(stream, chunk_size=chunk_size)
        if expected is not None and source_hash != expected:
            raise PreparationError("ISO SHA-256 does not match expected baseline")
        if source_section.get("iso") != {"size": source_bytes, "sha256": source_hash}:
            raise PreparationError("resource manifest source identity differs from ISO")
        files, directories, volume_blocks = _read_iso_layout(stream, source_bytes)
        if iso_section.get("volume_blocks") != volume_blocks:
            raise PreparationError("manifest ISO volume geometry differs from source")
        archive = _find_archive(files)
        directory = _read_archive_directory(stream, archive)
        _verify_output_tree(target, manifest, files, directories, archive, directory, chunk_size,
                            stream)
        _verify_source_payloads(stream, manifest, files, archive, directory, chunk_size)
        source_hash_after = _sha256_stream(stream, chunk_size=chunk_size)
        if source_hash_after != source_hash or source.stat().st_size != source_bytes:
            raise PreparationError("ISO changed during resource verification")
    return manifest


def prepare_resources(iso: Path, output: Path, *, expected_iso_sha256: str | None = None,
                      chunk_size: int = 1048576) -> dict:
    """Build and atomically publish raw resources, or verify an existing copy."""
    source, target, expected = _source_and_output(iso, output, expected_iso_sha256, chunk_size)
    if target.exists() or target.is_symlink():
        return verify_workspace(source, target, expected_iso_sha256=expected, chunk_size=chunk_size)
    target.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.staging-", dir=target.parent))
    try:
        for name in ("raw-disc", "raw-entries", "derived", "working"):
            (stage / name).mkdir()
        with source.open("rb") as stream:
            source_bytes = source.stat().st_size
            source_hash = _sha256_stream(stream, chunk_size=chunk_size)
            if expected is not None and source_hash != expected:
                raise PreparationError("ISO SHA-256 does not match expected baseline")
            files, directories, volume_blocks = _read_iso_layout(stream, source_bytes)
            archive = _find_archive(files)
            directory = _read_archive_directory(stream, archive)
            for name in directories:
                (stage / "raw-disc" / name).mkdir(parents=True, exist_ok=True)
            iso_rows = []
            for item in files:
                destination = stage / "raw-disc" / item.path
                destination.parent.mkdir(parents=True, exist_ok=True)
                digest = _copy_source_span(stream, item.offset, item.size, destination, chunk_size)
                iso_rows.append({"path": item.path, "output_path": f"raw-disc/{item.path}",
                                 "offset": item.offset, "size": item.size, "sha256": digest})
            archive_row = next(row for row in iso_rows if row["path"] == archive.path)
            entry_rows = []
            for index in range(len(directory.blocks) - 1):
                destination = stage / "raw-entries" / f"{index:05d}.bin"
                entry_rows.append(_extract_entry(stream, archive, directory, index, destination, chunk_size))
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "source": {"iso": {"size": source_bytes, "sha256": source_hash}},
            "iso": {"volume_blocks": volume_blocks, "directories": directories,
                    "files": iso_rows},
            "archive": {
                "path": archive.path,
                "offset": archive.offset,
                "size": archive.size,
                "sha256": archive_row["sha256"],
                "directory_blocks": directory.directory_blocks,
                "directory_bytes": directory.directory_blocks * SECTOR,
                "directory_sha256": directory.directory_sha256,
                "block_table_words": len(directory.blocks),
                "entry_count": len(entry_rows),
                "size_table_rows": len(directory.sizes),
                "trailer_offset": directory.trailer_offset,
                "trailer_length": directory.trailer_length,
                "trailer_sha256": directory.trailer_sha256,
            },
            "entries": entry_rows,
        }
        _write_manifest(stage / "manifest.json", manifest)
        # Catch mistakes in publication logic before the staged directory is visible.
        with source.open("rb") as stream:
            _verify_output_tree(stage, manifest, files, directories, archive, directory,
                                chunk_size, stream)
            source_hash_after = _sha256_stream(stream, chunk_size=chunk_size)
        if source_hash_after != source_hash or source.stat().st_size != source_bytes:
            raise PreparationError("ISO changed during resource preparation")
        if target.exists() or target.is_symlink():
            raise PreparationError("resource workspace appeared during preparation")
        os.replace(stage, target)
        return manifest
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("iso", type=Path, help="original PSP ISO, read in place")
    parser.add_argument("--output", required=True, type=Path, help="ignored local resource workspace")
    parser.add_argument("--verify-only", action="store_true", help="check an existing workspace")
    parser.add_argument("--expected-iso-sha256", help="frozen source ISO SHA-256")
    args = parser.parse_args(argv)
    try:
        operation = verify_workspace if args.verify_only else prepare_resources
        manifest = operation(args.iso, args.output, expected_iso_sha256=args.expected_iso_sha256)
    except (OSError, PreparationError) as error:
        parser.exit(1, f"resource preparation: {error}\n")
    print(f"Verified {len(manifest['iso']['files'])} ISO files and {len(manifest['entries'])} DATA.BIN entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
