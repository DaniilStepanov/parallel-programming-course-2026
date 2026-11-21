#!/usr/bin/env bash
# Stage 2 harness. The mock must be running (./run-mock.sh).
#   ./run.sh --impl lab3.StudentAggregator --rate 200
#   ./run.sh --impl lab3.StudentAggregator --sweep 50,100,200,500,1000 --seconds 10
# Extra classpath (e.g. your own build dir): CP=path ./run.sh ...
set -euo pipefail
cd "$(dirname "$0")"
exec java -Xmx2g -cp "out${CP:+:$CP}" lab3.Harness --data ../data "$@"
