#!/bin/sh
# Interleaved wall-time benchmark of committed binaries.
# usage: scripts/bench.sh DATA_DIR RUNS CPUS REV...
# Each REV's committed src/main is extracted to build/bench/<rev>; every run
# executes all binaries once (round-robin, so host noise hits all alike).
# Output dir removal happens outside the timed region.
set -e
data=$1; runs=$2; cpus=$3; shift 3
mkdir -p build/bench
for r in "$@"; do git show "$r:src/main" > "build/bench/$r"; chmod +x "build/bench/$r"; done
res=build/bench/results.txt
: > "$res"
i=1
while [ "$i" -le "$runs" ]; do
    for r in "$@"; do
        rm -rf build/bench/out
        t0=$(date +%s%N)
        DATA_DIR=$data OUTPUT_DIR=build/bench/out NCPU=$cpus "build/bench/$r" >/dev/null 2>&1
        t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 ))
        echo "$r $ms" >> "$res"
        echo "run $i $r ${ms} ms"
    done
    i=$((i + 1))
done
echo "--- $data, $runs runs, NCPU=$cpus (wall ms: min / median / max)"
for r in "$@"; do
    grep "^$r " "$res" | awk '{print $2}' | sort -n | awk -v r="$r" \
        '{a[NR]=$1} END {printf "%-10s %6d / %6d / %6d\n", r, a[1], a[int((NR+1)/2)], a[NR]}'
done
rm -rf build/bench/out
