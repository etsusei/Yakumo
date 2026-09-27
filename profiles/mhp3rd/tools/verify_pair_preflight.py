#!/usr/bin/env python3
"""Check real paired binaries without constructing the game runtime.

Only --test-preflight is invoked. No ISO, ELF, save, display, or input device
is supplied. This gate proves startup identity/seal behavior, not gameplay.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import native_modes


def fingerprint(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def check_pair(baseline: Path, candidate: Path, audit_path: Path) -> dict:
    binaries = {"baseline": baseline.resolve(strict=True),
                "candidate": candidate.resolve(strict=True)}
    audit = json.loads(audit_path.read_text())
    if audit["baseline_commit"] != "4292eb66ee66eab37c327575382d071addcf6249":
        raise ValueError("Source audit is not the pinned B0")
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("MHP3RD_", "PSPRECOMP_", "DYLD_"))}
    records = []
    identities = {}
    with tempfile.TemporaryDirectory(prefix="yakumo-paired-preflight-") as temporary:
        data = Path(temporary)
        settings = data / "settings.ini"
        seed = b"ui.language=zh-CN\n"
        settings.write_bytes(seed)

        def invoke(role: str, label: str, extra: dict | None = None,
                   isolated: bool = True, rejected: bool = False) -> dict | None:
            env = dict(environment)
            if isolated:
                env["MHP3RD_DATA_DIR"] = str(data)
            env.update(extra or {})
            child = subprocess.run([str(binaries[role]), "--test-preflight"],
                                   env=env, text=True, capture_output=True, timeout=20)
            if rejected:
                if child.returncode == 0 or child.stdout.strip():
                    raise ValueError(f"{role}/{label}: rejection was not explicit")
                value = None
            else:
                if child.returncode != 0:
                    raise ValueError(f"{role}/{label}: {child.stderr[:500]}")
                value = json.loads(child.stdout)
                required = {"schema", "recorder_revision", "configuration_sha256",
                            "build_config_sha256", "gameplay_source_commit",
                            "baseline_provenance_sha256", "baseline_sealed",
                            "renderer_compiled", "aot_probes_compiled"}
                if set(value) not in (required, required | {"native_mode_schema"}) or value["schema"] != "yakumo-test-preflight-v1":
                    raise ValueError(f"{role}/{label}: wrong preflight schema")
                if "native_mode_schema" in value and value["native_mode_schema"] != native_modes.V2_SCHEMA:
                    raise ValueError(f"{role}/{label}: unknown native mode schema")
            if settings.read_bytes() != seed or sorted(p.name for p in data.iterdir()) != ["settings.ini"]:
                raise ValueError(f"{role}/{label}: preflight mutated the isolated directory")
            records.append({"role": role, "check": label, "exit_code": child.returncode,
                            "status": "passed"})
            return value

        for role in binaries:
            identities[role] = invoke(role, "isolated_identity")
            invoke(role, "requires_explicit_data_directory", isolated=False, rejected=True)
        reference, changed = identities["baseline"], identities["candidate"]
        for key in ("configuration_sha256", "build_config_sha256", "recorder_revision"):
            if reference[key] != changed[key]:
                raise ValueError(f"Pair differs in {key}")
        if reference.get("native_mode_schema") != changed.get("native_mode_schema"):
            raise ValueError("Pair differs in native mode schema")
        for role, identity in identities.items():
            if identity["baseline_sealed"] is not (role == "baseline"):
                raise ValueError(f"{role}: wrong baseline seal")
            if identity["renderer_compiled"] is not True or identity["aot_probes_compiled"] is not True:
                raise ValueError(f"{role}: missing renderer or observational AOT")
        if (reference["gameplay_source_commit"] != audit["baseline_commit"] or
                reference["baseline_provenance_sha256"] != audit["source_content_sha256"] or
                reference["recorder_revision"] != audit["recording_revision"]):
            raise ValueError("Baseline binary does not bind its source audit")
        if changed["baseline_provenance_sha256"]:
            raise ValueError("Candidate claims baseline provenance")
        mode_fields = native_modes.fields(reference.get("native_mode_schema"))
        for name in mode_fields:
            for value in ("verify", "native", "invalid"):
                invoke("baseline", f"reject_{name}_{value}", {name: value}, rejected=True)
        off = invoke("baseline", "explicit_all_off",
                     {name: "off" for name in mode_fields})
        if off != reference:
            raise ValueError("Explicit off changed baseline identity")
        # Read a different setting, proving the hash is effective configuration.
        other = invoke("candidate", "configuration_change", {"MHP3RD_UI_LANGUAGE": "en"})
        if other["configuration_sha256"] == changed["configuration_sha256"]:
            raise ValueError("Effective language override did not affect configuration identity")
    return {"schema": "yakumo-paired-preflight-validation-v1", "status": "passed",
            "recorded_at": datetime.now(timezone.utc).isoformat(),
            "binaries": {role: {"path": str(path), "sha256": fingerprint(path),
                                 "preflight": identities[role]} for role, path in binaries.items()},
            "audit_sha256": fingerprint(audit_path), "checks": records,
            "full_game_launched": False,
            "limitations": ["Only the early preflight code path was executed.",
                            "Gameplay, live inputs, presentation and user acceptance are pending."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; preserve prior evidence")
    report = check_pair(args.baseline, args.candidate, args.audit)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"Passed {len(report['checks'])} real-binary preflight checks; no game launched.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
