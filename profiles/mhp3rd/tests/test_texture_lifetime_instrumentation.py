#!/usr/bin/env python3
"""Asset-free tests for the build-local lifetime checkpoint injector."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from unittest import mock
from pathlib import Path


TOOL = Path(__file__).resolve().parents[1] / "tools" / "instrument_texture_lifetime.py"
SPEC = importlib.util.spec_from_file_location("instrument_texture_lifetime", TOOL)
assert SPEC is not None and SPEC.loader is not None
lifetime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lifetime)


EXPECTED = {
    29: (("HeapInit", 0x08879DA4), ("HeapReset", 0x08879D58),
         ("ForwardAllocate", 0x08879DB4), ("ReverseAllocate", 0x08879F08),
         ("Free", 0x08879FF0)),
    40: (("OwnerReset", 0x088A53C8),),
    43: (("CallerTail", 0x088B0398), ("ProviderResult", 0x088B03B0),
         ("ChildResult", 0x088B03BC), ("CommandAllocationResult", 0x088B03E4),
         ("BuilderCall", 0x088B03F8)),
    46: (("FactoryAllocationResult", 0x088BD07C),
         ("FactoryConstructorCall", 0x088BD13C),
         ("FactoryConstructorResult", 0x088BD144)),
}
CALL_EDGES = {
    0x088B03B0: 0x088B03A8,
    0x088B03BC: 0x088B03B4,
    0x088B03E4: 0x088B03DC,
    0x088BD07C: 0x088BD074,
}


def synthetic_block(address: int) -> str:
    """A fixture-only block with no original game instruction content."""
    is_call = address in CALL_EDGES.values() or address in (0x088B03F8, 0x088BD13C)
    statement = "fixture_call(rt, ctx);" if is_call else "fixture_counter += 1;"
    return f"{statement}\n    goto L_{address + 4:08X};"


def synthetic_hash(block: str) -> str:
    return hashlib.sha256("".join(block.split()).encode("utf-8")).hexdigest()


def fixture(unit: int, newline: str = "\n", *, probes: bool = False) -> str:
    checkpoints = EXPECTED[unit]
    labels = sorted(
        {address + delta for _, address in checkpoints for delta in (-4, 0, 4)}
        | {CALL_EDGES[address] for _, address in checkpoints if address in CALL_EDGES}
    )
    base = 0x08878000 + (unit - 29) * 0x4000
    lines = [
        '#include "psprecomp/runtime.hpp"',
        '#include "testing/probes.hpp"' if probes else '#include "recomp_units.hpp"',
        "namespace psprecomp {",
        f"void recomp_unit_{unit:04d}_entry(Runtime &rt, AllegrexContext &ctx,",
        "        std::uint16_t direct_entry_id, GuestMemory::AotFastView &aot_mem) {",
        "    int fixture_counter = 0;",
        "    // Synthetic dispatch body.",
    ]
    anchors = {address for _, address in checkpoints} | set(CALL_EDGES.values())
    for index, address in enumerate(labels):
        lines.append(f"L_{address:08X}:")
        if address in anchors:
            lines.extend("    " + line for line in synthetic_block(address).splitlines())
        elif index + 1 < len(labels):
            lines.extend(("    // synthetic filler", f"    goto L_{labels[index + 1]:08X};"))
        else:
            lines.append("    return;")
        if probes and address == labels[0]:
            lines.append("    mhp3rd::testing::native_probe_aot_enter(rt, ctx, 0x08879D08u);")
    lines.extend((
        "}",
        f"void recomp_unit_{unit:04d}(Runtime &rt, AllegrexContext &ctx) {{}}",
        "void register_functions(Runtime &runtime) {",
        f"    runtime.register_generated_unit({unit}u, 0x{base:08X}u, 16384u, "
        f"&recomp_unit_{unit:04d}, &recomp_unit_{unit:04d}_entry);",
        "}",
        "}",
    ))
    return newline.join(lines) + newline


class SyntheticFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        # Check the production routing inventory before replacing only its
        # fingerprints for the synthetic source used in these tests.
        self.assertEqual(sum(map(len, EXPECTED.values())), 14)
        self.assertEqual(
            {unit: tuple((name, address) for name, address, _ in checkpoints)
             for unit, checkpoints in lifetime.CHECKPOINTS.items()},
            EXPECTED,
        )
        self.assertEqual(
            {address: call for address, (call, _) in lifetime.CALL_PREDECESSORS.items()},
            CALL_EDGES,
        )
        synthetic_checkpoints = {
            unit: tuple((name, address, synthetic_hash(synthetic_block(address)))
                        for name, address in checkpoints)
            for unit, checkpoints in EXPECTED.items()
        }
        synthetic_calls = {
            address: (call, synthetic_hash(synthetic_block(call)))
            for address, call in CALL_EDGES.items()
        }
        for unit, checkpoints in lifetime.CHECKPOINTS.items():
            for (_, _, production_hash), (_, _, fixture_hash) in zip(
                checkpoints, synthetic_checkpoints[unit]
            ):
                self.assertNotEqual(production_hash, fixture_hash)
        for target, (_, production_hash) in lifetime.CALL_PREDECESSORS.items():
            self.assertNotEqual(production_hash, synthetic_calls[target][1])
        for name, replacement in (
            ("CHECKPOINTS", synthetic_checkpoints),
            ("CALL_PREDECESSORS", synthetic_calls),
        ):
            patcher = mock.patch.object(lifetime, name, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)


class SourceTests(SyntheticFixtureTests):
    def test_all_checkpoints_are_before_original_blocks(self) -> None:
        self.assertEqual(set(lifetime.CHECKPOINTS), set(EXPECTED))
        for unit, checkpoints in EXPECTED.items():
            with self.subTest(unit=unit):
                source = fixture(unit)
                output, manifest = lifetime.instrument_source(source)
                self.assertEqual(manifest["schema"], lifetime.SCHEMA)
                self.assertEqual(manifest["unit"], f"generated_unit_{unit:04d}")
                self.assertEqual(manifest["checkpoint_calls"], len(checkpoints))
                self.assertEqual(manifest["checkpoints"], [
                    {"name": name, "address": f"0x{address:08X}"}
                    for name, address in checkpoints
                ])
                self.assertEqual(output.count(lifetime._INCLUDE), 1)
                for name, address in checkpoints:
                    expected = (
                        f"L_{address:08X}:\n"
                        "    mhp3rd::native::texture_lifetime_checkpoint(rt, ctx, "
                        f"mhp3rd::native::TextureLifetimeCheckpoint::{name});\n"
                        f"    {synthetic_block(address).splitlines()[0]}"
                    )
                    self.assertIn(expected, output)
                    self.assertEqual(output.count(f"TextureLifetimeCheckpoint::{name}"), 1)
                self.assertEqual(output.count("texture_lifetime_checkpoint("), len(checkpoints))
                self.assertNotIn("rt.stopped()", output)
                self.assertNotIn("local_pc = ctx.pc;", output)
                with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
                    lifetime.instrument_source(output)

    def test_crlf_and_existing_probe_hooks_are_preserved(self) -> None:
        source = fixture(29, "\r\n", probes=True)
        output, _ = lifetime.instrument_source(source)
        self.assertEqual(output.count('#include "testing/probes.hpp"'), 1)
        self.assertEqual(output.count("native_probe_aot_enter"), 1)
        self.assertIn("TextureLifetimeCheckpoint::HeapReset);\r\n", output)
        self.assertNotIn("\n", output.replace("\r\n", ""))

    def test_rejects_changed_unit_registration_label_or_block(self) -> None:
        source = fixture(43)
        mutations = (
            source.replace("recomp_unit_0043_entry", "recomp_unit_0042_entry", 1),
            source.replace("register_generated_unit(43u", "register_generated_unit(42u"),
            source.replace("L_088B03B0:", "L_088B03B1:"),
            source.replace("L_088B03B0:", "L_088B03B0:\nL_088B03B0:"),
            source.replace("L_088B03B4:", "L_088B03B8:"),
            source.replace("L_088B03B0:\n    fixture_counter += 1;",
                           "L_088B03B0:\n    fixture_counter += 9;"),
            source.replace("L_088B03A8:\n    fixture_call(rt, ctx);",
                           "L_088B03A8:\n    fixture_counter += 9;"),
            source.replace("AllegrexContext &ctx", "AllegrexContext &other"),
            source.replace('#include "psprecomp/runtime.hpp"', "// missing runtime include"),
            source + "\nvoid recomp_unit_0043_entry(Runtime &rt, AllegrexContext &ctx, int x) {}",
        )
        for changed in mutations:
            with self.subTest(change=changed[:60]):
                with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
                    lifetime.instrument_source(changed)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_source(fixture(40).replace(
                '#include "psprecomp/runtime.hpp"', lifetime._INCLUDE
            ))


class FileTests(SyntheticFixtureTests):
    def setUp(self) -> None:
        super().setUp()
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        # The filename is intentionally unrelated: unit inference is from C++.
        self.source = root / "intermediate.cpp"
        self.output = root / "build" / "texture.cpp"
        self.manifest = root / "build" / "texture.json"
        self.source.write_text(fixture(46))

    def test_hashes_idempotence_and_owned_replacement(self) -> None:
        original = self.source.read_bytes()
        manifest = lifetime.instrument_file(self.source, self.output, self.manifest)
        self.assertEqual(original, self.source.read_bytes())
        self.assertEqual(manifest["source_sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(manifest["output_sha256"], hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(json.loads(self.manifest.read_text()), manifest)
        self.assertEqual(lifetime.instrument_file(self.source, self.output, self.manifest), manifest)
        self.source.write_text(fixture(46).replace("// Synthetic dispatch body.",
                                                 "// Another synthetic body."))
        changed = lifetime.instrument_file(self.source, self.output, self.manifest)
        self.assertNotEqual(changed["source_sha256"], manifest["source_sha256"])

    def test_rejects_aliases_symlinks_and_unrelated_files(self) -> None:
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.source)
        source_alias = self.source.parent / "source-alias.cpp"
        os.link(self.source, source_alias)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, source_alias)
        source_link = self.source.parent / "source-link.cpp"
        source_link.symlink_to(self.source)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(source_link, self.output)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output, self.source)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output, self.output)

        self.output.parent.mkdir()
        self.output.write_text("unrelated output")
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output)
        self.output.unlink()
        self.output.symlink_to(self.source)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output)
        self.output.unlink()
        self.manifest.write_text('{"schema":"unrelated"}')
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output, self.manifest)
        self.manifest.write_text(
            '{"schema":"mhp3rd-texture-lifetime-instrumentation-v1",'
            '"unit":"generated_unit_0046"}'
        )
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output, self.manifest)
        self.manifest.unlink()
        self.manifest.symlink_to(self.source)
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output, self.manifest)

    def test_rejects_nonregular_and_invalid_utf8(self) -> None:
        self.source.write_bytes(b"\xff")
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output)
        self.source.write_text(fixture(46))
        self.output.parent.mkdir()
        self.output.mkdir()
        with self.assertRaises(lifetime.TextureLifetimeInstrumentationError):
            lifetime.instrument_file(self.source, self.output)


if __name__ == "__main__":
    unittest.main()
