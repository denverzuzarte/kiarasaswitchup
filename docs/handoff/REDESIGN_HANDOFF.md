# Redesign hand-off (2026-11-22, end of round 6 + intern experiment)

This is the starting point for the next agent working on speed. It covers what the engine is now, where the time goes, what has been tried (including what failed and why), and the measurement traps that cost us hours. Everything below was measured on the build host: AMD Ryzen 7 9700X (Zen 5, 8 cores / 16 threads), 15 GB RAM, Linux 7.2, Docker with cgroup v2. **The scorer is Intel Cascade Lake, so absolute numbers will differ there.**

Raw data is in `docs/handoff/raw/`. The reproducible timing script is `docs/handoff/measure.py`.

## 1. Where we are

`main` = `eeb320a`, which is round 6 merged with `main`'s tooling. Its committed `src/main` is byte-identical to a build of this branch.

Measured at the end of the session, on a quiet machine. Interleaved round-robin with exact timing (blocking `wait4`, no polling). Docker uses `scripts/docker_run.sh`'s flags: `--cpus 8 --memory 24g`, read-only root, all capabilities dropped. Previous `main` = `e5bea99`.

| dataset | mode | binary | wall ms min / median / max | CPU ms median | peak memory MiB |
|---|---|---|---|---|---|
| 20M events + 5M clicks | native | **eeb320a** | 596 / **605** / 648 | 4399 | 3566 (RSS) |
| | native | e5bea99 | 903 / 926 / 1002 | 6943 | 3847 |
| | docker | **eeb320a** | 734 / **749** / 1117 | 4595 | 1918 (cgroup peak, sampled) |
| | docker | e5bea99 | 946 / 1353 / 1407 | 6409 | 2311 |
| 60M events + 15M clicks | native | **eeb320a** | 2419 / **3138** / 3518 | 15888 | 11015 |
| | native | e5bea99 | 4542 / 4776 / 5176 | 30006 | 10672 |
| | docker | **eeb320a** | 2416 / **2461** / 2495 | 16222 | 5922 (cgroup peak, sampled) |
| | docker | e5bea99 | 4198 / 4207 / 4606 | 29378 | 6137 |

Notes:
- **Docker overhead is about 130–145 ms at 20M** (749 against 605 ms): container create/start/teardown plus the bind mounts. It's the same constant for every binary.
- **CPU throttling under `--cpus 8` is negligible.** The cgroup's `cpu.stat` shows a median of 1–3 throttled periods and under 5 ms throttled per run, even though we run 8 workers plus a coordinator thread.
- **60M native is disk-bound on this host.** The 5.4 GB input doesn't fit in the page cache next to roughly 11 GB of process memory (major faults: about 25K per run). Docker 60M came out *faster* than native (2.46 against 3.14 s), probably because by then the input was cached; this isn't understood. Don't draw conclusions from 60M absolute numbers on this host.
- The cgroup `memory.peak` values are **sampled every 20 ms** and come out much lower than native RSS (1.9 against 3.6 GB at 20M). The sampler probably misses the final peak, or the cgroup accounts differently. Treat them as unverified.
- **Correctness:** both binaries pass the 36 fixtures, the official dev answer (including `EVX_PSIZE=1024` forced person-table reruns), NCPU 1/2/3/8 determinism, fuzz sets against `scripts/reference_events.py`, and the 20M golden output. At 60M their outputs are identical row for row (6,536,686 `user_daily` + 28,014 `campaign_daily` rows).

## 2. Where the time goes (20M, native, 8 workers)

### Hardware counters (`raw/perf_stat.csv`, user space, all threads)

| counter | value |
|---|---|
| task-clock | 4.38 s CPU (about 7.2 of 8 cores busy over about 0.6 s) |
| instructions / cycles | 25.3 G / 19.2 G → **IPC 1.32** |
| branch misses | 108.5 M (2.45% of 4.43 G branches; about 5.5 per event) |
| cache misses | 184 M (31% of 598 M cache references) |
| page faults | 456 K (4 KB pages, by design: see §5) |

### Function profile (`raw/functions.txt`, share of all user cycles)

| function | share | phase | character |
|---|---|---|---|
| `evx_parse_events` | 17.4% | P1 | compute-bound (IPC ~2.4); writes each Cand into one of 256 partition streams |
| `evx_collect` | 13.2% | P3 | memory-bound: link-table and person-table probes (prefetched in batches of 64) |
| `evx_dedup_part` | 10.0% | P2 | DRAM-bandwidth-bound: reads the 1.2 GB Cand arena; private table per partition |
| `evx_lookup` | 8.1% | P1/P3/C1 | 51% of its samples are the table-entry load (a miss) |
| `evx_parse_ts` | 7.9% | P1/C1 | already SIMD (template compare + `vpmaddubsw`); called twice per event row, once per click |
| `evx_khash` | 5.2% | all | fmix64: three dependent multiplies; called about 4× per event |
| `evx_sessionize` | 5.1% | P6 | |
| `evx_attribute` | 4.8% | P6 | |
| `evx_parse_clicks` | 4.0% | C1 | |
| `evx_scatter_clicks` | 3.5% | C3 | IPC about 0.11 (random `lock xadd` cursors) |
| `link_intern` | 3.0% | P0 | |
| `links_finish` | 2.4% | P0c | union-find on worker 0, overlapped with parse (about 113 ms of CPU) |

