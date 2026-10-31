#!/usr/bin/env bash
# Throughput benchmark using redis-benchmark (install the tools with: brew install redis).
# usage: tests/bench.sh ./server
#
# Prints the exact setup next to the numbers. Run it 3 times and report the range, not one
# number: results move with whatever else the machine is doing.
set -euo pipefail

SERVER=${1:-./server}
PORT=6399
REQS=${REQS:-100000}
RUNS=${RUNS:-3}

if ! command -v redis-benchmark >/dev/null; then
    echo "redis-benchmark not found. Install it with: brew install redis" >&2
    exit 1
fi

"$SERVER" --port "$PORT" >/dev/null &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# Wait until the server really accepts connections (a freshly built binary can take a few
# seconds to start the first time macOS runs it), instead of sleeping a fixed time.
ready=0
for _ in $(seq 50); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then ready=1; break; fi
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done
# Something else could be listening on the port while OUR server failed to bind and exited.
if [ "$ready" -eq 1 ]; then
    sleep 0.3
    kill -0 "$SERVER_PID" 2>/dev/null || ready=0
fi
if [ "$ready" -ne 1 ]; then
    echo "The server did not start on port $PORT (is something else using it? try: lsof -nP -iTCP:$PORT)" >&2
    exit 1
fi

echo "date:     $(date '+%Y-%m-%d %H:%M')"
echo "machine:  $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sysctl -n hw.ncpu 2>/dev/null || nproc) cores, $(uname -sr)"
echo "build:    $(grep -m1 '^CXXFLAGS' Makefile | sed 's/.*:= //; s/ \$(SAN)//; s/ -MMD -MP//')"
echo "workload: redis-benchmark -t set,get -n $REQS, keyspace 10000 keys, 3-byte values, $RUNS runs each"
echo

for cfg in "1 1" "10 1" "50 1" "50 16"; do
    set -- $cfg
    clients=$1
    pipeline=$2
    echo "--- $clients client(s), pipeline $pipeline"
    for run in $(seq "$RUNS"); do
        out=$(redis-benchmark -p "$PORT" -t set,get -n "$REQS" -c "$clients" -P "$pipeline" -r 10000 -q 2>&1 \
              | tr '\r' '\n') || true
        if echo "$out" | grep -q 'requests per second'; then
            echo "$out" | grep 'requests per second' | tail -2 | sed "s/^/run $run: /"
        else
            echo "run $run: redis-benchmark gave no result. Its output was:" >&2
            echo "$out" >&2
            exit 1
        fi
    done
done
