#!/usr/bin/env bash
# Builds the stage 2 harness with your aggregator: ./build.sh [your .cpp files ...] [-Idir ...]
# Needs g++ (C++20) and Boost headers (Boost.Asio; e.g. apt install libboost-dev, brew install boost).
# Every .cpp becomes an object in out/obj/ and is recompiled only when it or a kit header changed,
# so after the first build only your edited files are recompiled.
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-g++}
FLAGS=(-O2 -std=c++20 -pthread -Wall -Wno-unused-parameter -Wno-unused-private-field)
KIT_SRC=(src/harness.cpp src/kit.cpp src/schedule.cpp src/airline_client.cpp src/student_aggregator.cpp)

srcs=("${KIT_SRC[@]}")
incs=()
for a in "$@"; do
  case "$a" in
    -I*) incs+=("$a") ;;
    *) srcs+=("$a") ;;
  esac
done

mkdir -p out/obj
objs=()
pids=()
for s in "${srcs[@]}"; do
  [ -f "$s" ] || { echo "no such file: $s" >&2; exit 1; }
  abs=$(realpath "$s")
  o="out/obj/$(echo "$abs" | sed 's|/|_|g; s|\.cpp$||').o"
  d="${o%.o}.d"
  stale=1
  if [ -f "$o" ] && [ -f "$d" ]; then
    stale=0
    # the .d file lists the source and every header it includes
    for dep in $(sed 's/^[^:]*://; s/\\$//' "$d"); do
      if [ "$dep" -nt "$o" ]; then stale=1; break; fi
    done
  fi
  if [ $stale = 1 ]; then
    echo "compiling $s"
    "$CXX" "${FLAGS[@]}" -Isrc "${incs[@]}" -MMD -MF "$d" -c "$abs" -o "$o" &
    pids+=($!)
  fi
  objs+=("$o")
done
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ $failed = 0 ] || { echo "build failed" >&2; exit 1; }
"$CXX" "${FLAGS[@]}" "${objs[@]}" -o out/harness
echo "built $(pwd)/out/harness"
