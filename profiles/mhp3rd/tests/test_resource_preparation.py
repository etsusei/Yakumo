"""Offline resource-preparation checks using original, synthetic ISO/archive bytes.

The fixture deliberately constructs its own ISO9660 records and DATA.BIN
ciphertext. It does not call either production parser to create expectations.
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from profiles.mhp3rd.tools import databin  # noqa: E402
from profiles.mhp3rd.tools import prepare_resources as resources  # noqa: E402


SECTOR = 2048
ISO_SECTORS = 64
ARCHIVE_SECTOR = 30


def digest(data):
    return hashlib.sha256(data).hexdigest()


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def encode_entry(plain, block_address):
    """Apply the documented transform directly, independently of decrypt()."""
    padded = plain + bytes(-len(plain) % 4)
    upper = (block_address >> 16) & 0xFFFF or 0x2345
    lower = block_address & 0xFFFF or 0x7F8D
    transformed = bytearray()
    for offset in range(0, len(padded), 4):
        upper = upper * 0x2345 % 0xFFD9
        lower = lower * 0x7F8D % 0xFFF1
        mask = struct.pack("<I", upper << 16 | lower)
        transformed.extend(padded[offset + j] ^ mask[j] for j in range(4))
    return bytes(transformed).translate(databin.ENCODE_TABLE)[: len(plain)]


def directory_record(extent, size, name, *, directory=False):
    name = bytes(name)
    length = 33 + len(name) + (len(name) % 2 == 0)
    record = bytearray(length)
    record[0] = length
    record[2:10] = both32(extent)
    record[10:18] = both32(size)
    record[18:25] = bytes((126, 9, 26, 0, 0, 0, 0))
    record[25] = 2 if directory else 0
    record[28:32] = both16(1)
    record[32] = len(name)
    record[33 : 33 + len(name)] = name
    return bytes(record)


def path_table_record(extent, parent, name, *, big_endian=False):
    endian = ">" if big_endian else "<"
    return (
        bytes((len(name), 0))
        + struct.pack(endian + "IH", extent, parent)
        + name
        + bytes(len(name) % 2)
    )


def synthetic_overlay():
    code = b"\x10\x20\x30\x40\x50"
    data = b"\xa1\xa2\xa3"
    load = 0x0A001780
    name = b"test_task.ovl"
    header = b"MWo3" + struct.pack(
        "<7I", 1, load, len(code), len(data), 4,
        load + 64 + len(code) + len(data), load + 68 + len(code) + len(data),
    ) + name.ljust(32, b"\0")
    assert len(header) == 64
    return header + code + data


def synthetic_archive(*, blocks=None, exact_sizes=None, trailer=None):
    """Return encoded archive bytes and each expected extracted entry."""
    entries = [
        b"",
        synthetic_overlay(),
        b"Hello world",
        b"Head" + bytes((i * 37 + 11) % 256 for i in range(SECTOR - 4)),
        b"PSMF" + bytes((i * 17 + 7) % 256 for i in range(119)),
        b"~SCE" + b"synthetic module stub!!"[:23],
    ]
    assert len(entries[5]) == 27
    if blocks is None:
        blocks = [1, 1, 2, 3, 4, 5, 6]
    if exact_sizes is None:
        exact_sizes = [(1, len(entries[1])), (2, len(entries[2])),
                       (4, len(entries[4])), (5, len(entries[5]))]
    if trailer is None:
        # No documented pair count exists. This invalid pair starts an opaque
        # trailer and must not force a made-up number of size rows.
        trailer = struct.pack("<II", 0xFFFFFFFF, 0xFEDCBA98) + bytes(
            (i * 71 + 3) % 256 for i in range(80)
        )
    directory = bytearray([0xD3] * SECTOR)
    table = b"".join(struct.pack("<I", value) for value in blocks)
    table += b"".join(struct.pack("<II", *pair) for pair in exact_sizes)
    table += trailer
    assert len(table) <= SECTOR
    directory[: len(table)] = table
    archive = bytearray([0xA5] * (6 * SECTOR))
    archive[:SECTOR] = encode_entry(directory, 0)
    for index, plain in enumerate(entries):
        start = blocks[index] * SECTOR
        if plain[:4] in (b"PSMF", b"~SCE"):
            archive[start : start + len(plain)] = plain
        else:
            archive[start : start + len(plain)] = encode_entry(plain, blocks[index])
    return bytes(archive), entries


class MiniImage:
    """A valid one-sector-per-directory ISO9660 image with mutation offsets."""

    def __init__(self, archive=None):
        if archive is None:
            archive, self.entries = synthetic_archive()
        else:
            self.entries = None
        self.archive = archive
        self.bytes = bytearray(ISO_SECTORS * SECTOR)
        self.record_offsets = {}
        self.file_bytes = {
            "README.TXT": b"A synthetic readme\n",
            "MIXED0.TXT": b"Another test file\n",
            "PSP_GAME/USRDIR/EBOOT.BIN": b"synthetic executable marker",
            "PSP_GAME/USRDIR/EMPTY.DAT": b"",
            "PSP_GAME/USRDIR/DATA.BIN": archive,
        }

        self._put_directory(20, [
            (".", directory_record(20, SECTOR, b"\0", directory=True)),
            ("..", directory_record(20, SECTOR, b"\1", directory=True)),
            ("PSP_GAME", directory_record(21, SECTOR, b"PSP_GAME", directory=True)),
            ("README.TXT;1", directory_record(40, len(self.file_bytes["README.TXT"]), b"README.TXT;1")),
            ("MIXED0.TXT;1", directory_record(41, len(self.file_bytes["MIXED0.TXT"]), b"MIXED0.TXT;1")),
        ], prefix="")
        self._put_directory(21, [
            (".", directory_record(21, SECTOR, b"\0", directory=True)),
            ("..", directory_record(20, SECTOR, b"\1", directory=True)),
            ("USRDIR", directory_record(22, SECTOR, b"USRDIR", directory=True)),
        ], prefix="PSP_GAME/")
        self._put_directory(22, [
            (".", directory_record(22, SECTOR, b"\0", directory=True)),
            ("..", directory_record(21, SECTOR, b"\1", directory=True)),
            ("DATA.BIN;1", directory_record(ARCHIVE_SECTOR, len(archive), b"DATA.BIN;1")),
            ("EBOOT.BIN;1", directory_record(42, len(self.file_bytes["PSP_GAME/USRDIR/EBOOT.BIN"]), b"EBOOT.BIN;1")),
            ("EMPTY.DAT;1", directory_record(43, 0, b"EMPTY.DAT;1")),
        ], prefix="PSP_GAME/USRDIR/")

        pvd = bytearray(SECTOR)
        pvd[0] = 1
        pvd[1:6] = b"CD001"
        pvd[6] = 1
        pvd[8:40] = b"SYNTHETIC".ljust(32, b" ")
        pvd[40:72] = b"RESOURCE_TEST".ljust(32, b" ")
        pvd[80:88] = both32(ISO_SECTORS)
        pvd[120:124] = both16(1)
        pvd[124:128] = both16(1)
        pvd[128:132] = both16(SECTOR)
        little_table = b"".join((
            path_table_record(20, 1, b"\0"),
            path_table_record(21, 1, b"PSP_GAME"),
            path_table_record(22, 2, b"USRDIR"),
        ))
        big_table = b"".join((
            path_table_record(20, 1, b"\0", big_endian=True),
            path_table_record(21, 1, b"PSP_GAME", big_endian=True),
            path_table_record(22, 2, b"USRDIR", big_endian=True),
        ))
        assert len(little_table) == len(big_table) == 40
        pvd[132:140] = both32(len(little_table))
        pvd[140:144] = struct.pack("<I", 23)
        pvd[148:152] = struct.pack(">I", 24)
        pvd[156:190] = directory_record(20, SECTOR, b"\0", directory=True)
        self.record_offsets["PVD_ROOT"] = 16 * SECTOR + 156
        self.bytes[16 * SECTOR : 17 * SECTOR] = pvd
        terminator = bytearray(SECTOR)
        terminator[:7] = b"\xffCD001\x01"
        self.bytes[17 * SECTOR : 18 * SECTOR] = terminator
        self.bytes[23 * SECTOR : 23 * SECTOR + len(little_table)] = little_table
        self.bytes[24 * SECTOR : 24 * SECTOR + len(big_table)] = big_table
        self.bytes[ARCHIVE_SECTOR * SECTOR : ARCHIVE_SECTOR * SECTOR + len(archive)] = archive
        for sector, path in ((40, "README.TXT"), (41, "MIXED0.TXT"),
                             (42, "PSP_GAME/USRDIR/EBOOT.BIN")):
            value = self.file_bytes[path]
            self.bytes[sector * SECTOR : sector * SECTOR + len(value)] = value

    def _put_directory(self, sector, entries, *, prefix):
        offset = sector * SECTOR
        for name, record in entries:
            self.record_offsets[prefix + name] = offset
            self.bytes[offset : offset + len(record)] = record
            offset += len(record)
        assert offset <= (sector + 1) * SECTOR

    def set_record_extent(self, name, sector):
        offset = self.record_offsets[name]
        self.bytes[offset + 2 : offset + 10] = both32(sector)

    def set_record_size(self, name, size):
        offset = self.record_offsets[name]
        self.bytes[offset + 10 : offset + 18] = both32(size)

    def set_record_name(self, name, replacement):
        offset = self.record_offsets[name]
        replacement = replacement.encode("ascii")
        old_length = self.bytes[offset + 32]
        assert len(replacement) == old_length
        self.bytes[offset + 33 : offset + 33 + old_length] = replacement


class ResourceFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.addCleanup(self.cleanup_fixture)
        self.image = MiniImage()
        self.iso = self.base / "synthetic.iso"
        self.output = self.base / "resources"
        self.save_image()

    def cleanup_fixture(self):
        # The tool may protect raw directories against writes; make temporary
        # fixture directories removable without changing any source outside it.
        for root, _, _ in os.walk(self.base):
            Path(root).chmod(0o700)
        self.tmp.cleanup()

    def save_image(self):
        self.iso.write_bytes(self.image.bytes)

    def prepare(self, *, chunk_size=13):
        return resources.prepare_resources(self.iso, self.output, chunk_size=chunk_size)

    def assert_invalid_image(self):
        self.save_image()
        original = self.iso.read_bytes()
        with self.assertRaises((resources.PreparationError, ValueError, OSError)):
            self.prepare()
        self.assertEqual(self.iso.read_bytes(), original)
        self.assertFalse(self.output.exists())


class CipherTests(unittest.TestCase):
    def test_arbitrary_offsets_match_independent_plaintext(self):
        plain = bytes((i * 29 + 17) % 256 for i in range(4097))
        for block in (0, 1, 0x12345678):
            cipher = encode_entry(plain, block)
            for start in (0, 1, 2, 3, 7, 31, 255, 2045, 2048, 4093):
                for length in (1, 2, 5, 17, 63):
                    with self.subTest(block=block, start=start, length=length):
                        end = min(start + length, len(plain))
                        self.assertEqual(
                            resources.decrypt_chunk(cipher[start:end], block, start),
                            plain[start:end],
                        )


class ExtractionTests(ResourceFixture):
    def test_all_bytes_and_manifest_provenance(self):
        manifest = self.prepare()
        self.assertEqual(json.loads((self.output / "manifest.json").read_text()), manifest)
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["source"]["iso"], {
            "size": len(self.image.bytes), "sha256": digest(self.image.bytes)
        })
        self.assertEqual(manifest["archive"]["offset"], ARCHIVE_SECTOR * SECTOR)
        self.assertEqual(manifest["archive"]["size"], len(self.image.archive))

        iso_files = {item["path"]: item for item in manifest["iso"]["files"]}
        self.assertEqual(set(iso_files), set(self.image.file_bytes))
        sector_for = {
            "README.TXT": 40, "MIXED0.TXT": 41,
            "PSP_GAME/USRDIR/DATA.BIN": ARCHIVE_SECTOR,
            "PSP_GAME/USRDIR/EBOOT.BIN": 42,
            "PSP_GAME/USRDIR/EMPTY.DAT": 43,
        }
        for path, expected in self.image.file_bytes.items():
            with self.subTest(iso_file=path):
                item = iso_files[path]
                self.assertEqual(item["offset"], sector_for[path] * SECTOR)
                self.assertEqual(item["size"], len(expected))
                self.assertEqual(item["sha256"], digest(expected))
                self.assertEqual(item["output_path"], "raw-disc/" + path)
                self.assertEqual((self.output / item["output_path"]).read_bytes(), expected)

        entries = manifest["entries"]
        self.assertEqual([item["id"] for item in entries], list(range(6)))
        blocks = [1, 1, 2, 3, 4, 5, 6]
        exact = {1, 2, 4, 5}
        for index, expected in enumerate(self.image.entries):
            with self.subTest(entry=index):
                item = entries[index]
                self.assertEqual(item["output_path"], f"raw-entries/{index:05d}.bin")
                self.assertEqual(item["archive_offset"], blocks[index] * SECTOR)
                self.assertEqual(item["source_offset"], (ARCHIVE_SECTOR + blocks[index]) * SECTOR)
                self.assertEqual(item["block_address"], blocks[index])
                self.assertEqual(item["stored_length"], (blocks[index + 1] - blocks[index]) * SECTOR)
                self.assertEqual(item["extracted_length"], len(expected))
                self.assertEqual(item["length_source"], "size_table" if index in exact else "block_span")
                self.assertEqual(item["transform"], "verbatim" if index in (4, 5) else "deobfuscated")
                self.assertEqual(item["sha256"], digest(expected))
                self.assertEqual((self.output / item["output_path"]).read_bytes(), expected)
        self.assertEqual(entries[0]["extracted_length"], 0)
        self.assertEqual(entries[0]["type"], "empty")
        self.assertIsNone(entries[0]["overlay"])
        for index in (1, 4, 5):
            self.assertNotIn(entries[index]["type"], (None, "unknown"))
        overlay = entries[1]["overlay"]
        self.assertEqual(overlay["id"], 1)
        self.assertEqual(overlay["name"], "test_task.ovl")
        self.assertEqual(overlay["load"], 0x0A001780)
        self.assertEqual(overlay["text"], 5)
        self.assertEqual(overlay["data"], 3)
        self.assertTrue((self.output / "derived").is_dir())
        self.assertTrue((self.output / "working").is_dir())

    def test_tiny_and_large_chunks_produce_same_raw_bytes(self):
        small = self.prepare(chunk_size=7)
        second = self.base / "large-chunk"
        large = resources.prepare_resources(self.iso, second, chunk_size=65536)
        self.assertEqual(small["entries"], large["entries"])
        self.assertEqual(small["iso"]["files"], large["iso"]["files"])
        for item in small["entries"] + small["iso"]["files"]:
            relative = item["output_path"]
            self.assertEqual((self.output / relative).read_bytes(), (second / relative).read_bytes())

    def test_verified_reuse_does_not_write_and_keeps_editable_directories(self):
        first = self.prepare()
        derived = self.output / "derived/notes.txt"
        working = self.output / "working/edits.bin"
        derived.write_text("local analysis\n")
        working.write_bytes(b"local conversion")
        raw = self.output / first["entries"][2]["output_path"]
        manifest_file = self.output / "manifest.json"
        timestamp = 1_600_000_000_000_000_000
        os.utime(raw, ns=(timestamp, timestamp))
        os.utime(manifest_file, ns=(timestamp, timestamp))
        self.assertEqual(resources.verify_workspace(self.iso, self.output), first)
        self.assertEqual(self.prepare(), first)
        self.assertEqual(raw.stat().st_mtime_ns, timestamp)
        self.assertEqual(manifest_file.stat().st_mtime_ns, timestamp)
        self.assertEqual(derived.read_text(), "local analysis\n")
        self.assertEqual(working.read_bytes(), b"local conversion")

    def test_existing_workspace_refuses_corrupt_or_unexpected_raw_files(self):
        manifest = self.prepare()
        raw = self.output / manifest["entries"][2]["output_path"]
        raw.chmod(0o644)
        raw.write_bytes(b"tampered")
        with self.assertRaises(resources.PreparationError):
            self.prepare()
        raw.write_bytes(self.image.entries[2])
        (self.output / "raw-entries").chmod(0o755)
        unexpected = self.output / "raw-entries/extra.bin"
        unexpected.write_bytes(b"not indexed")
        with self.assertRaises(resources.PreparationError):
            resources.verify_workspace(self.iso, self.output)
        unexpected.unlink()
        (self.output / "raw-disc").chmod(0o755)
        (self.output / "raw-disc/linked.bin").symlink_to(self.iso)
        with self.assertRaises(resources.PreparationError):
            resources.verify_workspace(self.iso, self.output)

    def test_reuse_rejects_coordinated_raw_and_manifest_changes(self):
        for kind, index, changed in (
            ("entry", 2, b"Jello world"),
            ("iso", "README.TXT", b"B synthetic readme\n"),
        ):
            with self.subTest(kind=kind):
                output = self.base / f"resources-{kind}"
                manifest = resources.prepare_resources(self.iso, output, chunk_size=7)
                row = (manifest["entries"][index] if kind == "entry" else
                       next(item for item in manifest["iso"]["files"] if item["path"] == index))
                raw = output / row["output_path"]
                self.assertEqual(len(changed), len(raw.read_bytes()))
                raw.chmod(0o644)
                raw.write_bytes(changed)
                row["sha256"] = digest(changed)
                manifest_path = output / "manifest.json"
                manifest_path.chmod(0o644)
                manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
                with self.assertRaisesRegex(resources.PreparationError, "differs from source"):
                    resources.verify_workspace(self.iso, output, chunk_size=7)

    def test_reuse_detects_source_change_during_verification(self):
        self.prepare()
        verify_payloads = resources._verify_source_payloads

        def change_source_after_check(*args, **kwargs):
            verify_payloads(*args, **kwargs)
            with self.iso.open("r+b") as source:
                source.seek(40 * SECTOR)
                source.write(b"X")

        with mock.patch.object(resources, "_verify_source_payloads", side_effect=change_source_after_check):
            with self.assertRaisesRegex(resources.PreparationError, "ISO changed during resource verification"):
                resources.verify_workspace(self.iso, self.output)

    def test_expected_source_identity_and_overlap(self):
        expected = digest(self.image.bytes)
        with self.assertRaises(resources.PreparationError):
            resources.prepare_resources(self.iso, self.output, expected_iso_sha256="0" * 64)
        self.assertFalse(self.output.exists())
        resources.prepare_resources(self.iso, self.output, expected_iso_sha256=expected)
        with self.assertRaises(resources.PreparationError):
            resources.verify_workspace(self.iso, self.output, expected_iso_sha256="0" * 64)
        with self.assertRaises(resources.PreparationError):
            resources.prepare_resources(self.iso, self.base)
        self.assertEqual(self.iso.read_bytes(), self.image.bytes)

    def test_command_line_verify_only(self):
        script = Path(resources.__file__)
        before = subprocess.run(
            [sys.executable, str(script), str(self.iso), "--output", str(self.output), "--verify-only"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(before.returncode, 0)
        self.assertFalse(self.output.exists())
        self.prepare()
        after = subprocess.run(
            [sys.executable, str(script), str(self.iso), "--output", str(self.output), "--verify-only"],
            capture_output=True, text=True,
        )
        self.assertEqual(after.returncode, 0, after.stderr)


class ISORejectionTests(ResourceFixture):
    def test_short_physical_image_and_file_extent(self):
        self.image.bytes = self.image.bytes[: 32 * SECTOR]
        self.assert_invalid_image()
        self.image = MiniImage()
        self.image.set_record_extent("README.TXT;1", ISO_SECTORS + 1)
        self.assert_invalid_image()

    def test_unsafe_names_and_duplicate_casefold_name(self):
        for replacement in ("../FOO.BIN;1", "AA\\BBB.TXT;1", "readme.txt;1"):
            with self.subTest(replacement=replacement):
                self.image = MiniImage()
                self.image.set_record_name("MIXED0.TXT;1", replacement)
                self.assert_invalid_image()

    def test_malformed_record_and_name_length(self):
        self.image.bytes[self.image.record_offsets["PSP_GAME/USRDIR/DATA.BIN;1"]] = 12
        self.assert_invalid_image()
        self.image = MiniImage()
        offset = self.image.record_offsets["PSP_GAME/USRDIR/DATA.BIN;1"]
        self.image.bytes[offset + 32] = 255
        self.assert_invalid_image()

    def test_mismatched_endian_fields_and_bad_sector_size(self):
        self.image.bytes[16 * SECTOR + 84 : 16 * SECTOR + 88] = struct.pack(">I", ISO_SECTORS + 1)
        self.assert_invalid_image()
        self.image = MiniImage()
        offset = self.image.record_offsets["PSP_GAME/USRDIR/DATA.BIN;1"]
        self.image.bytes[offset + 14 : offset + 18] = struct.pack(">I", len(self.image.archive) + 1)
        self.assert_invalid_image()
        self.image = MiniImage()
        self.image.bytes[16 * SECTOR + 128 : 16 * SECTOR + 132] = both16(1024)
        self.assert_invalid_image()

    def test_directory_cycle_multi_extent_and_interleaving(self):
        self.image.set_record_extent("PSP_GAME/USRDIR", 20)
        self.assert_invalid_image()
        self.image = MiniImage()
        offset = self.image.record_offsets["PSP_GAME/USRDIR/DATA.BIN;1"]
        self.image.bytes[offset + 25] |= 0x80
        self.assert_invalid_image()
        for field in (26, 27):
            self.image = MiniImage()
            offset = self.image.record_offsets["PSP_GAME/USRDIR/DATA.BIN;1"]
            self.image.bytes[offset + field] = 1
            self.assert_invalid_image()


class ArchiveRejectionTests(ResourceFixture):
    def replace_archive(self, archive):
        self.image = MiniImage(archive)
        self.save_image()

    def test_descending_block_and_missing_end_marker(self):
        for blocks in ([1, 1, 3, 2, 4, 5, 6], [1, 1, 2, 3, 4, 5, 7]):
            with self.subTest(blocks=blocks):
                archive, _ = synthetic_archive(blocks=blocks)
                self.replace_archive(archive)
                self.assert_invalid_image()

    def test_truncated_archive_extent_rejected(self):
        self.image.set_record_size("PSP_GAME/USRDIR/DATA.BIN;1", 5 * SECTOR + 1)
        self.assert_invalid_image()

    def test_failed_staged_extraction_never_publishes_partial_output(self):
        source_before = self.iso.read_bytes()
        actual_extract = resources._extract_entry

        def fail_after_writes(stream, archive, directory, index, destination, chunk_size):
            if index == 3:
                self.assertTrue((destination.parent / "00002.bin").exists())
                raise OSError("synthetic failure during staged extraction")
            return actual_extract(stream, archive, directory, index, destination, chunk_size)

        with mock.patch.object(resources, "_extract_entry", side_effect=fail_after_writes):
            with self.assertRaisesRegex(OSError, "synthetic failure"):
                self.prepare()
        self.assertEqual(self.iso.read_bytes(), source_before)
        self.assertFalse(self.output.exists())
        self.assertEqual({path.name for path in self.base.iterdir()}, {"synthetic.iso"})


if __name__ == "__main__":
    unittest.main()
