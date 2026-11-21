#!/usr/bin/env bash
# Compiles the kit and your code into out/. Java 21.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf out && mkdir -p out
javac --release 21 -d out $(find src -name '*.java')
echo "built into $(pwd)/out"
