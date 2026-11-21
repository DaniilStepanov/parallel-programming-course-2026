#!/usr/bin/env bash
# Builds the C++ tools of the kit into out/: gen (data generator), stage1_check (stage 1 checker), mock (airline mock).
# Needs g++ (C++20); the mock also needs Boost headers (Boost.Asio).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
CXX=${CXX:-g++}
$CXX -O2 -std=c++20 -Wall tools/gen.cpp -o out/gen
$CXX -O2 -std=c++20 -Wall tools/stage1_check.cpp -o out/stage1_check
$CXX -O2 -std=c++20 -pthread -Wall tools/mock.cpp -o out/mock
echo "built $(pwd)/out/{gen,stage1_check,mock}"
