# Assembly-level optimisations (events engine)

A log of every low-level choice in `src/evx_*.s`, why it was made, and what was measured. The target is Cascade Lake: AVX-512 F/BW/VL/DQ/VNNI, with **no VBMI** (so no `vpermb` and no cross-lane byte permutes).

## 1. Register and ISA hygiene

| # | Optimisation | Where | Why |
|---|---|---|---|
| 1.1 | **Only `zmm16`–`zmm31` (and their ymm/xmm views) are used.** | all files | Legacy-SSE code cannot address these registers, so no SSE/AVX transition penalty is possible and **no `vzeroupper`** is needed at any C boundary. |
| 1.2 | Constants (`'\n'`, `','`, `'"'`) are broadcast once per batch into `zmm20`–`zmm22`. | `evx_parse_events`, `evx_split_line` | No per-line constant loads. Callee helpers never clobber `zmm20+`. |
| 1.3 | Custom internal convention for `evx_memeq` (it preserves every GPR except `rax`/`r11`). | `evx_util.s` | Hot callers (dedup probe, person intern, lookups) keep their loop state in caller-saved registers across the call, with no spills. |
| 1.4 | Leaf functions use the **red zone** for SIMD spills. | `evx_parse_ts` | There is no stack adjustment in the timestamp parser. |

## 2. CSV scanning (`evx_parse_events`, `evx_split_line`)

| # | Optimisation | Why |
|---|---|---|
| 2.1 | **64-byte block classification.** One `vmovdqu8` and three `vpcmpeqb` → `k` masks (newline, comma, quote) per block. Fields are cut with `tzcnt` + `blsr` over the comma mask. | A typical ~90-byte event line costs 2 blocks instead of ~90 byte compares and branches. |
| 2.2 | **`bzhi` truncation at the newline.** The comma and quote masks are cut at the first newline bit, so newline search and field splitting are a single pass. | No second scan for line ends. |
| 2.3 | **Fault-suppressing masked loads for the tail** (`vmovdqu8 zmm{k}{z}`, mask = `bzhi(-1, remaining)`). | The scanner never reads past the task's end. That means no padded mappings or copied buffers are needed, and the last line of a file needs no special case. |
| 2.4 | **Virtual newline** (`bts rax, remaining`) in the final partial block. | A last line without `\n` is handled by the same code path. |
| 2.5 | **Quote rule without a state machine.** A field is wrapped iff len ≥ 2 and both ends are `"`; the line is valid iff `popcnt(quote mask) == 2 × wrapped`. | This exactly implements "a quote anywhere except around a whole field is malformed" with one `popcnt` per block and two byte compares per field. |
| 2.6 | Comma positions are stored only up to 7; the count continues past that. | A line with too many fields can't overflow the frame, and it's rejected with a single compare. |
| 2.7 | **Validation is computed once per physical row**, at parse time, as a flag in the candidate. Rows are never re-parsed after dedup. | Spec §12.2 ("parse each physical row once"). |
| 2.8 | **Lookups only for rows that are still valid**, and the **product lookup only for purchases** (only purchases need a category; for other types, emptiness is the only product rule). | Skips about 97% of the product hashing and probing on generated data. |
| 2.9 | `event_type` is checked as **one 4- or 8-byte integer compare** (`"view"`, `"cart"`, `"purchase"`). | Case-sensitive, exact, and branch-light. |

## 3. Timestamps (`evx_parse_ts`)