### Phase timeline (`raw/timeline_20ms.txt`)

These are 20 ms buckets of perf samples. A fully busy bucket has about 640 samples (8 threads × 4 kHz × 20 ms), so low counts mean idle cores.

| ms | phase | notes |
|---|---|---|
| 0–20 | links parse + products (P0) | 400 samples: startup not fully parallel |
| 20–220 | **event parse (P1), about 200 ms** | ~1/3 of the run; worker 0 does the union-find (`links_finish`) for the first ~100 ms |
| 220–280 | dedup per partition (P2), about 60 ms | the bucket at 220 is partly idle (barrier tail) |
| 280–400 | **collect (P3), about 120 ms** | the bucket at 280 has 470 samples (barrier) |
| 400–460 | P4 compaction + click parse (C1), about 60 ms | the bucket at 400 has 534 samples |
| 460–480 | click grouping (C2/C3), about 20 ms | the bucket at 460 has 427 samples (serial C2 prefix sums) |
| 480–560 | group + sessionize + attribute (P5+P6), about 80 ms | |
| 560–600 | Parquet (P7), about 40 ms | **under-used**: 420 → 168 samples; `memmove` + `write` |
| (after) | process exit, not sampled | about 50 ms native, more in Docker: freeing about 3.6 GB of mappings |

## 3. Architecture now (round 6)

`CLAUDE.md` has the authoritative table. In short:
1. **Parse** writes 56-byte `Cand`s straight into 256 `event_id`-hash partitions (4 KB chunks bump-allocated from one arena).
2. **Dedup** runs per partition in a private, cache-resident table with no atomics, and marks the losing copies dead.
3. **Collect** walks the arena sequentially, interns persons (a `cmpxchg16b` table, about 1/8 of the candidates), and emits Wins into per-worker person-range chunk lists, reusing consumed Cand chunks.
4. **P4** builds dense IDs from person-table occupancy.
5. **Clicks** are parsed, then grouped.
6. **P5+P6** runs per person range: count, then place into an L2-resident scratch buffer, then sessionize, then attribute.
7. **P7** writes one Parquet part per worker, in parallel.

The binary is silent and fully static.

## 4. What has been tried

### Worked (round 6, total 934 → about 620 ms native at 20M)

The details are in `docs/ASM_OPTIMIZATIONS.md` §6i:
- huge pages only for random-access tables
- the partitioned Cand store with lock-free dedup
- person-range Win lists with recycled chunks
- grouping fused into the session job
- a branchless quote unwrap
- no per-person `lock inc` in collect
- parallel Parquet parts
- overlapping the products load with the links parse

### Tried and reverted

All of these were measured; see §6i.10 of `ASM_OPTIMIZATIONS.md`:
- **Dedup CAS fused into parse** (lag 6, then bursts): about 300 cycles per `lock cmpxchg` either way. No gain.
- **Whole-chunk software prefetch:** no gain.
- **Dedup and collect fused per partition:** 199 against 169 ms. Running both at once evicts the person and link tables.
- **Parallel `munmap` before exit:** TLB-shootdown IPIs, no gain.
- **Parquet encoded from Rows in dead arena memory:** cold read-for-ownership traffic made it slower than fresh pages.
- **Person table at 1/16 of candidates:** about 3 ms faster, but at 32 events per person this data is at the edge of the about-400 ms rerun.

### This session's micro attempts (inconclusive: see the 50 ms-timer trap in §5)

| patch (`docs/handoff/patches/`) | idea | result |
|---|---|---|
| `khash_crc.patch` | `evx_khash` → two `crc32` instructions | **−13%** (765 → 865 ms, 2 grid steps): CRC is linear, and its low bits cluster on structured IDs, so probe chains get longer. Probably genuinely slower. |
| `parse_ts_date_memo.patch` | TLS memo of the last date (YYYY-MM-DD → day number) in `evx_parse_ts` | ±0, but measured on the broken 50 ms grid: **re-measure** |
| `parse_prefetch_next_slot.patch` | `prefetchw` of the partition's next Cand slot in parse | ±0, same: **re-measure** |

All three pass the 121 unit tests. None passed through the full gates with exact timing.

