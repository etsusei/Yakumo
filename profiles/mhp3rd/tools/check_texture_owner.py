#!/usr/bin/env python3
"""Bind the bounded original lobby owner gate to local source and module identities."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import tempfile
from check_texture_allocation import digest, unique_object

EXPECTED = {"calls_per_path":22,"constructor_cases":2,"state_reset_cases":2,
            "freed_owner_vptr_preserved_cases":2,"owner_address_reuses":2,"command_release_cases":2}


def validate(report):
    if (type(report.get("schema_version")) is not int or report["schema_version"] != 1 or
            report.get("scope") != "original_lobby_owner_construction" or report.get("success") is not True):
        raise ValueError("Unsupported owner gate scope")
    for field,value in EXPECTED.items():
        if type(report.get(field)) is not int or report[field] != value:
            raise ValueError(f"Owner coverage differs: {field}")
    if report.get("full_ram_vram_cpu_compared") is not True or report.get("resource_loader_executed") is not False:
        raise ValueError("Missing CPU/memory evidence or broadened loader claim")
    for field,limit in (("max_interpreter_slices",2000000),("max_aot_dispatches",10000)):
        if type(report.get(field)) is not int or not 0 < report[field] <= limit:
            raise ValueError(f"Missing or exceeded budget: {field}")
    pages=report.get("executed_pages")
    if not isinstance(pages,list) or not pages or len(pages)>1024 or len(set(pages))!=len(pages):
        raise ValueError("Missing or invalid executed pages")
    if any(type(pc) is not int or pc & 0xFFF or not (0x08804000 <= pc < 0x08966000 or pc==0x0A0E7000) for pc in pages):
        raise ValueError("Executed page outside bounded code domains")
    if 0x0A0E7000 not in pages:
        raise ValueError("Original overlay constructor was not observed")


def check(elf,overlay,module,oracle,output):
    if output.exists() or output.is_symlink(): raise ValueError("Refusing to overwrite evidence")
    root=Path(__file__).resolve().parents[3]
    paths={"elf":elf.resolve(),"lobby_image":overlay.resolve(),"lobby_module":module.resolve(),
           "oracle_binary":oracle.resolve(),"oracle_source":root/"profiles/mhp3rd/tests/texture_owner_oracle.cpp",
           "runner_source":Path(__file__).resolve(),"hash_helpers":Path(__file__).with_name("check_texture_allocation.py").resolve()}
    before={name:digest(path) for name,path in paths.items()}
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="owner-gate-",dir=output.parent) as temporary:
        stage=Path(temporary);raw=stage/"original.json"
        env={key:value for key,value in os.environ.items() if not key.startswith(("MHP3RD_","PSPRECOMP_"))}
        result=subprocess.run([str(paths[k]) for k in ("oracle_binary","elf","lobby_image","lobby_module")]+[str(raw)],
                              env=env,capture_output=True,text=True,timeout=60,check=False)
        if result.returncode: raise ValueError(f"Owner gate failed: {result.stderr[-3000:]}")
        if raw.stat().st_size>65536: raise ValueError("Owner metadata budget exceeded")
        report=json.loads(raw.read_text(),object_pairs_hook=unique_object);validate(report)
        if before!={name:digest(path) for name,path in paths.items()}: raise ValueError("Gate source or inputs changed")
        final={"schema_version":1,"recorded_at":datetime.now(timezone.utc).isoformat(),
               "scope":"bounded_original_lobby_owner_provenance","success":True,"original_report":report,
               "original_report_sha256":digest(raw),"sha256":before,
               "paths":{name:str(path) for name,path in paths.items()},
               "limitations":["Constructed heap/stack; original allocation, placement, construction, provider, reset and free execute separately.",
                   "Full object factory, asynchronous source loading and gameplay were not executed.",
                   "No live ownership registry or native production dispatch is installed."]}
        staged=stage/"published.json";staged.write_text(json.dumps(final,indent=2)+"\n");os.link(staged,output)
        return final


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("elf","overlay","module","oracle","output"): p.add_argument("--"+name,type=Path,required=True)
    args=p.parse_args()
    try:
        report=check(args.elf,args.overlay,args.module,args.oracle,args.output)
        print(f"Original owner gate: {report['original_report']['calls_per_path']} calls per path; passed")
    except (ValueError,OSError,subprocess.TimeoutExpired) as error: p.exit(1,f"{error}\n")


if __name__=="__main__": main()
