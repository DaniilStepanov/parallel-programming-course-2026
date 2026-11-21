#!/usr/bin/env bash
# Airline mock (Java) on 127.0.0.1:7700. Leave it running in a separate terminal.
#   ./run-mock.sh [--port 7700] [--airlines ../data/airlines.conf]
set -euo pipefail
cd "$(dirname "$0")"
OUT=mock/out
SRC="$(find mock/src -name '*.java') src/lab3/Pricing.java src/lab3/Schedule.java src/lab3/Flight.java src/lab3/PriceKey.java"
if [ ! -d $OUT ] || [ -n "$(find $SRC -newer $OUT 2>/dev/null)" ]; then
  rm -rf $OUT && mkdir -p $OUT
  javac --release 21 -d $OUT $SRC
fi
exec java -Xmx3g -cp $OUT lab3mock.Mock --schedule ../data/schedule.bin --airlines ../data/airlines.conf "$@"
