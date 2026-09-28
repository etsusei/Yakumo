#!/usr/bin/env python3
"""Synthetic shape and build-local ownership tests for read instrumentation."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "texture_read_instrumentation", TOOLS / "instrument_texture_read.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def make_fixture(unit: int) -> tuple[str, dict]:
    names = module.TEST_CHECKPOINTS[unit]
    source = [
        '#include "psprecomp/runtime.hpp"\n',
        '#include "recomp_units.hpp"\n',
        f'void recomp_unit_{unit:04d}_entry(Runtime &rt, AllegrexContext &ctx, '
        'std::uint16_t id, GuestMemory::AotFastView &view) {\n',
    ]
    addresses = [address for _, address, _ in names]
    for address in range(addresses[0] - 4, addresses[-1] + 8, 4):
        source.append(f"L_{address:08X}:\n")
        checkpoint = next((row for row in names if row[1] == address), None)
        if checkpoint:
            source.append(checkpoint[2])
        else:
            source.append("    ctx.gpr[2] = 0u;\n")
        source.append(f"    goto L_{address + 4:08X};\n")
    source.extend([
        "}\n",
        f"void register_fixture_{unit}(Runtime &runtime) {{\n",
        f"    runtime.register_generated_unit({unit}u, 0x08860000u, 16384u, "
        f"&recomp_unit_{unit:04d}, &recomp_unit_{unit:04d}_entry);\n",
    ])
    for _, address, _ in names:
        source.append(f"    runtime.register_function(0x{address:08X}u, "
                      f"&recomp_unit_{unit:04d}, 0u);\n")
    source.append("}\n")
    code = "".join(source)
    labels = list(module._LABEL.finditer(code))
    specs = []
    for name, address, _body in names:
        indexes = [i for i, label in enumerate(labels)
                   if int(label.group(1), 16) == address]
        if len(indexes) != 1:
            raise AssertionError(f"synthetic label count at {address:08X}")
        index = indexes[0]
        start = module._next_line(code, labels[index])
        end = labels[index + 1].start()
        digest = hashlib.sha256(re.sub(r"\s+", "", code[start:end]).encode()).hexdigest()
        specs.append((name, address, digest))
    return code, {unit: tuple(specs)}


class TextureReadInstrumentationTests(unittest.TestCase):
    @staticmethod
    def publish(source: str, specs: dict, output: Path, manifest: Path):
        transformed, metadata = module.instrument_source(source, specs)
        output_bytes = transformed.encode("utf-8")
        source_bytes = source.encode("utf-8")
        metadata["source_sha256"] = hashlib.sha256(source_bytes).hexdigest()
        metadata["output_sha256"] = hashlib.sha256(output_bytes).hexdigest()
        manifest_bytes = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
        module.publish_pair(output, output_bytes, manifest, manifest_bytes, specs)
        return output_bytes, manifest_bytes, metadata

    def test_instruments_only_audited_build_local_read_edges(self):
        for unit in (23, 24):
            with self.subTest(unit=unit):
                source, specs = make_fixture(unit)
                result, metadata = module.instrument_source(source, specs)
                self.assertEqual(result.count(module.READ_CALLBACK), len(specs[unit]))
                self.assertEqual(metadata["unit"], f"generated_unit_{unit:04d}")
                self.assertEqual(metadata["checkpoint_calls"], len(specs[unit]))
                with self.assertRaises(module.TextureReadInstrumentationError):
                    module.instrument_source(result, specs)

    def test_only_result_edge_contains_oracle_prefix_exit(self):
        source, specs = make_fixture(24)
        result, manifest = module.instrument_source(source, specs)
        self.assertEqual(result.count(module.STOP_CALLBACK), 1)
        self.assertIn("ctx.pc = 0x08001000u;\n        return;", result)
        address = next(addr for name, addr, _ in module.TEST_CHECKPOINTS[24]
                       if name == "ReadResult")
        label = f"L_{address:08X}:\n"
        start = result.index(label) + len(label)
        self.assertTrue(result[start:].startswith(
            f"    {module.READ_CALLBACK}(rt, ctx, "
            "mhp3rd::native::TextureReadCheckpoint::ReadResult);"))
        self.assertEqual(manifest["oracle_stop_after_read_result"], True)

        source23, specs23 = make_fixture(23)
        result23, manifest23 = module.instrument_source(source23, specs23)
        self.assertNotIn(module.STOP_CALLBACK, result23)
        self.assertFalse(manifest23["oracle_stop_after_read_result"])

    def test_wrong_block_and_registration_fingerprints_are_rejected(self):
        source, specs = make_fixture(23)
        changed = source.replace(
            "L_08863608:\n    ctx.gpr[29] = ctx.gpr[29] - 16u;",
            "L_08863608:\n    ctx.gpr[29] = ctx.gpr[29] - 15u;", 1)
        with self.assertRaises(module.TextureReadInstrumentationError):
            module.instrument_source(changed, specs)
        bad_registration = source.replace("register_generated_unit(23u", "register_generated_unit(24u", 1)
        with self.assertRaises(module.TextureReadInstrumentationError):
            module.instrument_source(bad_registration, specs)

    def test_owned_pair_refreshes_after_changed_input(self):
        source, specs = make_fixture(23)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output, manifest = root / "unit.cpp", root / "unit.json"
            first_output, first_manifest, first = self.publish(source, specs, output, manifest)
            changed = source.replace("ctx.gpr[2] = 0u;", "ctx.gpr[2] = 7u;", 1)
            second_output, second_manifest, second = self.publish(changed, specs, output, manifest)
            self.assertNotEqual(first["source_sha256"], second["source_sha256"])
            self.assertNotEqual(first_output, second_output)
            self.assertNotEqual(first_manifest, second_manifest)
            self.assertEqual(output.read_bytes(), second_output)
            self.assertEqual(manifest.read_bytes(), second_manifest)
            self.publish(changed, specs, output, manifest)
            self.assertEqual(output.read_bytes(), second_output)

    def test_tampered_or_incomplete_pair_is_preserved_and_rejected(self):
        source, specs = make_fixture(24)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output, manifest = root / "unit.cpp", root / "unit.json"
            first_output, first_manifest, _ = self.publish(source, specs, output, manifest)
            output.write_bytes(first_output + b"// tampered\n")
            tampered = output.read_bytes()
            with self.assertRaisesRegex(module.TextureReadInstrumentationError, "tampered"):
                self.publish(source.replace("ctx.gpr[2] = 0u;", "ctx.gpr[2] = 5u;", 1),
                             specs, output, manifest)
            self.assertEqual(output.read_bytes(), tampered)
            self.assertEqual(manifest.read_bytes(), first_manifest)
            output.write_bytes(first_output)
            manifest.unlink()
            with self.assertRaisesRegex(module.TextureReadInstrumentationError, "incomplete"):
                self.publish(source, specs, output, manifest)
            self.assertEqual(output.read_bytes(), first_output)
            self.assertFalse(manifest.exists())

    def test_read_result_oracle_header_is_private_to_unit_0024(self):
        source, specs = make_fixture(24)
        result, manifest = module.instrument_source(source, specs)
        self.assertIn(module.STOP_INCLUDE, result)
        self.assertTrue(manifest["stop_include_added"])
        source23, specs23 = make_fixture(23)
        result23, manifest23 = module.instrument_source(source23, specs23)
        self.assertNotIn(module.STOP_INCLUDE, result23)
        self.assertFalse(manifest23["stop_include_added"])


if __name__ == "__main__":
    unittest.main()
