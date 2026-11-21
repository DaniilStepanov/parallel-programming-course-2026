#!/usr/bin/env bash
# Airline mock (C++) on 127.0.0.1:7700. Leave it running in a separate terminal.
#   ./run-mock.sh [--port 7700] [--airlines ../data/airlines.conf]
# Builds out/mock on first use (./build-tools.sh).
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x out/mock ] || [ tools/mock.cpp -nt out/mock ]; then
  mkdir -p out
  ${CXX:-g++} -O2 -std=c++20 -pthread -Wall tools/mock.cpp -o out/mock
fi
exec out/mock --schedule ../data/schedule.bin --airlines ../data/airlines.conf "$@"
