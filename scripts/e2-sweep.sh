#!/usr/bin/env bash
#
# E2: the rig's own ceiling.
#
#     ./scripts/e2-sweep.sh [out-dir]
#
# Runs dariyanaap against dariyanaap-null at rising concurrency. The target
# replies as fast as a socket allows, so what moves across the rows is the
# RIG's limit, not a server's — which is the number decision 7 requires be
# published and every later repo quotes.
#
# Read the connections_started column before anything else. If it falls short
# of connections_requested, that row offered less load than it claims and its
# throughput is not comparable to the rows above it.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="${1:-runs/e2}"
mkdir -p "$OUT"
rm -f "$OUT/summary.csv"

if [[ ! -x build/dariyanaap || ! -x build/dariyanaap-null ]]; then
    echo "build first: cmake --build build -j" >&2
    exit 1
fi

# One target for the whole sweep. Restarting it per step would put TCP
# connection setup and a cold accept path into the first seconds of every row.
build/dariyanaap-null --payload 64 --backlog 128 > "$OUT/null.log" 2>&1 &
NULL_PID=$!
trap 'kill $NULL_PID 2>/dev/null || true' EXIT

for _ in $(seq 50); do
    [[ -s "$OUT/null.log" ]] && break
    sleep 0.1
done
PORT="$(awk 'NR==1 {split($2, a, ":"); print a[length(a)]}' "$OUT/null.log")"
echo "target on 127.0.0.1:$PORT"
echo

for CONNECTIONS in 1 2 4 8 16 32 64 128 256 500 1000; do
    echo "--- $CONNECTIONS connections ---"
    # Warm-up matters here more than anywhere: E0 measured a 2.5x difference in
    # the clock's own resolution between a cold core and a warm one.
    build/dariyanaap \
        --target "127.0.0.1:$PORT" \
        --connections "$CONNECTIONS" \
        --duration 3000 \
        --warmup 500 \
        --csv-dir "$OUT" \
        || echo "  (step exited non-zero — check the accounting warning above)"
    # Let TIME_WAIT drain a little between steps so ephemeral ports last the
    # whole sweep.
    sleep 1
done

echo
echo "summary: $OUT/summary.csv"
