#!/usr/bin/env python3
"""Tests for the build-local texture command AOT seam injector."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).resolve().parents[1] / "tools" / "instrument_texture_commands.py"
SPEC = importlib.util.spec_from_file_location("instrument_texture_commands", TOOL)
assert SPEC is not None and SPEC.loader is not None
texture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(texture)


def generated_fixture(newline: str = "\n") -> str:
    """Make an original, small-bodied C++ fixture with the required label map."""
    head = [
        '#include "psprecomp/runtime.hpp"',
        '#include "recomp_units.hpp"',
        "namespace psprecomp {",
        "void recomp_unit_0038_entry(Runtime &rt, AllegrexContext &ctx,",
        "        std::uint16_t direct_entry_id, GuestMemory::AotFastView &aot_mem) {",
        "    std::uint32_t local_pc = ctx.pc;",
        "    std::uint32_t jump_target = 0u;",
        "    std::uint32_t local_transfers = 0u;",
        "    std::uint32_t entry_id = direct_entry_id;",
        "LOCAL_DISPATCH:",
        "    // Synthetic dispatch fixture; no game instructions are reproduced.",
    ]
    body = []
    for address in range(texture.ENTRY, texture.END + 4, 4):
        body.append(f"L_{address:08X}:")
        if address == texture.ENTRY:
            body.extend((
                "    ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-80));",
                "    goto L_0889E5C4;",
            ))
        elif address in (texture.EARLY_RETURN, texture.POSITIVE_RETURN):
            body.extend("    " + line for line in texture._RETURN_LINES)
        elif address == 0x0889E5E0:
            # A call-continuation return inside the span is permitted.
            body.extend(("    ctx.pc = 0x0889E5E8u;", "    return;"))
        elif address != texture.END:
            body.extend(("    // nop", f"    goto L_{address + 4:08X};"))
        else:
            body.append("    return;")
    foot = [
        "}",
        "void register_functions(Runtime &runtime) {",
        '    runtime.register_function(0x0889E5C0u, &recomp_unit_0038_entry, "fixture");',
        "}",
        "}",
    ]
    return newline.join(head + body + foot) + newline


class InstrumentSourceTests(unittest.TestCase):
    def test_instruments_entry_and_both_original_returns(self) -> None:
        source = generated_fixture()
        output, manifest = texture.instrument_source(source)
        self.assertEqual(manifest["schema"], "mhp3rd-texture-command-instrumentation-v1")
        self.assertEqual(manifest["entry"], "0x0889E5C0")
        self.assertEqual(manifest["positive_return"], "0x0889E7C8")
        self.assertEqual(manifest["no_command_return"], "0x0889E650")
        self.assertEqual(manifest["no_command_authority"], "unsupported")
        self.assertEqual(manifest["entry_calls"], 1)
        self.assertEqual(manifest["return_calls"], 2)
        self.assertEqual(output.count('#include "native/texture_command_dispatch.hpp"'), 1)
        self.assertEqual(output.count("texture_command_entry(rt, ctx)"), 1)
        self.assertEqual(output.count("texture_command_return(rt, ctx, jump_target)"), 2)
        self.assertIn(
            "L_0889E5C0:\n"
            "    if (mhp3rd::native::texture_command_entry(rt, ctx)) {\n"
            "        if (rt.stopped()) return;\n"
            "        local_pc = ctx.pc;\n"
            "        if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; }\n"
            "        return;\n"
            "    }\n"
            "    ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-80));",
            output,
        )
        self.assertIn(
            "L_0889E7C8:\n"
            "    jump_target = ctx.gpr[31];\n"
            "    // nop\n"
            "    mhp3rd::native::texture_command_return(rt, ctx, jump_target);\n"
            "    if (rt.stopped()) { ctx.pc = jump_target; return; }\n"
            "    local_pc = jump_target;",
            output,
        )
        early = output.split("L_0889E650:", 1)[1].split("L_0889E654:", 1)[0]
        self.assertIn(
            "jump_target = ctx.gpr[31];\n"
            "    // nop\n"
            "    mhp3rd::native::texture_command_return(rt, ctx, jump_target);\n"
            "    if (rt.stopped()) { ctx.pc = jump_target; return; }\n"
            "    local_pc = jump_target;",
            early,
        )
        self.assertIn("ctx.pc = 0x0889E5E8u;\n    return;", output)
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_source(output)

    def test_crlf_source_preserves_callback_line_endings(self) -> None:
        output, _ = texture.instrument_source(generated_fixture("\r\n"))
        self.assertIn("texture_command_entry(rt, ctx)) {\r\n", output)
        self.assertIn("texture_command_return(rt, ctx, jump_target);\r\n", output)

    def test_rejects_changed_anchors_and_return_shapes(self) -> None:
        source = generated_fixture()
        changes = (
            source.replace("L_0889E5C0:", "L_0889E5C1:"),
            source.replace("L_0889E5C4:", "L_0889E5C8:"),
            source.replace("L_0889E7D0:", "L_0889E7D4:"),
            source.replace("L_0889E7C8:", "L_0889E7C8:\nL_0889E7C8:"),
            source.replace("-80));", "-64));"),
            source.replace("L_0889E7C8:\n    jump_target", "L_0889E7C8:\n    extra();\n    jump_target"),
            source.replace("L_0889E7C8:\n    jump_target = ctx.gpr[31];\n    // nop",
                           "L_0889E7C8:\n    jump_target = ctx.gpr[31];\n    store();"),
            source.replace("L_0889E650:\n    jump_target", "L_0889E650:\n    other_return = 1;\n    jump_target"),
            source.replace("AllegrexContext &ctx", "AllegrexContext &other"),
            source.replace('#include "psprecomp/runtime.hpp"', "// missing runtime include"),
            source.replace("runtime.register_function(0x0889E5C0u,", "runtime.register_function(0x0889E5C4u,"),
        )
        for changed in changes:
            with self.subTest(change=changed[:90]):
                with self.assertRaises(texture.TextureInstrumentationError):
                    texture.instrument_source(changed)


class InstrumentFileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        base = Path(self.directory.name)
        self.source = base / "generated_unit_0038.cpp"
        self.output = base / "instrumented" / "generated_unit_0038.cpp"
        self.manifest = base / "instrumented" / "generated_unit_0038.cpp.json"
        self.source.write_text(generated_fixture())

    def test_atomic_outputs_are_identified_and_idempotent(self) -> None:
        original = self.source.read_bytes()
        manifest = texture.instrument_file(self.source, self.output, self.manifest)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(manifest["source_sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(manifest["output_sha256"], hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(json.loads(self.manifest.read_text()), manifest)
        self.assertEqual(texture.instrument_file(self.source, self.output, self.manifest), manifest)

    def test_refuses_source_alias_symlink_and_unrelated_outputs(self) -> None:
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.source)
        hardlink = self.source.parent / "hardlink.cpp"
        os.link(self.source, hardlink)
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, hardlink)
        source_link = self.source.parent / "source-link.cpp"
        source_link.symlink_to(self.source)
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(source_link, self.output)
        self.output.parent.mkdir()
        self.output.write_text("unrelated output")
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output)
        self.output.unlink()
        self.output.symlink_to(self.source)
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output)

    def test_refuses_unrelated_or_aliased_manifest(self) -> None:
        self.manifest.parent.mkdir()
        self.manifest.write_text('{"schema":"unrelated"}')
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output, self.manifest)
        self.manifest.unlink()
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output, self.source)
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output, self.output)

    def test_refuses_invalid_source_and_nonregular_destination(self) -> None:
        self.source.write_bytes(b"\xff")
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output)
        self.source.write_text(generated_fixture())
        self.output.parent.mkdir()
        self.output.mkdir()
        with self.assertRaises(texture.TextureInstrumentationError):
            texture.instrument_file(self.source, self.output)


if __name__ == "__main__":
    unittest.main()
