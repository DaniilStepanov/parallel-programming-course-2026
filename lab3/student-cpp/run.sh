#!/usr/bin/env bash
# Stage 2 harness. The mock must be running (./run-mock.sh).
#   ./run.sh --impl student --rate 200
#   ./run.sh --impl student --sweep 50,100,200,500,1000 --seconds 10
# Another binary (e.g. with your extra files linked in): BIN=path ./run.sh ...
set -euo pipefail
cd "$(dirname "$0")"
exec "${BIN:-out/harness}" --data ../data "$@"
