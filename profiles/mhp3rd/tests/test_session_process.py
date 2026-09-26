#!/usr/bin/env python3
"""Check recorder files from real normal and interrupted child processes."""

from __future__ import annotations

import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path


FILE_HEADER = struct.Struct("<8sHHI")
RECORD_HEADER = struct.Struct("<4sIQQHHI")
MAX_PAYLOAD = 65536
RUN_BEGIN = 1
RUN_END = 2
INPUT = 7


@dataclass(frozen=True)
class Record:
    sequence: int
    monotonic_ns: int
    kind: int
    fields: dict[str, object]


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> object:
    raise ValueError(f"nonstandard JSON number: {value}")


def read_records(path: Path) -> list[Record]:
    data = path.read_bytes()
    assert len(data) >= FILE_HEADER.size, "missing or partial file header"
    magic, version, header_bytes, reserved = FILE_HEADER.unpack_from(data)
    assert (magic, version, header_bytes, reserved) == (b"YKMJNL1\x00", 1, 16, 0), (
        "incorrect file header", magic, version, header_bytes, reserved
    )
    offset = FILE_HEADER.size
    records: list[Record] = []
    while offset < len(data):
        assert len(data) - offset >= RECORD_HEADER.size, f"partial record header at {offset}"
        marker, length, sequence, monotonic_ns, kind, reserved, checksum = RECORD_HEADER.unpack_from(
            data, offset
        )
        assert marker == b"YKE1" and reserved == 0, f"bad record marker/reserved at {offset}"
        assert length <= MAX_PAYLOAD, f"record payload exceeds framing limit at {offset}"
        assert kind in range(1, 13), f"unknown record kind at {offset}"
        assert sequence == len(records) + 1, f"noncontiguous sequence at {offset}"
        payload_start = offset + RECORD_HEADER.size
        payload_end = payload_start + length
        assert payload_end <= len(data), f"partial record payload at {offset}"
        payload = data[payload_start:payload_end]
        expected_checksum = zlib.crc32(data[offset : offset + 28] + payload) & 0xFFFFFFFF
        assert checksum == expected_checksum, f"CRC mismatch at {offset}"
        fields = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
        assert isinstance(fields, dict), f"record payload is not a JSON object at {offset}"
        records.append(Record(sequence, monotonic_ns, kind, fields))
        offset = payload_end
    assert offset == len(data), "unparsed trailing bytes"
    return records


def check_common_prefix(records: list[Record]) -> None:
    assert len(records) >= 2, "flushed RunBegin and Input were not recovered"
    assert records[0].kind == RUN_BEGIN and records[0].fields == {"role": "process_fixture"}
    assert records[1].kind == INPUT
    assert records[0].monotonic_ns == 100 and records[1].monotonic_ns == 200
    fields = records[1].fields
    assert fields["text"] == '\u65e5\u672c\u8a9e "\\\n\t\x01'
    assert type(fields["signed_min"]) is int and fields["signed_min"] == -(1 << 63)
    assert type(fields["unsigned_max"]) is int and fields["unsigned_max"] == (1 << 64) - 1
    assert type(fields["ratio"]) is float and fields["ratio"] == 2.5
    assert fields["enabled"] is True and fields["missing"] is None


def check_normal(binary: Path, directory: Path) -> None:
    path = directory / "normal.journal"
    result = subprocess.run(
        [str(binary), "--child-normal", str(path)], capture_output=True, text=True, timeout=8
    )
    assert result.returncode == 0, (
        f"normal child exited {result.returncode}: {result.stdout} {result.stderr}"
    )
    records = read_records(path)
    check_common_prefix(records)
    assert len(records) == 3 and records[-1].kind == RUN_END, "normal close did not write one footer"
    assert records[-1].fields == {
        "stop_reason": "synthetic_normal",
        "completed": True,
        "accepted_events": 1,
        "written_events": 1,
        "dropped_events": 0,
        "invalid_events": 0,
    }, "normal footer has incorrect completion or counters"


def check_immediate_exit(binary: Path, directory: Path) -> None:
    path = directory / "exit.journal"
    result = subprocess.run(
        [str(binary), "--child-exit", str(path)], capture_output=True, text=True, timeout=8
    )
    assert result.returncode == 23, (
        f"exit child returned {result.returncode} instead of deliberate _Exit(23): "
        f"{result.stdout} {result.stderr}"
    )
    records = read_records(path)
    check_common_prefix(records)
    assert all(record.kind != RUN_END for record in records), "_Exit unexpectedly ran the recorder destructor"


def check_killed_process(binary: Path, directory: Path) -> None:
    path = directory / "killed.journal"
    ready = Path(str(path) + ".ready")
    child = subprocess.Popen(
        [str(binary), "--child-wait", str(path)], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True
    )
    try:
        deadline = time.monotonic() + 3.0
        while not ready.exists() and time.monotonic() < deadline:
            if child.poll() is not None:
                stdout, stderr = child.communicate(timeout=2)
                raise AssertionError(
                    f"wait child exited before ready marker: {child.returncode}: {stdout} {stderr}"
                )
            time.sleep(0.01)
        assert ready.exists(), "wait child did not finish its flush barrier within 3 seconds"
        child.kill()
        stdout, stderr = child.communicate(timeout=3)
        killed = child.returncode == -signal.SIGKILL if os.name == "posix" else child.returncode not in (0, 24)
        assert killed, (
            f"wait child was not killed promptly: {child.returncode}: {stdout} {stderr}"
        )
    finally:
        if child.poll() is None:
            child.kill()
            child.communicate(timeout=3)
    records = read_records(path)
    check_common_prefix(records)
    assert all(record.kind != RUN_END for record in records), "killed process unexpectedly wrote RunEnd"


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: test_session_process.py SESSION_RECORDER_TEST_BINARY", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="yakumo-journal-process-") as temp:
        directory = Path(temp)
        check_normal(binary, directory)
        check_immediate_exit(binary, directory)
        check_killed_process(binary, directory)
    print("Session process lifecycle tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
