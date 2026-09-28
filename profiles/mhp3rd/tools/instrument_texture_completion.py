#!/usr/bin/env python3
"""Add build-local completion checkpoints to the unit-0024 read copy.

The completion copy keeps the read callbacks but deliberately omits the R1
read-result stop, so the original inline copy/transform/retirement path can
run to a bounded test-only terminal hook.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

import instrument_texture_read as read_output
import instrument_texture_transfer as transfer_output


class TextureCompletionInstrumentationError(ValueError):
    pass


SCHEMA = "mhp3rd-texture-completion-instrumentation-v1"
CALLBACK = "mhp3rd::native::texture_completion_checkpoint"
CALL_BOUNDARY_CALLBACK = "mhp3rd::native::texture_completion_call_boundary"
STOP_CALLBACK = "mhp3rd::native::texture_completion_oracle_stop_after_retirement"
STOP_INCLUDE = '#include "texture_completion_oracle_stop.hpp"'
CHECKPOINTS = (
    ("ClassifierReturn", 0x0886577C, "24c28b5af8d6cc7e8f19083ed8d6bd6b739ee43c08a294f3283ba4f7c57fd9c1"),
    ("CopyReturn", 0x088652AC, "9acfafefa15938a370737f4573033eeccc350e733e24226a8e5c1d04ad1b6438"),
    ("HelperReturn", 0x088659B4, "fcc0457fa253d40388c9b5ad5bad556cebbe51af22836d0bf793b2354a431d78"),
    ("PolicyReturn", 0x088659C4, "19665ef130ae8e29f81f6861d88cc793a835b3a6bc91f0378239b571ecdab1e4"),
    ("WorkerRequestCall", 0x088659CC, "5c2a3f197b494fcc93e3ac30b5498d89088d64edcf2b386c695c560734037aff"),
    ("WorkerRequestReturn", 0x088659E4, "5014796da2c249384a46332c52d85264cb886b2653ca7b7835c135bf77684bf6"),
    ("WorkerEventSetReturn", 0x088659F4, "4fb466ed29bb4c839ee931380fcace98eb81b0a6fab8e799b8e9fbf0296857e3"),
    ("WorkerWaitReturn", 0x08865A10, "fed27a170205460af0a9445c3f4097db05dd6c3a8f862fca6ec0c4393a2b158a"),
    ("WorkerEntry", 0x08865378, "e313d0aafdec6685a59ccdda7b33b068e611f7718552a84809e7ad12e23de5ea"),
    ("VerbatimBranch", 0x088653A0, "c36cc8a8c2654d2f6ff41bb7d9e2ef3a4db03975d4352321c5050d28f2fd54ab"),
    ("DigestSkippedBranch", 0x088653B4, "8589d76280d794beabedd80f849cc307c5ec0229d5189b30e7aebf5905c981f6"),
    ("TransformCall", 0x08865420, "20cafba67331effd63a260b45d3ab2f4a41e2f6c4a688a8b81d4cf9b566592ab"),
    ("TransformReturn", 0x08865428, "4407d9c4a2182be0365f5a2e4c439e6a94e03b1af7173294293e867be78c663a"),
    ("DigestCall", 0x08865440, "02a8dadb05961823432630168ba98270f31594e8aeaf6bfe3c97547c8ce693ae"),
    ("DigestReturn", 0x08865448, "8b48f2bc9025d8c9c482e7bbdbab83dcbbdcc13ac28c819a1779de92aab0bec3"),
    ("WorkerAckReturn", 0x088653C4, "7d1003eddc72c5f42fdaf523b8a96ab68622ea81b6a1a3bd9ca18ed47513b8d7"),
    ("WorkerCopyReturn", 0x08865368, "01d0923747fa640babaec75e80d750c54841b64d38252fc2693a9d892a544083"),
    ("RetirementCall", 0x08865814, "bcaf66af287654b7be90bcd2b9163f0e27934ff8dbbace61385aa7c2a23c85a1"),
    ("RetirementEntry", 0x08865D8C, "3a1844e1c9ced1e99606489db085a0299896130e34bfcdb32cb2b37df9716f58"),
    ("RetirementReturn", 0x0886581C, "7fa3afac75cded4d84844481b3f151ff22fe16fb08f3fbc39e85154f75d5d6eb"),
    ("UnsupportedCopyRoute", 0x088652C4, "4d30e9815597950df703005a1365fa960f7851f63e5e7e01ae942297ac9d5ac6"),
    ("FullQueueCancellation", 0x08865F00, "68e3384de2732c687427b6ab58d9050e9db853b3b7e59b68147a8793de5148b1"),
    ("GroupCancellation", 0x08866044, "bc5c9454ed73c0f39bfd7399405e81ea474d416499d776f9f368a7720a45ffc5"),
)

# The call-boundary callback is inserted after the generated call's delay-slot
# ``nop``.  Its site PC is the label containing that call, retained as an
# inserted constant; it must never be reconstructed from ctx.pc.  These hashes
# cover the complete original label blocks before either callback is inserted.
CALL_BOUNDARIES = (
    ("InlineCopyCall", 0x088652A4, "f71c7d68cc237ffe6597d8dfaf9176fc9665cb0bc38a9556e1bb39b9a9514c99"),
    ("HelperCall", 0x088659AC, "4476299dfa788eca57c237ab35b4d97026397bffe196b23d7ddca13ed8130700"),
    ("WorkerCopyCall", 0x08865360, "4f85780e6595d6c8ad126c08ada2d6c6abd6025c54b35c10d42ad77a9a141e65"),
    ("WorkerAckCall", 0x088653BC, "db7a44f5f23eaec139cd017520493eccbc5eb83752085380c590f08a7c8c2fbd"),
    ("TransformCall", 0x08865420, "20cafba67331effd63a260b45d3ab2f4a41e2f6c4a688a8b81d4cf9b566592ab"),
    ("DigestCall", 0x08865440, "02a8dadb05961823432630168ba98270f31594e8aeaf6bfe3c97547c8ce693ae"),
    ("RetirementCall", 0x08865814, "bcaf66af287654b7be90bcd2b9163f0e27934ff8dbbace61385aa7c2a23c85a1"),
    ("PolicyCall", 0x088659BC, "d1c02b0305e9d8afb5cb59e4311defda26347fdaa076e28912ef45c10cb97ce7"),
    ("WorkerRequestCall", 0x088659DC, "63159907e2bf4f0895638b1adda1ebdca34a96f0564824d07dccc3c4eb475327"),
    ("WorkerEventSetCall", 0x088659EC, "b6c71d2a0ebf5d21adecd586626ccb44fe39b9509d6914e02b97b4bef6db3394"),
    ("WorkerWaitCall", 0x08865A08, "c824869a2da59ed1867d0386896ca41812ec19e06d92adcdd123221b912f23e2"),
)

# Static audit entries that are deliberately kept distinct from the callback
# list above.  Some branch/cancellation points are not admitted to the current
# completion tracker yet, but their source identity and placement still need a
# mechanical guard before a future integration enables them.
AUDITED_CHECKPOINTS = (
    ("ClassifierReturn", 0x0886577C, "24c28b5af8d6cc7e8f19083ed8d6bd6b739ee43c08a294f3283ba4f7c57fd9c1", "after_call"),
    ("InlineCopyCall", 0x088652A4, "f71c7d68cc237ffe6597d8dfaf9176fc9665cb0bc38a9556e1bb39b9a9514c99", "after_delay_slot"),
    ("CopyReturn", 0x088652AC, "9acfafefa15938a370737f4573033eeccc350e733e24226a8e5c1d04ad1b6438", "after_call"),
    ("HelperCall", 0x088659AC, "4476299dfa788eca57c237ab35b4d97026397bffe196b23d7ddca13ed8130700", "after_delay_slot"),
    ("HelperReturn", 0x088659B4, "fcc0457fa253d40388c9b5ad5bad556cebbe51af22836d0bf793b2354a431d78", "after_call"),
    ("PolicyCall", 0x088659BC, "d1c02b0305e9d8afb5cb59e4311defda26347fdaa076e28912ef45c10cb97ce7", "after_delay_slot"),
    ("PolicyReturn", 0x088659C4, "19665ef130ae8e29f81f6861d88cc793a835b3a6bc91f0378239b571ecdab1e4", "after_call"),
    ("WorkerRequestCall", 0x088659DC, "63159907e2bf4f0895638b1adda1ebdca34a96f0564824d07dccc3c4eb475327", "after_delay_slot"),
    ("WorkerRequestReturn", 0x088659E4, "5014796da2c249384a46332c52d85264cb886b2653ca7b7835c135bf77684bf6", "after_call"),
    ("WorkerEventSetCall", 0x088659EC, "b6c71d2a0ebf5d21adecd586626ccb44fe39b9509d6914e02b97b4bef6db3394", "after_delay_slot"),
    ("WorkerEventSetReturn", 0x088659F4, "4fb466ed29bb4c839ee931380fcace98eb81b0a6fab8e799b8e9fbf0296857e3", "after_call"),
    ("WorkerWaitCall", 0x08865A08, "c824869a2da59ed1867d0386896ca41812ec19e06d92adcdd123221b912f23e2", "after_delay_slot"),
    ("WorkerWaitReturn", 0x08865A10, "fed27a170205460af0a9445c3f4097db05dd6c3a8f862fca6ec0c4393a2b158a", "after_call"),
    ("WorkerCopyCall", 0x08865360, "4f85780e6595d6c8ad126c08ada2d6c6abd6025c54b35c10d42ad77a9a141e65", "after_delay_slot"),
    ("WorkerCopyReturn", 0x08865368, "01d0923747fa640babaec75e80d750c54841b64d38252fc2693a9d892a544083", "after_call"),
    ("WorkerEntry", 0x08865378, "e313d0aafdec6685a59ccdda7b33b068e611f7718552a84809e7ad12e23de5ea", "entry"),
    ("VerbatimBranch", 0x088653A0, "c36cc8a8c2654d2f6ff41bb7d9e2ef3a4db03975d4352321c5050d28f2fd54ab", "branch"),
    ("DigestSkippedBranch", 0x088653B4, "8589d76280d794beabedd80f849cc307c5ec0229d5189b30e7aebf5905c981f6", "branch"),
    ("WorkerAckCall", 0x088653BC, "db7a44f5f23eaec139cd017520493eccbc5eb83752085380c590f08a7c8c2fbd", "after_delay_slot"),
    ("WorkerAckReturn", 0x088653C4, "7d1003eddc72c5f42fdaf523b8a96ab68622ea81b6a1a3bd9ca18ed47513b8d7", "after_call"),
    ("TransformCall", 0x08865420, "20cafba67331effd63a260b45d3ab2f4a41e2f6c4a688a8b81d4cf9b566592ab", "after_delay_slot"),
    ("TransformReturn", 0x08865428, "4407d9c4a2182be0365f5a2e4c439e6a94e03b1af7173294293e867be78c663a", "after_call"),
    ("DigestCall", 0x08865440, "02a8dadb05961823432630168ba98270f31594e8aeaf6bfe3c97547c8ce693ae", "after_delay_slot"),
    ("DigestReturn", 0x08865448, "8b48f2bc9025d8c9c482e7bbdbab83dcbbdcc13ac28c819a1779de92aab0bec3", "after_call"),
    ("EarlyGroupAbort", 0x088654F8, "2c89545bca6973feb01a95fa7dea855e459c5e282d0cc0e2bab95a87f43bc83d", "branch"),
    ("RetirementCall", 0x08865814, "bcaf66af287654b7be90bcd2b9163f0e27934ff8dbbace61385aa7c2a23c85a1", "after_delay_slot"),
    ("RetirementEntry", 0x08865D8C, "3a1844e1c9ced1e99606489db085a0299896130e34bfcdb32cb2b37df9716f58", "entry"),
    ("RetirementReturn", 0x0886581C, "7fa3afac75cded4d84844481b3f151ff22fe16fb08f3fbc39e85154f75d5d6eb", "after_call"),
    ("FullQueueCancellation", 0x08865F00, "68e3384de2732c687427b6ab58d9050e9db853b3b7e59b68147a8793de5148b1", "entry"),
    ("GroupCancellation", 0x08866044, "bc5c9454ed73c0f39bfd7399405e81ea474d416499d776f9f368a7720a45ffc5", "entry"),
)
_LABEL = re.compile(r"(?m)^L_([0-9A-F]{8}):(?=\r?$)")


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _after_delay_slot(source: str, labels: list[re.Match[str]], by_address: dict[int, int],
                      name: str, address: int, expected_hash: str,
                      callback: str, enum_name: str) -> tuple[int, dict]:
    index = by_address.get(address)
    if index is None or index + 1 >= len(labels):
        raise TextureCompletionInstrumentationError(
            f"missing completion call-boundary label 0x{address:08X}")
    previous = int(labels[index - 1].group(1), 16)
    following = int(labels[index + 1].group(1), 16)
    if previous != address - 4 or following != address + 4:
        raise TextureCompletionInstrumentationError(
            f"completion call-boundary neighbours changed at 0x{address:08X}")
    start = source.find("\n", labels[index].end()) + 1
    end = labels[index + 1].start()
    block = source[start:end]
    actual = _sha(re.sub(r"\s+", "", block).encode())
    if actual != expected_hash:
        raise TextureCompletionInstrumentationError(
            f"completion call-boundary block changed at 0x{address:08X}: {actual}")

    # A generated call block has exactly one RA assignment followed by the
    # delay-slot nop.  Inserting at the next line observes the arguments after
    # that nop and before the callee can overwrite them.
    delay = re.search(
        r"(?m)^\s*ctx\.gpr\[31\] = \([^\n]+\);\r?\n"
        r"\s*// nop\r?\n", block)
    if delay is None:
        raise TextureCompletionInstrumentationError(
            f"call-boundary delay slot changed at 0x{address:08X}")
    insertion = start + delay.end()
    text = (f"    {callback}(rt, ctx, "
            f"mhp3rd::native::TextureCompletionCallCheckpoint::{enum_name}, "
            f"0x{address:08X}u);\n")
    return insertion, {"name": name, "address": f"0x{address:08X}",
                       "sha256": actual, "placement": "after_delay_slot"}


def instrument_source(source: str, checkpoint_specs=None, *,
                      call_boundary_specs=None,
                      stop_after_read_result: bool = False) -> tuple[str, dict]:
    if CALLBACK in source or CALL_BOUNDARY_CALLBACK in source:
        raise TextureCompletionInstrumentationError("completion callback already present")
    read_source, read_manifest = read_output.instrument_source(
        source, checkpoint_specs, stop_after_read_result=stop_after_read_result)
    labels = list(_LABEL.finditer(read_source))
    by_address = {int(m.group(1), 16): i for i, m in enumerate(labels)}
    insertions: list[tuple[int, str]] = []
    entries = []
    audit_entries = []
    if checkpoint_specs is None:
        for name, address, expected_hash, placement in AUDITED_CHECKPOINTS:
            index = by_address.get(address)
            if index is None or index + 1 >= len(labels):
                raise TextureCompletionInstrumentationError(
                    f"missing audited completion label 0x{address:08X}")
            start = read_source.find("\n", labels[index].end()) + 1
            end = labels[index + 1].start()
            block = read_source[start:end]
            actual = _sha(re.sub(r"\s+", "", block).encode())
            if actual != expected_hash:
                raise TextureCompletionInstrumentationError(
                    f"audited completion block changed at 0x{address:08X}: {actual}")
            if placement == "after_delay_slot" and re.search(
                    r"(?m)^\s*ctx\.gpr\[31\] = \([^\n]+\);\r?\n"
                    r"\s*// nop\r?\n", block) is None:
                raise TextureCompletionInstrumentationError(
                    f"audited completion delay slot changed at 0x{address:08X}")
            audit_entries.append({"name": name, "address": f"0x{address:08X}",
                                  "sha256": actual, "placement": placement})
    completion_specs = CHECKPOINTS if checkpoint_specs is None else checkpoint_specs[24]
    for name, address, expected_hash in completion_specs:
        index = by_address.get(address)
        if index is None or index + 1 >= len(labels):
            raise TextureCompletionInstrumentationError(
                f"missing completion label 0x{address:08X}")
        previous = int(labels[index - 1].group(1), 16)
        following = int(labels[index + 1].group(1), 16)
        if previous != address - 4 or following != address + 4:
            raise TextureCompletionInstrumentationError(
                f"completion label neighbours changed at 0x{address:08X}")
        start = read_source.find("\n", labels[index].end()) + 1
        block = read_source[start:labels[index + 1].start()]
        actual = _sha(re.sub(r"\s+", "", block).encode())
        if actual != expected_hash:
            raise TextureCompletionInstrumentationError(
                f"completion block changed at 0x{address:08X}: {actual}")
        text = (f"    {CALLBACK}(rt, ctx, "
                f"mhp3rd::native::TextureCompletionCheckpoint::{name}, "
                f"0x{address:08X}u);\n")
        if name == "RetirementReturn":
            text += (f"    if ({STOP_CALLBACK}(rt, ctx)) {{\n"
                     "        ctx.pc = 0x08001000u;\n"
                     "        return;\n"
                     "    }\n")
        insertions.append((start, text))
        entries.append({"name": name, "address": f"0x{address:08X}",
                        "sha256": actual})
    call_entries = []
    if call_boundary_specs is None and checkpoint_specs is None:
        call_boundary_specs = CALL_BOUNDARIES
    if call_boundary_specs is not None:
        for name, address, expected_hash in call_boundary_specs:
            insertion, metadata = _after_delay_slot(
                read_source, labels, by_address, name, address, expected_hash,
                CALL_BOUNDARY_CALLBACK, name)
            insertions.append((insertion, metadata.pop("text", "") or
                               (f"    {CALL_BOUNDARY_CALLBACK}(rt, ctx, "
                                f"mhp3rd::native::TextureCompletionCallCheckpoint::{name}, "
                                f"0x{address:08X}u);\n")))
            call_entries.append(metadata)
    result = read_source
    for offset, text in sorted(insertions, reverse=True):
        result = result[:offset] + text + result[offset:]
    if STOP_INCLUDE not in result:
        result = result.replace('#include "texture_read_oracle_stop.hpp"\n',
                                '#include "texture_read_oracle_stop.hpp"\n' + STOP_INCLUDE + '\n', 1)
    return result, {"schema": SCHEMA, "unit": "generated_unit_0024",
                    "read_manifest": read_manifest, "checkpoints": entries,
                    "checkpoint_calls": len(entries),
                    "audited_checkpoints": audit_entries,
                    "audited_checkpoint_calls": len(audit_entries),
                    "call_boundaries": call_entries,
                    "call_boundary_calls": len(call_entries),
                    "read_stop_disabled": not stop_after_read_result,
                    "completion_terminal_stop": True,
                    "completion_stop_include_added": True}


def instrument_file(input_path: Path, output_path: Path, manifest_path: Path) -> dict:
    if any(path.is_symlink() for path in (input_path, output_path, manifest_path)):
        raise TextureCompletionInstrumentationError("symlinks are not allowed")
    source = input_path.read_text(encoding="utf-8")
    output, metadata = instrument_source(source)
    raw = output.encode("utf-8")
    metadata["source_sha256"] = _sha(source.encode())
    metadata["output_sha256"] = _sha(raw)
    manifest = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
    transfer_output._atomic_write_pair(output_path, raw, manifest_path, manifest)
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = instrument_file(args.input, args.output, args.manifest)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"instrumented {result['checkpoint_calls']} completion checkpoints")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
