"""Synthetic, offline checks for certified AOT probe source instrumentation."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from profiles.mhp3rd.tools import instrument_probes as probes  # noqa: E402


ANGLE = 0x088775AC
VECTOR = 0x08877818
SCALE = 0x08878B28
TRANSLATION = 0x08878B4C
COPY = 0x08879D08


def synthetic_source(entries, runtime_name="rt"):
    """Make label and transfer shapes without including any game instruction."""
    addresses = set()
    returns = {}
    for entry in entries:
        leaf = probes.LEAVES[entry]
        addresses.update(range(entry, entry + leaf.size + 4, 4))
        for offset in leaf.returns:
            returns[entry + offset] = leaf.store_delay_slot
    lines = [
        '#include "psprecomp/runtime.hpp"',
        '#include "recomp_units.hpp"',
        "namespace psprecomp {",
        f"void recomp_unit_0028_entry(Runtime &{runtime_name}, AllegrexContext &ctx, ",
        "                            std::uint16_t direct_entry_id, GuestMemory::AotFastView &aot_mem) {",
        "LOCAL_DISPATCH:",
        f"    if ({runtime_name}.invoke_chained_direct<&synthetic_entry, 7u, 1u, 0x1000u>(ctx, &aot_mem)) goto L_12345678;",
    ]
    for address in sorted(addresses):
        lines.append(f"L_{address:08X}:")
        if address in returns:
            lines.append("    jump_target = ctx.gpr[31];")
            lines.append("    aot_mem.aot_store32(0u, 0u);" if returns[address] else "    // nop")
            lines.extend([
                "    local_pc = jump_target;",
                "    if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; }",
                "    ctx.pc = jump_target;",
                "    return;",
            ])
        else:
            lines.append("    // Synthetic work only.")
    lines.extend(["}", "} // namespace psprecomp", ""])
    return "\n".join(lines)


def remove_probe_lines(source):
    return "\n".join(
        line for line in source.split("\n")
        if line != '#include "testing/probes.hpp"'
        and "mhp3rd::testing::native_probe_aot_" not in line
    )


class SourceInstrumentationTests(unittest.TestCase):
    def test_angle_two_exits_and_vector_preserve_every_original_line(self):
        source = synthetic_source((ANGLE, VECTOR))
        output, manifest = probes.instrument_source(source, (ANGLE, VECTOR))
        self.assertEqual(remove_probe_lines(output), source)
        self.assertEqual(output.count("native_probe_aot_enter"), 2)
        self.assertEqual(output.count("native_probe_aot_exit"), 3)
        self.assertEqual(manifest["entry_calls"], 2)
        self.assertEqual(manifest["exit_calls"], 3)
        self.assertIn("native_probe_aot_enter(rt, ctx, 0x088775ACu);", output)
        self.assertIn("native_probe_aot_enter(rt, ctx, 0x08877818u);", output)
        self.assertEqual(output.count("native_probe_aot_exit(rt, ctx, 0x088775ACu, jump_target);"), 2)
        for address in (ANGLE + 0x48, ANGLE + 0x5C, VECTOR + 0x10):
            block = output.split(f"L_{address:08X}:\n", 1)[1].split("\nL_", 1)[0]
            self.assertLess(block.index("// nop"), block.index("native_probe_aot_exit"))
            self.assertLess(block.index("native_probe_aot_exit"), block.index("local_pc = jump_target;"))
        self.assertEqual(output.count("invoke_chained_direct<&synthetic_entry"), 1)

    def test_store_delay_slot_precedes_exit_in_both_matrix_leaves(self):
        source = synthetic_source((SCALE, TRANSLATION, COPY), runtime_name="runtime")
        output, manifest = probes.instrument_source(source, (SCALE, TRANSLATION, COPY))
        self.assertEqual(remove_probe_lines(output), source)
        self.assertEqual(manifest["exit_calls"], 3)
        for address, entry in ((SCALE + 0x1C, SCALE), (TRANSLATION + 0x1C, TRANSLATION)):
            block = output.split(f"L_{address:08X}:\n", 1)[1].split("\nL_", 1)[0]
            self.assertLess(block.index("aot_mem.aot_store32"), block.index("native_probe_aot_exit"))
            self.assertLess(block.index("native_probe_aot_exit"), block.index("local_pc = jump_target;"))
            self.assertIn(f"native_probe_aot_exit(runtime, ctx, 0x{entry:08X}u, jump_target);", block)

    def test_invalid_entry_requests(self):
        for value in ("", "0x088775AC,", "0x088775AC,0x088775AC", "0xDEADBEEF", "088775AC"):
            with self.subTest(value=value), self.assertRaises(probes.ProbeInstrumentationError):
                probes.parse_entries(value)
        self.assertEqual(probes.parse_entries("0x088775ac,0X08877818"), (ANGLE, VECTOR))
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_source(synthetic_source((ANGLE,)), (ANGLE, ANGLE))

    def test_missing_duplicate_and_altered_anchors_fail_closed(self):
        source = synthetic_source((ANGLE,))
        mutations = (
            source.replace("L_088775AC:\n", "", 1),
            source + "L_088775AC:\n",
            source.replace("L_088775F4:\n", "", 1),
            source + "L_088775F4:\n",
            source.replace("L_08877610:\n", "", 1),
            source.replace("jump_target = ctx.gpr[31];", "jump_target = ctx.gpr[30];", 1),
            source.replace("    // nop\n    local_pc = jump_target;", "    local_pc = jump_target;\n    // nop", 1),
            source.replace("    // Synthetic work only.", "    return;", 1),
            source.replace("    // nop\n    local_pc = jump_target;", "    aot_mem.aot_store32(0u, 0u);\n    local_pc = jump_target;", 1),
        )
        for changed in mutations:
            with self.subTest(changed=hashlib.sha256(changed.encode()).hexdigest()[:8]):
                with self.assertRaises(probes.ProbeInstrumentationError):
                    probes.instrument_source(changed, (ANGLE,))
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_source(source, (VECTOR,))
        output, _ = probes.instrument_source(source, (ANGLE,))
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_source(output, (ANGLE,))

    def test_include_and_context_must_match_generated_shape(self):
        source = synthetic_source((ANGLE,))
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_source(source.replace('#include "psprecomp/runtime.hpp"', "// no runtime include"), (ANGLE,))
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_source(source.replace("AllegrexContext &ctx", "AllegrexContext &other"), (ANGLE,))


class FileInstrumentationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.source = self.directory / "generated_unit.cpp"
        self.output = self.directory / "build" / "generated_unit.cpp"
        self.manifest = self.directory / "build" / "probe_manifest.json"
        self.source.write_text(synthetic_source((ANGLE, VECTOR)), encoding="utf-8")

    def test_atomic_generation_manifest_and_identical_output_mtime(self):
        original = self.source.read_bytes()
        result = probes.instrument_file(self.source, self.output, (ANGLE, VECTOR), self.manifest)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(result["source_sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(result["output_sha256"], hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(json.loads(self.manifest.read_text()), result)
        self.assertFalse(list(self.output.parent.glob("*.tmp")))
        old_time = 1_700_000_000_000_000_000
        os.utime(self.output, ns=(old_time, old_time))
        os.utime(self.manifest, ns=(old_time, old_time))
        again = probes.instrument_file(self.source, self.output, (ANGLE, VECTOR), self.manifest)
        self.assertEqual(again, result)
        self.assertEqual(self.output.stat().st_mtime_ns, old_time)
        self.assertEqual(self.manifest.stat().st_mtime_ns, old_time)
        self.source.write_text(self.source.read_text() + "// Synthetic revision.\n", encoding="utf-8")
        updated = probes.instrument_file(self.source, self.output, (ANGLE, VECTOR), self.manifest)
        self.assertNotEqual(updated["output_sha256"], result["output_sha256"])
        self.assertIn("// Synthetic revision.", self.output.read_text())

    def test_reject_unsafe_paths_and_overwrites(self):
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, self.source, (ANGLE,))
        hardlink = self.directory / "hardlink.cpp"
        os.link(self.source, hardlink)
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, hardlink, (ANGLE,))
        source_link = self.directory / "source_link.cpp"
        source_link.symlink_to(self.source)
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(source_link, self.output, (ANGLE,))
        self.output.parent.mkdir()
        self.output.write_text("unrelated existing file\n")
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, self.output, (ANGLE,))
        self.output.unlink()
        self.output.symlink_to(self.source)
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, self.output, (ANGLE,))
        self.output.unlink()
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, self.output, (ANGLE,), self.source)
        self.manifest.write_text('{"schema":"unrelated"}\n')
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, self.output, (ANGLE,), self.manifest)
        self.assertFalse(self.output.exists())
        self.manifest.unlink()
        self.manifest.symlink_to(self.source)
        with self.assertRaises(probes.ProbeInstrumentationError):
            probes.instrument_file(self.source, self.output, (ANGLE,), self.manifest)

    def test_cli_uses_only_synthetic_files(self):
        script = Path(probes.__file__)
        result = subprocess.run(
            [sys.executable, str(script), "--input", str(self.source), "--output", str(self.output),
             "--entries", "0x088775AC,0x08877818", "--manifest", str(self.manifest)],
            text=True, capture_output=True, check=False,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 entries and 3 exits", result.stdout)
        self.assertEqual(json.loads(self.manifest.read_text())["exit_calls"], 3)


if __name__ == "__main__":
    unittest.main()
