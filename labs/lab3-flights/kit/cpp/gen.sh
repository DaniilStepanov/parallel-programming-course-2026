#!/usr/bin/env bash
# Generates ../data/schedule.bin (and queries.txt, airlines.conf) from the seed, then prints its sha256.
#   ./gen.sh [--seed 2026]
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x out/gen ] || [ tools/gen.cpp -nt out/gen ]; then
  mkdir -p out && ${CXX:-g++} -O2 -std=c++20 -Wall tools/gen.cpp -o out/gen
fi
out/gen --seed 2026 --out ../data "$@"
sha256sum ../data/schedule.bin
