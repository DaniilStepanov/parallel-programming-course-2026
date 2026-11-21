#!/usr/bin/env bash
# Checks your stage 1 output against ../data/expected-stage1.txt (ms= is ignored).
#   ./check.sh my-output.txt
set -euo pipefail
out=$(realpath "$1")
cd "$(dirname "$0")"
if [ ! -x out/stage1_check ] || [ tools/stage1_check.cpp -nt out/stage1_check ]; then
  mkdir -p out && ${CXX:-g++} -O2 -std=c++20 -Wall tools/stage1_check.cpp -o out/stage1_check
fi
exec out/stage1_check "$out" ../data/expected-stage1.txt
