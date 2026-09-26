#!/usr/bin/env bash
# Assemble a fresh local pair after the two observed builds have been audited.
set -euo pipefail

profile_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$profile_dir/tools/package_test_pair.py" "$@"
