#!/bin/sh
# Interleaved wall-time benchmark of committed binaries.
# usage: scripts/bench.sh DATA_DIR RUNS CPUS REV...
# Each REV's committed src/main is extracted to build/bench/<rev>; every run
# executes all binaries once (round-robin, so host noise hits all alike).
# Output dir removal happens outside the timed region. Per run: wall ms,
# user+sys CPU ms, major page faults (> 0 means the input was re-read from
# disk, i.e. the run measured the host's page cache, not the binary).
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
        line=$(DATA_DIR=$data OUTPUT_DIR=build/bench/out NCPU=$cpus python3 -c '
import os, resource, subprocess, sys, time
t = time.monotonic()
subprocess.run([sys.argv[1]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
w = (time.monotonic() - t) * 1000
u = resource.getrusage(resource.RUSAGE_CHILDREN)
print("%d %d %d %d" % (w, (u.ru_utime + u.ru_stime) * 1000, u.ru_majflt, u.ru_maxrss // 1024))' "build/bench/$r")
        echo "$r $line" >> "$res"
        echo "run $i $r $line" | awk '{printf "run %s %-10s wall %6d ms  cpu %6d ms  majflt %d  rss %d MiB\n", $2, $3, $4, $5, $6, $7}'
    done
    i=$((i + 1))
done
echo "--- $data, $runs runs, NCPU=$cpus: wall ms min / median / max | cpu ms median | majflt median | peak RSS MiB"
for r in "$@"; do
    grep "^$r " "$res" | sort -k2 -n | awk -v r="$r" \
        '{w[NR]=$2; c[NR]=$3; m[NR]=$4; if ($5 > rss) rss = $5} END {
            n = asort(c, cs); asort(m, ms);
            printf "%-10s %6d / %6d / %6d | %6d | %d | %d\n", r, w[1], w[int((NR+1)/2)], w[NR], cs[int((n+1)/2)], ms[int((n+1)/2)], rss}'
done
rm -rf build/bench/out
