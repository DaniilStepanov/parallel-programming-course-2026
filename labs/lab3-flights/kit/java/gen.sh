#!/usr/bin/env bash
# Generates ../data/schedule.bin (and queries.txt, airlines.conf) from the seed, then prints its sha256.
#   ./gen.sh [--seed 2026]
set -euo pipefail
cd "$(dirname "$0")"
java tools/Gen.java --seed 2026 --out ../data "$@"
sha256sum ../data/schedule.bin
