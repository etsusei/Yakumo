#!/usr/bin/env python3
"""Synthetic shape and failure tests for build-local transfer instrumentation."""
from __future__ import annotations

import hashlib
import importlib.util
import re
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "transfer_instrumentation", TOOLS / "instrument_texture_transfer.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def make_fixture(unit: int) -> tuple[str, dict]:
    labels = module.TEST_CHECKPOINTS[unit]
    # Independent placeholder instructions exercise structure only; no game
    # instruction text or generated game files are retained by this fixture.
    pieces = ['#include "psprecomp/runtime.hpp"\n',
              '#include "recomp_units.hpp"\n',
              f'void recomp_unit_{unit:04d}_entry(Runtime &rt, AllegrexContext &ctx, std::uint16_t id, GuestMemory::AotFastView &view) {{\n']
    start = labels[0][1] - 4
    for address in range(start, labels[-1][1] + 8, 4):
        pieces.append(f"L_{address:08X}:\n")
        match = next((item for item in labels if item[1] == address), None)
        if match:
            pieces.extend(match[3])
        else:
            pieces.append("    ctx.gpr[2] = 1u;\n")
        pieces.append(f"    goto L_{address + 4:08X};\n")
    pieces.extend([
        "}\n",
        f"void register_fixture_{unit}(Runtime &runtime) {{\n"
        f"    runtime.register_generated_unit({unit}u, 0x08860000u, 16384u, "
        f"&recomp_unit_{unit:04d}, &recomp_unit_{unit:04d}_entry);\n",
    ])
    for name, address, _, _ in labels:
        pieces.append(f"    runtime.register_function(0x{address:08X}u, &recomp_unit_{unit:04d}, 0u);\n")
    pieces.append("}\n")
    code = "".join(pieces)
    source_labels = list(module._LABEL.finditer(code))
    specs_list = []
    for name, address, _, _block in labels:
        indexes = [i for i, match in enumerate(source_labels)
                   if int(match.group(1), 16) == address]
        if len(indexes) != 1:
            raise AssertionError(f"synthetic label count for {address:08X}: {len(indexes)}")
        i = indexes[0]
        block = code[module._next_line(code, source_labels[i]):source_labels[i + 1].start()]
        specs_list.append((name, address, hashlib.sha256(
            re.sub(r"\s+", "", block).encode("utf-8")).hexdigest()))
    specs = tuple(specs_list)
    return code, {unit: specs}