### Local LLM intern (gpt-oss-20b, `tools/intern/`)

The ledger (`raw/intern_ledger.jsonl`, `raw/intern_summary.txt`) has 551 attempts from 16:46 to 19:45:
- 323 malformed or non-applying patches (59%)
- 152 gate failures (28%): 73 build, 60 fixtures, 19 other
- 68 correct but not faster (12%)
- 8 "accepted", all false positives (see §5)

**There are no verified improvements.** A 20B model is ineffective at hand-tuned AVX-512 assembly at any reasoning effort we tried. The harness itself is sound, and worth keeping as a referee for any change:
- gates, Docker benchmarks, rest windows
- confirmation runs
- architect-patch tasks
- an exact timer

Its config is in `build/intern/config.json` (not committed). The intern is stopped, and so is llama-server.

## 5. Measurement traps (each cost real time)

1. **`subprocess.run(timeout=...)` quantises wall time to about 50 ms.** CPython waits for a child with a timeout by polling with sleeps of up to 50 ms. Every harness benchmark before 19:45 landed on a 50 ms grid, and noise jumping one step produced 7–13% "wins". Time children with a blocking `wait`/`wait4` (as `measure.py` and the fixed harness do).
2. **THP direct compaction.** With `MADV_HUGEPAGE` on everything, first-touch faults cost about 3 s of sys time under memory pressure. Huge pages are now only on random-access tables. Recheck on the scorer, where there may be plenty of free memory.
3. **llama-server's default `--cache-ram` of 8 GB** silently filled host RAM, pushed the benchmark input out of the page cache, and slowed the same binary from 630 to 790 ms. If a model runs on the box, use `-cram 0 -no-kvu`.
4. **Shared host.** Another agent session ran benchmarks and rewrote `../../../fz` mid-session. Gate benchmarks on an idle CPU (`idle_wait`), and keep private copies of datasets.
5. **`pkill -f <pattern>`** matches the invoking shell's own command line and kills it (exit 144). Kill by PID, or use a `[x]` bracket in the pattern.
6. **Docker volume paths must be absolute.** A relative path is treated as a named volume, and the run fails in about 10 ms with rc 125.
7. **perf call graphs** (`-g`, `--call-graph lbr`) don't work on this host and kernel. Plain sampling plus `perf annotate` does. Callers have to be inferred.

## 6. Suggested redesign directions

Estimates are rough and assume about 605 ms native at 20M.

1. **Shrink the Cand (56 → 40 bytes or less).** Dedup and collect each stream the 1.2 GB arena from DRAM (about 180 ms of wall time combined, bandwidth-bound), and the arena is most of the about-3.6 GB RSS that exit teardown frees.
   - Drop `eid_h32` and recompute the hash in dedup (memory-bound anyway).
   - Store `ts_off` as 32 bits when the window span is under 2^32 ms (a second layout otherwise).
   - Store `ingest` as F1 seconds relative to the window.
   - Expected effect: −20 to −40 ms, plus less teardown.
2. **Resolve links during the person probe.** Collect does khash(raw) → link probe → khash(canonical) → person probe. A combined raw-ID → person-slot table built once (links are known by P3) would make it one probe per winner. `evx_lookup` + `evx_khash` are about 13% of cycles.
3. **Parse (about 200 ms, compute-bound, IPC 2.4).**
   - Multi-row SIMD field splitting: today one row's 64-byte blocks are scanned per iteration.
   - A cheaper F1 timestamp path; the date memo (patch above) is worth a proper re-measurement.
   - Skip the `ingest_ts` full parse when it equals the `ts` field bytes.
4. **Exit and teardown** (about 50 ms native, more in Docker). Reuse fewer, larger mappings, release the arena with `MADV_DONTNEED` in a background thread during Parquet, or shrink peak RSS (item 1).
5. **Idle spots:** the startup bucket (400/640 samples), barrier tails at each phase change (P2→P3, P3→C1, C1→C3), the serial C2 click prefix sum, and the Parquet tail (168/640). Together that's worth perhaps 20–30 ms of wall time.
6. **Recheck on Cascade Lake:**
   - the 256-partition count (L2 is 1 MB/core there as well, but its L3 is non-inclusive and slower)
   - the huge-page policy
   - 512-bit against 256-bit vectors (Cascade Lake's AVX-512 frequency license)

## 7. Reproduce

```sh
make && make unit && make fixtures-native PY=<python with polars>
# exact interleaved timings, native + docker, 20M + 60M:
docker build -t evx_ours:latest .
python3 docs/handoff/measure.py build/handoff-raw/walltimes NEW=src/main OLD=<other binary>
# profile + timeline: see the commands in this file's history (perf record -F 4000, perf script -F time,ip,sym)
```
