#!/usr/bin/env python3
"""Run bounded original transfer scenarios and bind their private sample/evidence."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from check_texture_allocation import digest, unique_object

EXPECTED={"cases":64,"retry_cases":24,"marker_clear_cases":8,"late_group_write_cases":16,
          "digest_match_cases":4,"digest_comparisons_observed":0,"normal_mode_cases":36,
          "data_transform_cases":32,"copy_worker_cases":14,"transform_worker_cases":56,"rounded_write_cases":4}
ENCODED_HASH="91f5655fe6f01631d64692c0be30e43acb90588e164e500770ec5dd41e972978"
DECODED_HASH="3f06d53ef775166b06a1bd98a8a03a0b79c5d895eb4ad652fc9ca3656a781885"


def validate(report):
    allowed=set(EXPECTED)|{"schema_version","scope","success","max_interpreter_slices",
        "full_ram_vram_cpu_compared","imports_modeled","event_scheduling_modeled",
        "transform_worker_executed","game_executed"}
    if type(report) is not dict or set(report)!=allowed:
        raise ValueError("Missing or unrecognized transfer report fields")
    if (type(report.get("schema_version")) is not int or report["schema_version"]!=1 or
            report.get("scope")!="original_read_copy_transform_with_modeled_imports" or report.get("success") is not True):
        raise ValueError("Unsupported transfer scope")
    for field,expected in EXPECTED.items():
        if type(report.get(field)) is not int or report[field]!=expected: raise ValueError(f"Transfer coverage differs: {field}")
    for field in ("full_ram_vram_cpu_compared","imports_modeled","event_scheduling_modeled","transform_worker_executed"):
        if report.get(field) is not True: raise ValueError(f"Missing or broadened execution evidence: {field}")
    if report.get("game_executed") is not False: raise ValueError("Unsupported game-execution claim")
    steps=report.get("max_interpreter_slices")
    if type(steps) is not int or not 0<steps<=2000000: raise ValueError("Missing or exceeded execution budget")


def check(elf,iso,workspace,oracle,output):
    if output.exists() or output.is_symlink(): raise ValueError("Refusing to overwrite evidence")
    root=Path(__file__).resolve().parents[3]
    paths={"elf":elf.resolve(),"oracle_binary":oracle.resolve(),
           "oracle_source":root/"profiles/mhp3rd/tests/texture_transfer_oracle.cpp",
           "runner_source":Path(__file__).resolve(),"hash_helpers":Path(__file__).with_name("check_texture_allocation.py").resolve(),
           "resource_manifest":(workspace/"manifest.json").resolve(),"decoded_entry":(workspace/"raw-entries/01489.bin").resolve()}
    before={name:digest(path) for name,path in paths.items()}
    if before["decoded_entry"]!=DECODED_HASH: raise ValueError("Decoded sample identity differs")
    manifest=json.loads(paths["resource_manifest"].read_text(),object_pairs_hook=unique_object)
    rows=[row for row in manifest["entries"] if row.get("id")==1489]
    if len(rows)!=1: raise ValueError("Missing or ambiguous sample")
    row=rows[0]
    for key,value in {"block_address":285849,"source_offset":700106752,"extracted_length":22528,"sha256":DECODED_HASH}.items():
        if row.get(key)!=value: raise ValueError(f"Sample metadata differs: {key}")
    with iso.open("rb") as stream: stream.seek(row["source_offset"]);encoded=stream.read(row["extracted_length"])
    if hashlib.sha256(encoded).hexdigest()!=ENCODED_HASH: raise ValueError("Original source window differs")
    decoded=paths["decoded_entry"].read_bytes()
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="transfer-gate-",dir=output.parent) as temporary:
        stage=Path(temporary);encoded_path=stage/"sample.encoded";decoded_path=stage/"sample.decoded";raw=stage/"original.json"
        encoded_path.write_bytes(encoded);decoded_path.write_bytes(decoded)
        env={key:value for key,value in os.environ.items() if not key.startswith(("MHP3RD_","PSPRECOMP_"))}
        result=subprocess.run([str(paths["oracle_binary"]),str(paths["elf"]),str(raw),str(encoded_path),str(decoded_path)],
                              env=env,capture_output=True,text=True,timeout=60,check=False)
        if result.returncode: raise ValueError(f"Original transfer gate failed: {result.stderr[-3000:]}")
        if raw.stat().st_size>65536: raise ValueError("Report exceeds metadata budget")
        report=json.loads(raw.read_text(),object_pairs_hook=unique_object);validate(report)
        if before!={name:digest(path) for name,path in paths.items()}: raise ValueError("Gate source/input identities changed")
        with iso.open("rb") as stream: stream.seek(row["source_offset"]);after=stream.read(row["extracted_length"])
        if after!=encoded or encoded_path.read_bytes()!=encoded or decoded_path.read_bytes()!=decoded:
            raise ValueError("Private source window or staged fixture changed")
        final={"schema_version":1,"recorded_at":datetime.now(timezone.utc).isoformat(),
            "scope":"bounded_original_transfer_completion","success":True,"original_report":report,
            "original_report_sha256":digest(raw),"sha256":before,"paths":{k:str(v) for k,v in paths.items()},
            "sample":{"resource_id":1489,"iso_path":str(iso.resolve()),"iso_window_offset":row["source_offset"],
                      "transferred_bytes":22528,"block_address":285849,"encoded_sha256":ENCODED_HASH,"decoded_sha256":DECODED_HASH,
                      "decoded_sha1":hashlib.sha1(decoded).hexdigest()},
            "limitations":["I/O returns, cache/thread imports and event scheduling are explicit test models.",
                "Original read/copy/transform workers execute, but no game or actual concurrent scheduler was run.",
                "State-8 DATA.BIN route computes optional SHA-1 without the alternate-file comparison; matching digest is checked by the harness.",
                "Single-fragment fixtures do not certify multi-fragment traffic or alternate-file failures.",
                "No live source authority or native production dispatch installed."]}
        publish=stage/"published.json";publish.write_text(json.dumps(final,indent=2)+"\n");os.link(publish,output);return final


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("elf","iso","workspace","oracle","output"):p.add_argument("--"+name,type=Path,required=True)
    a=p.parse_args()
    try:
        r=check(a.elf,a.iso,a.workspace,a.oracle,a.output);print(f"Transfer completion gate: {r['original_report']['cases']} scenarios per path; passed")
    except (ValueError,OSError,subprocess.TimeoutExpired) as e:p.exit(1,f"{e}\n")


if __name__=="__main__":main()
