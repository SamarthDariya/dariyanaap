#!/usr/bin/env bash
#
# E3: coordinated omission.
#
#     ./scripts/e3-omission.sh [out-dir]
#
# One target, stalled 200ms once per second. Same offered load. Two modes.
#
# Closed-loop stops sending while the target is frozen, so the requests that
# should have been issued during the stall are never issued and never timed.
# Open-loop issues them on schedule and measures each from its intended send
# time, so the stall appears as the latency it is.
#
# If the two rows report the same throughput and wildly different p99, that is
# the whole lesson of unit 0, and the reason the eleven repos after it measure
# with this tool instead of a hand-rolled loop.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="${1:-runs/e3}"
RATE="${RATE:-40000}"
CONNECTIONS="${CONNECTIONS:-32}"
DURATION="${DURATION:-3000}"

mkdir -p "$OUT"
rm -f "$OUT/summary.csv"

build/dariyanaap-null --payload 64 --stall-every 1000 --stall-for 200 \
    > "$OUT/null.log" 2>&1 &
NULL_PID=$!
trap 'kill $NULL_PID 2>/dev/null || true' EXIT

for _ in $(seq 50); do [[ -s "$OUT/null.log" ]] && break; sleep 0.1; done
PORT="$(awk 'NR==1 {split($2, a, ":"); print a[length(a)]}' "$OUT/null.log")"
echo "target on 127.0.0.1:$PORT, stalling 200ms every 1000ms"
echo "offered load: $RATE rps over $CONNECTIONS connections, ${DURATION}ms measured"
echo

echo "--- closed-loop (no rate: sends as fast as replies allow) ---"
build/dariyanaap --target "127.0.0.1:$PORT" --connections "$CONNECTIONS" \
    --duration "$DURATION" --warmup 500 --csv-dir "$OUT" || true
mv "$OUT/histogram.csv" "$OUT/histogram-closed.csv"
sleep 1

echo
echo "--- open-loop ($RATE rps on schedule, whatever the target does) ---"
build/dariyanaap --target "127.0.0.1:$PORT" --connections "$CONNECTIONS" \
    --rate "$RATE" --duration "$DURATION" --warmup 500 --csv-dir "$OUT" || true
mv "$OUT/histogram.csv" "$OUT/histogram-open.csv"

echo
echo "summary: $OUT/summary.csv"