| # | Optimisation | Why |
|---|---|---|
| 3.1 | Form dispatch on **length and one byte** (20 with `'Z'` → F1, 25 with a sign → F2, anything else → F3). | No regex-like scanning. |
| 3.2 | **SIMD template check** of `YYYY-MM-DDTHH:MM:SS`: one masked 32-byte load, `vpcmpeqb` against a separator template, and `vpsubb '0'` + `vpcmpub ≤ 9` for the digits. Both masks are merged and compared with `0x7FFFF`. | The whole 19-byte shape is validated in about 6 instructions, ASCII-only by construction. |
| 3.3 | **Digit-pair gather + `vpmaddubsw`.** `vpshufb` gathers the 7 two-digit fields (within lanes; there's no VBMI on Cascade Lake, so the seconds pair is taken from lane 1). One `vpmaddubsw` with `(10, 1)` weights produces all 7 numbers at once. | Replaces 14 scalar multiply-adds. |
| 3.4 | Leap-year check from the two century halves (`YY` hi / `YY` lo) without any division. | `y % 4 = lo % 4`, `y % 100 = lo`, `y % 400 ⇔ hi % 4`. |
| 3.5 | **Hinnant days-from-civil with reciprocal multiplications** (`/400` → `×5243 >> 21`, `/100` → `×5243 >> 19`, `/5` → `×52429 >> 18`), exact over the shifted year range. Shifting the year by one era (+400) keeps every value non-negative. | No `div` in the timestamp path. |
| 3.6 | **F3 overflow via `imul`/`add` overflow flags (`jo`).** | Value-based range check (leading zeros are unlimited) for 1 extra branch per digit. |

## 4. Dedup (`evx_dedup_insert`)

| # | Optimisation | Why |
|---|---|---|
| 4.1 | **Lock-free global table.** One `lock cmpxchg` claims an empty slot, and one replaces a winner with a smaller `ingest`. All workers insert concurrently; no shards or locks. | Global across files (spec §7.1); a slot only moves 0 → key or between copies of one key, so there's no ABA hazard. |
| 4.2 | **16-bit hash tag in the pointer's unused top bits** (user pointers < 2^47). | Most probe mismatches are rejected from the 8-byte slot alone, without touching the 64-byte candidate. |
| 4.3 | **`prefetchw` of the slot of the candidate 8 ahead.** | Overlaps the random DRAM misses of a table larger than cache. The `w` hint fetches the line in exclusive state for the CAS. |
| 4.4 | Table sized to a power of two ≥ 2× candidates (load ≤ 0.5), with `and`-mask indexing. | Short linear-probe chains, no modulo. |

## 5. Collect and group (`evx_collect`, `evx_scatter`)

| # | Optimisation | Why |
|---|---|---|
| 5.1 | **Sequential walk of the dedup table** to find winners. | Hardware-prefetch friendly; no hash-order random reads. |
| 5.2 | **Person ID = person-table slot index**, claimed with the same CAS that interns the name. | No ID counter, and no race to publish an ID after the insert. |
| 5.3 | `lock inc` histogram during the same pass. | Counting-sort counts come for free. |
| 5.4 | **Counting-sort scatter with `lock xadd` cursors.** Every person's events become one contiguous range of 16-byte records (offsets, not per-person allocations: spec §8). | One pass, no comparison sort across people. |
| 5.5 | Slot → dense-ID remap reuses the `counts` array in place (C side, P4). | `offsets`/`cursor`/names are sized by **people**, not by table slots. This saved ~1 GB at 20M events. |

## 6. Sessionize (`evx_sessionize`)

| # | Optimisation | Why |
|---|---|---|
| 6.1 | **Hybrid sort.** Insertion sort with 16-byte `vmovdqu64` moves for n ≤ 32; LSD radix for larger n. | Most people have few events, and whales (up to ~2% of all events) get O(n). |
| 6.2 | **All radix histograms from one read** of the data (`radix_passes` × 256 counters on the stack). | One counting pass instead of one per digit. |
| 6.3 | **Pass skipping:** a digit that's identical for every key is skipped. | Common for high bytes. |
| 6.4 | **Number of radix passes derived from the window span** (`ts_off < span`): 4 for a 14-day window, 5 for a > 2^32 ms window. | General for any manifest; nothing is hard-coded. |
| 6.5 | One `div` per **session** (not per event) for the UTC day, done as unsigned on `base_ms + ts_off`. | Sessions are ~1/5 of events. |
| 6.6 | **Top category via a 256-bit touched bitmap** (4 × u64) and `tzcnt`/`blsr`. Category IDs are byte-order ranks, so an ascending walk with a strict `>` is the byte-wise tie-break. The walk also clears the state. | No 256-entry reset per day and no string compares. |
| 6.7 | Price and category packed in one u32 (`price` < 2^24 because the max is 10,000,000). | 16-byte grouped events, 4 per cache line. |

## 6b. Clicks and attribution (`evx_click.s`)

| # | Optimisation | Why |
|---|---|---|
| 6b.1 | **No click dedup table.** Copies of a click are identical by the rules, so credit (a maximum) and assists (a set) are unchanged by duplicates. | Removes a ~12M-key CAS table (spec §8). |
| 6b.2 | **Cheap-first drop order:** field count/quotes → empty IDs → campaign lookup (L2-resident table) → ts/window → person resolution. | Dropped rows never pay for the two DRAM probes (link table, person table). |
| 6b.3 | **Shared `evx_canon`** for events (collect) and clicks. | Identical canonical name and tag bit for bit; one code path. |
| 6b.4 | The **events person table doubles as the click person index** (it's compact after §5 memory work). | No second index build; one probe per click. |
| 6b.5 | Clicks of people with no surviving events are dropped in C1. | Exact (no sessions) and shrinks C3/P6 work. |
| 6b.6 | `ClickEv` is Ev-shaped (8-byte key + 8 bytes); `click_id` stays in a cold 32-byte record read only for equal-ms ties. | The proven radix/insertion sort is reused (`evx_sort16`); string bytes stay out of the sort. |
| 6b.7 | **Click key in the click frame** (`k = ts - cw_start`): the qualifying window becomes `s ≤ k ≤ s + 7 d`, unsigned only. | No signed arithmetic, non-negative radix keys. |
| 6b.8 | **Two-pointer sweep** over sessions and sorted clicks; credit is `C[hi-1]`; the tie scan covers only the top equal-k group. | O(sessions + clicks) per person. |
| 6b.9 | **Lazy assist flush:** per campaign, enter/exit session indices; on exit, add the interval split by `day_first[]`. | O(clicks × days) instead of O(sessions × active campaigns) for whales. |
| 6b.10 | Attribution runs per block **right after** `evx_sessionize`, while the block's `Ev` is L2-hot. Blocks are weighted by events + clicks. | Near-fused cache behaviour without modifying the verified sessionizer. |
| 6b.11 | **Per-worker dense (campaign + organic) × day tables**, reduced in C. | No contended atomics on whale campaigns / `organic`. |
| 6b.12 | `evx_memcmp`: 64-byte masked compares, first difference via `tzcnt` of the not-equal mask, unsigned byte subtract. | Byte-wise `click_id` order without a byte loop. |

## 6c. Round 2: removing the memory stalls (profile-driven)

The profile at 48M showed 45% of user CPU in `evx_memeq` + `evx_lookup`. The cause was not the compares but the pointer chases into the 4 GB of CSV text that each compare needed (cache + TLB misses).

| # | Change | Effect (20M events + 5M clicks) |
|---|---|---|
| 6c.1 | **Key16 inline keys** (`evx_key16`, `evx_khash`, `evx_keq`): every ID ≤ 15 bytes is two words (bytes + length in byte 15), exact; ≥ 16 bytes keeps a pointer + len + hash32 and compares bytes (still exact). Used by dedup, collect, the person table, link/product/campaign tables and the click parse. Output names point straight at the inline key. | memeq left the profile; 1.70–1.96 s → 1.41–1.50 s |
| 6c.2 | **Parallel links build:** all workers parse and intern link IDs into the final lookup table (CAS on `k1` with a BUSY state, dense id per new ID); union-find + smallest name run on dense-id arrays on worker 0 *while the other workers parse events*. | links off the critical path; → 1.21–1.24 s, 7.5 of 8 cores busy |
| 6c.3 | **Software-pipelined `evx_collect`:** batches of 64 winners in four stages (prefetch Cand → user key + prefetch link entry → canonical key + prefetch person slot and counter → intern/count/emit). | collect 340 → 245 ms; → 1.12–1.17 s |
| 6c.4 | Tried, no gain, reverted: reusing the dead candidate buffer for `Ev`/rows (kernel time unchanged: only ~60K minor faults, THP + fault-around already work); dedup prefetch distance 16/32 (each `lock cmpxchg` is a full fence, which caps memory-level parallelism). | — |

Next candidates: a partitioned dedup table (each worker owns a cache-sized slice, no atomics); pipelined link/person probes in `evx_parse_clicks`; group events by person earlier to drop the `evx_scatter` pass.

## 6d. Round 3: per-segment cycle profiling and variant binaries

Method: `EVX_PROFILE=1` (src/prof.c) reads per-thread user-space hardware counters around every task. `make variant NAME=x VDEFS=.. VASDEFS=..` builds alternative implementations of one segment from the same sources, and `make profile BIN=build/x/main DEV=..` profiles them. Host: AMD Ryzen 7 9700X (Zen 5). The scorer is Intel Cascade Lake, so absolute cycle counts there will differ.

Cycles per item, summed over 8 threads (20M events / 5M clicks, and 60M / 15M):

| Segment | Item | Before | Variant(s) tried | After (20M) | After (60M) | Kept |
|---|---|---|---|---|---|---|
| links parse | link line | 1,720–1,830 (10.8 misses) | per-thread ID blocks vs shared atomic counter | **673–884** (2.8 misses) | 671 | ID blocks |
| click parse | valid click | 926–1,230 (IPC 0.48) | batch-64 pipeline (prefetch link entry → person slot → remap) vs serial | **532–584** (IPC 1.0–1.16) | 503 | pipeline |
| dedup | candidate | 336 (IPC 0.10) | partitioned private L1/L2 tables, no atomics (`DEDUP_PART=1`) | local dedup 94, but partition pass +44 and parse +47 → wall 1,163 vs 1,136 ms | — | **global CAS** (faster here) |
| group events | event | 180–217 (IPC 0.06) | batch-16: prefetch remap → prefetchw cursor → xadd → prefetchw dest | 152–187 | 237 | batched |
| collect | winner | 350–466 | batch size 16 / 32 / 64 / 128 | 376–399, all within noise | 372 | 64 |
| event parse | row | 238–258 (IPC 2.2–2.4) | — (compute-bound) | 252 | 237 | — |
| sessionize / attribute | event | 77–86 / 74–90 | — | 77 / 74 | 67 / 63 | — |

At 60M the wall times on this host are dominated by I/O: the 5.4 GB input doesn't stay in the 15 GB machine's page cache (major faults), so event parse waits on disk. The cycle counts are still valid.

## 6e. Round 4: kernel page zeroing and link-table misses

The per-phase `sys_ms` column (RUSAGE_THREAD) showed about 1.8 s of CPU spent in the kernel at 20M events, about 20% of all CPU. Most of it was first-touch zeroing of memory that was mapped worst-case-sized and written sparsely.

- **Compact output arenas (`Arena` in `main.c`).** Parse (`Cand`), collect (`Win`) and sessionize (`Row`) each append into per-worker chunks taken from one shared array with a single `atomic_fetch_add`. Before, each task wrote at a position derived from its input (bytes/8 for parse, the dedup slot range for collect, `offsets[k]` for rows). With THP, the sparse writes touched (and zeroed) every 2 MB page: rows 626 MB for 70 MB used, wins 513 MB for 320 MB. Chunks are 8× the largest single task output, so an abandoned chunk tail is at most 1/8 and is never touched.
- **Grouped events reuse the candidate pages.** With dense candidates, `Ev` (16 B × valid events) is written over the start of the dead candidate arena (56 B × candidates), which is already faulted in. `evx_scatter` writes all 16 bytes, including the pad.
- **Linked-id bit filter (`evx_collect` stage 2, `evx_parse_clicks` line loop and flush stage A).** At 20M, 37% of events and 39% of clicks have a user_id that appears in no link line, yet each paid a probe of the 128 MB link table. `link_intern` sets bit `khash >> fshift` (4 bits per link-table slot, at least 8 per id, 2 MB at 20M). A clear bit proves the id is unlinked, so the id is its own canonical name. In collect, stage 3 (canon + rehash) is skipped and stage 2 prefetches the person slot and counter directly. In clicks, the prefetch of the link entry and the `evx_canon` call are skipped. A set bit still does the exact lookup, so false positives cost only the old path. The test is `shrx` + `shr 6` + load + `bt reg, reg` (no `bt mem, reg`, which is microcoded).
- **Person table at ncand/16** (was /8). The overflow retry (>50% claims → ×8, rerun) keeps it safe. `EVX_PSIZE=1024` on a 400K-event seed retries twice and still matches the reference.
- **Parallel P4 / C2.** The dense-id compaction and both prefix sums run as two passes on fixed per-worker ranges (count, serial prefix of 8 values, fill). Dense ids stay in slot order.

| 20M events / 5M clicks, best of 4 | before | after |
|---|---|---|
| program total | 1.10–1.20 s | 0.97–0.99 s |
| collect | 244 ms (389 cyc/item, sys 512 ms) | 178–190 ms (317 cyc/item, sys 270–300 ms) |
| sessionize + attribute | 116 ms (sys 287 ms) | 63–68 ms (sys 30 ms) |
| group events | 109 ms (sys 238 ms) | 79–82 ms (sys 103–110 ms) |
| event parse | 213–261 ms (sys 738 ms) | 201–210 ms (sys 355–410 ms) |
| click parse | 84 ms (584 cyc/item) | 75–79 ms (523 cyc/item) |

Link union-find (worker 0, 180–190 ms) is now nearly as long as the event parse it overlaps (about 200 ms). It is the next critical path.

## 6f. `dopt` branch: no diagnostics, no dead code, Parquet straight from RAM

- Removed: the profiler (`prof.c`, `EVX_PROFILE`), all stats output and the invariant sums, the partitioned dedup (`evx_partition`, `evx_dedup_local`, the parser's `PartOut` path), the sequential `load_links`, and the `COLLECT_BATCH` / `SCATTER_SERIAL` / `LINK_IDBLOCK` / `NO_HUGEPAGE` / `INPUT_POPULATE` switches with the `variant` / `profile` targets. The parser frame keeps its size, so call alignment is unchanged.
- Parquet: before, the coordinator gathered every row into six column arrays on one thread, and then encoded and `write()`-ed them serially. Now each row group (a run of consecutive session tasks, about nrows / 4nw rows, at least 64K) is encoded by one worker straight from its tasks' `Row` arrays. `pq_layout` fixes the offsets and writes the magic and footer, and the workers `pwrite` their groups at their final offsets. The pool stays alive through P7. A `MAP_SHARED` mapping of the ftruncate-sized file was A/B tested: same wall time (867 vs 868 ms median at 20M), but about 500 major faults on the file holes, so `pwrite` was kept.

## 6g. Round 5: perf-guided (cycles, branch misses, cache misses per function)

`perf record -e cycles:u,branch-misses:u,cache-misses:u` at 20M events / 5M clicks, shares per function (cycles / branch misses / cache misses) before this round: dedup 22 / 9 / 16 %, collect 16 / 16 / **34** %, scatter 9 / 0 / **23** %, parse 14 / **23** / 1 %, parse_ts 5 / 12 / 1 %, attribute 4 / 12 / 1 %.

- **Dedup marks losers dead; collect walks candidates in memory order.** When `evx_dedup_insert` compares two copies of an event_id, both are in cache. The loser (the new copy on `>=` ingest, or the displaced resident after a successful replace) gets `user.k1 = 0`. After P2, `user.k1 != 0` means "live and valid". Before, `evx_collect` scanned the 1.5×-candidate dedup table and chased each winner pointer into a random 56-byte `Cand`, which usually spans two lines and was prefetched as two lines. Now stage 1 streams the dense candidate arena, so the hardware prefetcher covers it, and the dedup table is freed right after P2. Collect fell from 16.2 to 12.9 % of cycles and from 34 to 26 % of cache misses. Wall time: 885 → 858 ms median.
- **Branchless quote unwrap** (`evx_parse_events` and `evx_parse_clicks` field loops). The test "field wrapped in quotes?" (len ≥ 2, first = last = `"`) was two data-dependent branches per field. In this messy CSV the quoting is random, and they were the top two branch-miss sites in parse (about 17 % of its misses). It is now `cmovb` (fields shorter than 2 point at two zero bytes, `evx_noquote`, so nothing is read past the field), xor/or, `cmp 1; sbb; neg` → 0/1, and arithmetic. Wall time: 852 → 845 ms median.
- **Phase reorder (links spec):** the link-file parse + intern tasks run at the start of the P1 job instead of in a phase of their own. The worker completing the last link task (an `acq_rel` counter) runs the union-find. The coordinator maps and plans the click files during P2 (`pool_start`/`pool_wait`). The `madvise(MADV_SEQUENTIAL | MADV_WILLNEED)` call was a bitwise OR of two enumerated values (2 | 3 = 3), so it is now plain `MADV_WILLNEED`. Wall time: 885 → 852 ms median, measured together with an earlier quiet-host bench (most of the gain is the removed barrier).

| 20M / 5M, 8 interleaved runs | main | after round 5 |
|---|---|---|
| wall median | 885 ms | 845 ms |
| CPU (user+sys) median | 6.66 s | 6.33 s |
| 60M / 15M, wall min (4 runs; disk-bound host) | 3979 ms | 3773 ms |
| 60M peak RSS | 11350 MiB | 11347 MiB |

Not done, measured: the epoch-ms (F3) digit loop and the F1/F2/F3 length dispatch account for about 12 % of branch misses, but at about 1 mispredict per F3 row they are worth roughly 3 ms. Dedup (24 % of cycles, `lock cmpxchg`-bound) needs a partitioned design.

## 6h. Idle spots (perf per-thread timeline)

A `perf record -F 20000 -e cycles:u` timeline in 5 ms bins at 20M showed about 180 ms of 8-core time with workers idle or in the kernel. That includes a ~13 ms single-threaded start (products, link line count), about 10 ms at each first-touch phase switch, and ~20 ms of serialized `pwrite`s at the end. Outside the process, the container adds ~170 ms of startup and ~45 ms of teardown, and process exit costs ~42 ms (mapping teardown).

- **Parquet layout fixed up front.** A parallel size pass computes every column chunk's exact page-body size, including the RLE definition-level runs; `pq_presize` adds the header size and `pq_encode_done` verifies it. Then `pq_layout` writes the footer, and each worker `pwrite`s a row group as soon as it is encoded, so the kernel-serialized writes overlap the encoding of other groups. The coordinator builds and writes `campaign_daily` meanwhile (`pool_start`/`pool_wait`).
- **Parallel start.** `links_map` (coordinator), then a `job_startup` phase: products on worker 0, campaigns on worker 1, and `links_count` on every worker for one slice of the concatenated link bytes. The coordinator maps and plans the event files meanwhile, then `links_plan` allocates.
- **Tried and reverted: `pread` of every parse task into a huge-page anonymous buffer** (to avoid the file-mapping page tables and their teardown at exit). 20M wall went from 866 to 1401 ms and CPU from 6.5 to 10.4 s: the kernel zeroes the fresh 1.8 GB buffer and then copies the whole input into it, which costs far more than the file faults it saves.

| 20M / 5M, 10 interleaved runs (min / median) | wall |
|---|---|
| before | 868 / 873 ms |
| + Parquet presized layout | 861 / 868 ms |
| + parallel start | 857 / 866 ms |
| + pread input (reverted) | 1330 / 1401 ms |

## 6i. Round 6: partitioned store, fused grouping

This round was developed from round 3 on a separate branch and then merged. Where it overlaps rounds 4 and 5 and the `dopt` branch (6e–6h), it supersedes them. The global CAS dedup table, `evx_scatter`, the global `Ev` array and the single-file Parquet layout no longer exist. The round-5 ideas also found there are kept: losers marked dead, memory-order collect, branchless quotes, `MADV_WILLNEED`, coordinator overlap. Interleaved head-to-head at 20M events + 5M clicks on the same host: round 6 **634 ms** median against 890 ms for `main` at `e5bea99`.

Measured on the build host (Ryzen 7 9700X), interleaved runs, idle-CPU gated. Start of round: 934–947 ms.

| # | Change | Effect |
|---|---|---|
| 6i.1 | Huge pages only on random-access tables (dedup/person/counts/links); streaming buffers `MADV_NOHUGEPAGE`. THP direct compaction made first-touch faults cost ~3 s of sys time under memory pressure | sys 3.0 s → 0.8 s |
| 6i.2 | Tried: dedup CAS fused into parse (lag 6, then bursts of 16–64 after prefetching the slots). Each `lock cmpxchg` still ~300 cycles; no gain over a separate phase. Reverted | — |
| 6i.3 | **Partitioned Cand store**: parse writes each Cand into its `event_id`-hash partition's 4 KB chunk; dedup per partition in a private table without atomics; losers marked dead; collect walks the arena sequentially. 256 partitions measured best (16/64/128/512/4096 tried: more streams slow parse, fewer partitions spill the table) | dedup 180 → 58 ms; total ≈ 780 ms |
| 6i.4 | Wins into 256 person-range chunk lists; group per range in cache (no `lock xadd`). Fresh Win memory made collect +70 ms, so consumed Cand chunks are recycled as Win chunks | group 116 → 44 ms |
| 6i.5 | **Group fused into the session job** per range, into a per-worker scratch: the 313 MB `Ev` array and its faults are gone | total ≈ 690 ms |
| 6i.6 | Branchless, unrolled quote unwrap (80% of event rows are quoted): parse branch misses 3.4 → 1.0 per row | parse −20 ms |
| 6i.7 | No per-person `lock inc` in collect (58% of collect samples sat on it): dense IDs from person-table occupancy, counts per range in the session job | collect 140 → 115 ms |
| 6i.8 | `user_daily` as one Parquet part per worker, written in parallel, INT32 sessions/events, compact index columns, pre-reserved page buffer | Parquet 46 → 28 ms |
| 6i.9 | Coordinator loads products/campaigns while workers parse links (`pool_start`/`pool_wait`); link-line count is a vectorised byte loop, not a memchr per line | −8 ms |
| 6i.10 | Tried and reverted: whole-chunk software prefetch (no gain); fused dedup+collect per partition (199 vs 169 ms: the person and link tables get evicted); parallel `munmap` teardown before exit (TLB-shootdown IPIs, no gain); Parquet encoded from Rows in dead arena memory (cold RFOs, slower than fresh pages) | — |
| 6i.11 | All timing, profiling (`prof.c`), statistics and invariant reporting removed; the binary is silent | — |

Thread occupancy after this round: every pool phase is 98–99% busy (barrier imbalance 1–2%), and the serial gaps between phases total about 5 ms.

Result: **619–620 ms** wall on a quiet machine (start 934–947 ms; the `main` branch at that time: 861–919 ms). Correct on the 36 fixtures, the dev answer (including forced person-table retries), NCPU 1/2/3/8 determinism, fuzz sets against `reference_events.py`, and the 20M golden output.

## 6j. Round 7: 32-byte Rec, per-ID resolution, single-pass dedup + emit (current implementation)

Branch `r7-core`. This supersedes the Cand / `evx_collect` parts of 6i. The pipeline is now: P1 parse (Rec + raw-ID intern), then P1.5 `evx_resolve_raw` (canon, person intern, compact, `remap[rid]`), then P2 `evx_dedup_emit`, then P5+P6 as before. Measured on the build host (Ryzen 7 9700X) with interleaved native runs timed with a blocking `wait4`, at 20M events + 5M clicks. Start: `main` eeb320a, 617 / **622** / 630 ms, 3651 MiB peak RSS.

| # | Change | Effect |
|---|---|---|
| 7.1 | **32-byte Rec** instead of the 56-byte Cand: `ki` = (ingest seconds − window + 2^39) << 24 \| 24 hash bits (one compare gives ingest order), `eptr` = event_id pointer \| 16 more hash bits, `ts_off`, `rid`, `price_cat`. The chunk is a 32-byte header + 127 Recs, 32-byte aligned | arena 1.2 GB → 0.68 GB |
| 7.2 | **Raw-ID intern in parse**: each valid copy interns its raw `user_id` Key16 (`cmpxchg16b` slots, slot = rid), delayed through a 16-row prefetch ring. Ring sizes 8 and 32 measured equal. Overflow (more than half claimed) reruns P1 with an 8× table. `EVX_RSIZE` forces it | parse 200 → ~228 ms |
| 7.3 | The shared raw-claim counter took 6.5% of parse samples (`lock xadd` line bouncing). Claims are now counted per call and added in batches of 64 (slack of at most nw × 64 past the half-full limit) | parse −1.5% |
| 7.4 | **P1.5 `evx_resolve_raw`**: one link lookup and one person intern per *distinct* raw id (1.28M at 20M, instead of 21M winners). Batched 32 deep (scan, hash and prefetch the link entry, canon and prefetch the person slot, intern). The person table is sized `pow2(2 × raw claims)`, so it cannot overflow unless `EVX_PSIZE` forces it. Then P1.5 alone reruns. The compaction and `remap[raw slot] = dense id` happen in place | P1.5 + P4 ≈ 30 ms |
| 7.5 | **P2 `evx_dedup_emit`** replaces `evx_dedup_part` + `evx_collect`. The table holds tagged Rec pointers (hash bits 24–39 in the top 16 bits), so most mismatches need no Rec load. Equal 40-bit hashes are confirmed on the event_id bytes in the input (`evx_eid_eq`: the value ends at its first `,` or `"`). Then it walks the table and emits Wins `{ts_off, dense pid, price_cat}` into person ranges `pid >> dshift`, prefetching the Rec 48 slots ahead and `remap[rid]` 24 ahead (16/8 was 10 ms slower). The partition's Rec chunks go to the worker's free list and become the next partition's Win chunks | dedup + collect 180 → 110 ms |
| 7.6 | Partitions of ≥ 64K estimated rows, up to 1024 (512 at 20M), instead of 128K / 512. The private table (≈ 0.5 MB) stays in L2. 32K rows / 1024 partitions measured the same | −15 ms |
| 7.7 | Chunks come from per-worker 2 MB slabs of the arena (one atomic add per slab), allocated by `evx_chunk_alloc` (C, called from the asm) | — |
| 7.8 | Tried, default off: `MADV_POPULATE_WRITE` on each slab. **+18 ms** (614 vs 596 ms): a whole slab is pre-faulted even when only part of it is used. Kept behind `-DEVX_POPULATE=1` | reverted |
| 7.9 | The arena is unmapped by the idle coordinator while the workers write Parquet | wall ±0, RSS −100 MiB |
| 7.10 | Tried and reverted: the `parse_ts` date memo (`docs/handoff/patches/parse_ts_date_memo.patch`): 615 vs 616 ms. Skipping the ingest parse when its bytes equal `ts`: only 139 of 200,000 dev20m rows qualify, so not implemented. A table home-slot prefetch 8/16 Recs ahead in the insert loop (`IN_PF`): 598–601 ms either way, left off | — |

Phase times at 20M (wall, 8 workers): links + products 29 ms, P1 parse 229 ms, P1.5 resolve 17 ms, compaction + remap 13 ms, P2 dedup+emit 110 ms (256 partitions; less with 512), C1 53 ms, C3 22 ms, P5+P6 78 ms, Parquet 22–25 ms, exit about 50 ms.

Result (`make bench REVS="main HEAD" RUNS=5`, dev20m): **main 622 ms, r7-core 599 ms median** (−4%), CPU 4447 → 4297 ms, peak RSS 3651 → 3074 MiB. About 1.85 GB of that RSS is the mapped input (page cache counted in RSS); the anonymous part is about 1.2 GB. The plan estimated 420–460 ms. Parse absorbed most of what collect saved: the raw-ID probe per row costs about as much as collect's per-winner probe did. Outputs are identical to eeb320a on dev20m (NCPU 1/2/3/8, `EVX_PSIZE=1024`, `EVX_RSIZE=4096`), on the dev answer, on the 36 fixtures, and on fuzz seeds 3 and 11 against `reference_events.py`.

## 7. OS and memory (C side, in support of the asm)

- **Lazily committed** `MAP_NORESERVE` anonymous buffers sized by worst-case bounds (≥ 8 bytes per candidate row). Untouched pages cost nothing, so no growth or realloc logic is needed in asm.
- `MADV_HUGEPAGE` on the large tables, to cut TLB misses on the random-access dedup and person tables.
- Input is `mmap`ed read-only with `MADV_SEQUENTIAL | MADV_WILLNEED`; candidates point into the mappings (event IDs and names are never copied).
- Buffers are released as soon as their phase is done (dedup table and candidates after P4; wins and counts after P5).

## Measurements

8 workers. "Native" means the host; "Docker" means `scripts/docker_run.sh` with the scorer flags.

| Dataset | Events | Rows | Total | Parse | Dedup | Collect | Prefix | Group | Session | Parquet | Peak RSS |
|---|---|---|---|---|---|---|---|---|---|---|---|
| dev (seed 1), Docker | 1.07M cand | 117,038 | 114 ms | 30 | 12 | 22 | 18 | 6 | 9 | 6 | 363 MiB |
| dev (seed 1), native, after dense persons | 1.07M | 117,038 | 72 ms | 19 | 9 | 18 | 4 | 4 | 4 | 5 | 435 MiB |
| seed 11, native | 3M | 346,064 | 341 ms | 72 | 37 | 149 | 17 | 13 | 13 | 14 | 1023 MiB |
| seed 12, Docker | 10M | 1,139,012 | 1.04–1.10 s | 276–354 | 124–143 | 270–306 | 83 | 60 | 42 | 36 | 2.57 GiB |

| dev (seed 1), `make run-dev`, 8 CPU / 10g, 3 runs | 1.07M cand | 117,038 | 76–84 ms | 24–27 | 9–10 | 18–20 | 4–5 | 4 | 4–5 | 4–5 | 493 MiB |
| dev20m (seed 1), `make run-dev DEV=dev20m`, warm, 2 runs | 21.4M cand | 2,206,402 | 2.45–2.81 s | 666–671 | 260–275 | 660–752 | 179–474 | 149 | 94 | 105–125 | 4.8 GiB |
| dev20m, cold page cache (first run) | 21.4M cand | 2,206,402 | 3.78 s | 850 | 358 | 1513 | 202 | 146 | 235 | 150 | 4.8 GiB |
| dev48m (seed 1), `make run-dev DEV=dev48m RUN_MEM=24g`, warm, 2 runs | 51.4M cand | 5,242,106 | 5.85–5.97 s | 1494–1689 | 554–617 | 1661–1695 | 371–373 | 424–432 | 219–226 | 180–235 | 10.6 GiB |
| dev48m, cold page cache (first run) | 51.4M cand | 5,242,106 | 8.50 s | 2157 | 979 | 3014 | 464 | 438 | 246 | 272 | 10.5 GiB |
| dev48m + 12M clicks (both tables), Docker 24g, warm | 51.4M cand + 12.1M clicks | 5.24M + 28,014 | 5.13 s | 542 | 536 | 1635 | 50 | 345 | 471 (+attr) | 221 | 8.0 GiB |
| same, cold first run | | | 7.06 s | 1636 | 712 | 1712 | 90 | 385 | 482 | 460 | 7.9 GiB |
| dev 1M + 250K clicks (both tables), `make run-dev` | 1.07M + 252K | 117,038 + 27,400 | 93 ms | 24 | 7 | 26 | 1 | 4 | 6 | 6 | 0.4 GiB |

At 48M with clicks, click parse is 470–602 ms and click grouping is 67–142 ms.
| dev20m 20M events + 5M clicks, Docker 24g, before round 2 | 21.4M cand + 5M clicks | 2.21M + ~28K | 1.70–1.96 s (container 1.83–2.33) | 182–201 | 193–201 | 525–712 | 10–13 | 116–124 | 119–123 | 71–75 | 4.8 GiB |
| dev20m, after round 2 | same | same | **1.12–1.17 s (container 1.26 s)** | 209–216 | 182–185 | 240–255 | 10–14 | 114–119 | 112–118 | 49–56 | 4.1 GiB |

Worker threads are hard-capped at 8 (`CPU_BUDGET` in `main.c`); average busy cores measured 5.2–6.4. At 20M, kernel time (sys 6–10 s) exceeds user time (8.7–9.2 s): first-touch page faults on the large lazily committed buffers dominate. Pre-faulting per worker or reusing buffers across phases is the next candidate.

The dictionary phase (products + links DSU, single-threaded C) is 10–115 ms and grows with the number of links.

## Next measured candidates (not done yet)

1. **Exit teardown** is about 50 ms of wall time outside the program: the kernel frees about 3.6 GB of mappings on one CPU. Parallel `munmap` didn't help (TLB-shootdown IPIs). The lever is touching less memory: a smaller `Cand` (drop `eid_h32` and recompute it in dedup; pack `ts_off` and `ingest`).
2. **Dedup + collect** read the 1.2 GB Cand arena twice from DRAM. Fusing them per partition was slower, because the person and link tables got evicted. A smaller `Cand` cuts both passes.
3. **Link union-find** (worker 0, 113 ms CPU, IPC 0.27): the final scatter into the 38 MB link table and the random smallest-name reads. Prefetching or a parallel scatter is worth about 7 ms of wall time.
4. **`evx_parse_ts`** (7% of cycles): memoise the date part for consecutive rows of a file.
5. 512-bit vs 256-bit blocks in the scanner (Cascade Lake AVX-512 license), to be A/B timed on the scorer.

## Round 7 idle-spot fixes (branch r7-idle)

Measured with exact `wait4` timing, 20M native, interleaved.

- **C2 click prefix sum parallel** (`job_cprefix_sum` / `job_cprefix_write` in `main.c`): per-worker block sums and max, a serial pass over 8 block bases, then each worker writes its block's offsets and cursors. Median 625 -> 623 ms (about 2 ms, near the noise floor).
- **Parallel exit teardown** (`job_release`, region registry in `main.c`): `big_alloc`/`map_file` register every large mapping; after the last Parquet part, each worker `MADV_DONTNEED`s its 1/nw slice of every region before the process exits (read-lock only, so slices run in parallel, unlike `munmap` which is write-locked and was slower). Measured split of the old ~50 ms exit: events file mapping about 25 ms, Cand arena about 20 ms, click buffers about 4 ms. Median 635 -> 605 ms (about -30 ms); min 629 -> 600.
- **Rejected (measured, no gain):** coordinator joining the link tasks after loading products (median 617 -> 617 ms). Parquet tail: instrumented per-worker timings show P7 is about 22 ms with all parts finishing within 3 ms of each other (already split by row count); the "under-used" samples in the perf timeline are kernel `write` time, which user-only perf sampling does not see. Startup: manifest and pool creation are under 0.3 ms; the 2.5 ms serial link plan and about 28 ms link parse (products load finishes at 14 ms) are fault-bound. 1 GB first-touch costs 80 ms on 8 threads whether by faulting or `MADV_POPULATE_WRITE` (zeroing/memcg, not exception entry), so prefaulting cannot help.

## Vector-width study (branches vec-512 / vec-256 / vec-128 / vec-64)

Four builds of round 7 that differ only in SIMD width, to measure whether AVX-512
frequency licensing costs anything on the target Cascade Lake host. All produce
byte-identical output. This branch, **vec-128**: every zmm/ymm block is a 16-byte
xmm16+ block (EVEX AVX-512VL/BW, 16-bit k-masks, masked tail loads kept). The
F1/F2 timestamp parse runs as two xmm halves (plain 16-byte load of bytes 0..15,
masked 3-byte load of 16..18; vpshufb/vpmaddubsw are lane-local, so each half uses
its lane of the old constants). The 32-byte ClickCand copy is two xmm moves.
C is built with -mprefer-vector-width=128.
