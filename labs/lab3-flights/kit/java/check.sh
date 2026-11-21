#!/usr/bin/env bash
# Checks your stage 1 output against ../data/expected-stage1.txt (ms= is ignored).
#   ./check.sh my-output.txt
set -euo pipefail
out=$(realpath "$1")
cd "$(dirname "$0")"
exec java tools/Stage1Check.java "$out" ../data/expected-stage1.txt