class TransferInstrumentationTests(unittest.TestCase):
    @staticmethod
    def publish_fixture(source: str, specs: dict, output_path: Path,
                        manifest_path: Path) -> tuple[bytes, bytes, dict]:
        transformed, metadata = module.instrument_source(source, specs)
        output = transformed.encode("utf-8")
        original = source.encode("utf-8")
        metadata["source_sha256"] = hashlib.sha256(original).hexdigest()
        metadata["output_sha256"] = hashlib.sha256(output).hexdigest()
        manifest = (module.json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8")
        module.publish_instrumented_pair(output_path, output, manifest_path, manifest,
                                         specs[int(metadata["unit"][-4:])])
        return output, manifest, metadata

    def test_instruments_only_named_build_local_boundaries(self):
        for unit in (23, 40):
            with self.subTest(unit=unit):
                source, specs = make_fixture(unit)
                result, manifest = module.instrument_source(source, specs)
                callbacks = re.findall(r"texture_transfer_checkpoint\(rt, ctx,", result)
                self.assertEqual(len(callbacks), len(specs[unit]))
                self.assertEqual(manifest["unit"], f"generated_unit_{unit:04d}")
                self.assertEqual(manifest["checkpoint_calls"], len(specs[unit]))
                for name, address, _, _ in module.TEST_CHECKPOINTS[unit]:
                    marker = f"L_{address:08X}:\n    mhp3rd::native::texture_transfer_checkpoint"
                    self.assertIn(marker, result)
                # Each source already needs the lifetime/dispatcher API, so no
                # duplicate include may be introduced.
                self.assertEqual(result.count(module._INCLUDE), 1)
                with self.assertRaises(module.TextureTransferInstrumentationError):
                    module.instrument_source(result, specs)

    def test_refuses_wrong_block_hash_or_checkpoint_layout(self):
        source, specs = make_fixture(23)
        altered = source.replace("L_08863CDC:\n    ctx.gpr[2] = 1u;",
                                 "L_08863CDC:\n    ctx.gpr[2] = 2u;", 1)
        with self.assertRaises(module.TextureTransferInstrumentationError):
            module.instrument_source(altered, specs)
        wrong = source.replace("L_08863DD8:", "L_08863DDC:", 1)
        with self.assertRaises(module.TextureTransferInstrumentationError):
            module.instrument_source(wrong, specs)

    def test_requires_single_registered_unit_function(self):
        source, specs = make_fixture(40)
        bad = source.replace("runtime.register_generated_unit(40u", "runtime.register_generated_unit(41u", 1)
        with self.assertRaises(module.TextureTransferInstrumentationError):
            module.instrument_source(bad, specs)

    def test_synthetic_shapes_are_not_the_production_fingerprints(self):
        for unit in (23, 40):
            source, synthetic = make_fixture(unit)
            self.assertNotEqual(synthetic[unit], module.CHECKPOINTS[unit])
            with self.assertRaises(module.TextureTransferInstrumentationError):
                module.instrument_source(source)

    def test_owned_output_pair_refreshes_for_changed_input(self):
        source, specs = make_fixture(23)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "generated_unit_0023.cpp"
            manifest = root / "generated_unit_0023.cpp.json"
            first_output, first_manifest, first = self.publish_fixture(
                source, specs, output, manifest)
            changed = source.replace('#include "recomp_units.hpp"\n',
                '#include "recomp_units.hpp"\n// refreshed build-local source\n', 1)
            second_output, second_manifest, second = self.publish_fixture(
                changed, specs, output, manifest)
            self.assertNotEqual(first["source_sha256"], second["source_sha256"])
            self.assertNotEqual(first_output, second_output)
            self.assertNotEqual(first_manifest, second_manifest)
            self.assertEqual(output.read_bytes(), second_output)
            self.assertEqual(manifest.read_bytes(), second_manifest)
            # Re-running an unchanged input is idempotent.
            self.publish_fixture(changed, specs, output, manifest)
            self.assertEqual(output.read_bytes(), second_output)

    def test_tampered_or_incomplete_owned_pair_is_preserved_and_rejected(self):
        source, specs = make_fixture(40)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "generated_unit_0040.cpp"
            manifest = root / "generated_unit_0040.cpp.json"
            self.publish_fixture(source, specs, output, manifest)
            original_manifest = manifest.read_bytes()
            output.write_bytes(output.read_bytes() + b"// tampered\n")
            tampered_output = output.read_bytes()
            changed = source.replace('#include "recomp_units.hpp"\n',
                '#include "recomp_units.hpp"\n// new input\n', 1)
            next_output, metadata = module.instrument_source(changed, specs)
            next_bytes = next_output.encode("utf-8")
            metadata["source_sha256"] = hashlib.sha256(changed.encode("utf-8")).hexdigest()
            metadata["output_sha256"] = hashlib.sha256(next_bytes).hexdigest()
            next_manifest = (module.json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
            with self.assertRaisesRegex(module.TextureTransferInstrumentationError, "tampered"):
                module.publish_instrumented_pair(output, next_bytes, manifest, next_manifest,
                                                 specs[40])
            self.assertEqual(output.read_bytes(), tampered_output)
            self.assertEqual(manifest.read_bytes(), original_manifest)

            output.write_bytes(next_bytes)
            manifest.unlink()
            with self.assertRaisesRegex(module.TextureTransferInstrumentationError, "incomplete"):
                module.publish_instrumented_pair(output, next_bytes, manifest, next_manifest,
                                                 specs[40])
            self.assertEqual(output.read_bytes(), next_bytes)
            self.assertFalse(manifest.exists())

    def test_second_temp_creation_failure_removes_first_temp(self):
        source, specs = make_fixture(23)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "generated_unit_0023.cpp"
            manifest = root / "generated_unit_0023.cpp.json"
            transformed, metadata = module.instrument_source(source, specs)
            output_bytes = transformed.encode("utf-8")
            manifest_bytes = (module.json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
            real_write_temp = module._write_temp
            temporary_paths = []

            def fail_second_temp(path, data):
                if temporary_paths:
                    raise OSError("injected second temporary-file creation failure")
                created = real_write_temp(path, data)
                temporary_paths.append(Path(created))
                return created

            with mock.patch.object(module, "_write_temp", side_effect=fail_second_temp):
                with self.assertRaisesRegex(OSError, "second temporary-file"):
                    module._atomic_write_pair(output, output_bytes, manifest, manifest_bytes)
            self.assertEqual(len(temporary_paths), 1)
            self.assertFalse(temporary_paths[0].exists())
            self.assertFalse(output.exists())
            self.assertFalse(manifest.exists())

    def test_backup_move_failure_preserves_both_original_files(self):
        source, specs = make_fixture(23)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "generated_unit_0023.cpp"
            manifest = root / "generated_unit_0023.cpp.json"
            old_output, old_manifest, _ = self.publish_fixture(source, specs, output, manifest)
            new_output = old_output + b"// next input\n"
            new_manifest = old_manifest + b" "
            real_replace = module.os.replace

            def fail_output_backup(src, dst):
                if Path(src) == output and Path(dst).suffix == ".bak":
                    raise OSError("injected backup move failure")
                return real_replace(src, dst)

            with mock.patch.object(module.os, "replace", side_effect=fail_output_backup):
                with self.assertRaisesRegex(OSError, "backup move failure"):
                    module._atomic_write_pair(output, new_output, manifest, new_manifest)
            self.assertEqual(output.read_bytes(), old_output)
            self.assertEqual(manifest.read_bytes(), old_manifest)
            self.assertEqual(list(root.glob(".*.bak")), [])
            self.assertEqual(list(root.glob(".*.tmp")), [])

    def test_manifest_install_failure_rolls_back_complete_old_pair(self):
        source, specs = make_fixture(23)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "generated_unit_0023.cpp"
            manifest = root / "generated_unit_0023.cpp.json"
            old_output, old_manifest, _ = self.publish_fixture(source, specs, output, manifest)
            real_replace = module.os.replace
            failed = False

            def fail_manifest_install(src, dst):
                nonlocal failed
                if Path(src).suffix == ".tmp" and Path(dst) == manifest and not failed:
                    failed = True
                    raise OSError("injected new manifest install failure")
                return real_replace(src, dst)

            with mock.patch.object(module.os, "replace", side_effect=fail_manifest_install):
                with self.assertRaisesRegex(OSError, "new manifest install failure"):
                    module._atomic_write_pair(output, old_output + b"// new\n",
                                              manifest, old_manifest + b" ")
            self.assertTrue(failed)
            self.assertEqual(output.read_bytes(), old_output)
            self.assertEqual(manifest.read_bytes(), old_manifest)
            self.assertEqual(list(root.glob(".*.bak")), [])
            self.assertEqual(list(root.glob(".*.tmp")), [])

    def test_restore_failure_preserves_backup_and_continues_other_restore(self):
        source, specs = make_fixture(23)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "generated_unit_0023.cpp"
            manifest = root / "generated_unit_0023.cpp.json"
            old_output, old_manifest, _ = self.publish_fixture(source, specs, output, manifest)
            real_replace = module.os.replace
            real_unlink = module.os.unlink
            failed = {"install": False, "unlink": False, "restore": False}

            def fail_install_and_output_restore(src, dst):
                src_path, dst_path = Path(src), Path(dst)
                if src_path.suffix == ".tmp" and dst_path == manifest and not failed["install"]:
                    failed["install"] = True
                    raise OSError("injected new manifest install failure")
                if src_path.suffix == ".bak" and dst_path == output and not failed["restore"]:
                    failed["restore"] = True
                    raise OSError("injected old output restore failure")
                return real_replace(src, dst)

            def fail_output_unlink(path, *args, **kwargs):
                if Path(path) == output and not failed["unlink"]:
                    failed["unlink"] = True
                    raise OSError("injected rollback output unlink failure")
                return real_unlink(path, *args, **kwargs)

            with mock.patch.object(module.os, "replace", side_effect=fail_install_and_output_restore):
                with mock.patch.object(module.os, "unlink", side_effect=fail_output_unlink):
                    with self.assertRaisesRegex(OSError, "recovery location") as raised:
                        module._atomic_write_pair(output, old_output + b"// new\n",
                                                  manifest, old_manifest + b" ")
            self.assertEqual(failed, {"install": True, "unlink": True, "restore": True})
            self.assertIn(str(output), str(raised.exception))
            self.assertEqual(manifest.read_bytes(), old_manifest)
            retained = list(root.glob(".*.bak"))
            self.assertEqual(len(retained), 1)
            self.assertEqual(retained[0].read_bytes(), old_output)
            self.assertEqual(list(root.glob(".*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
