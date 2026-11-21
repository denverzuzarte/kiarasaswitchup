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

Worker threads are hard-capped at 8 (`CPU_BUDGET` in `main.c`); average busy cores measured 5.2–6.4. At 20M, kernel time (sys 6–10 s) exceeds user time (8.7–9.2 s): first-touch page faults on the large lazily committed buffers dominate. Pre-faulting per worker or reusing buffers across phases is the next candidate.

The dictionary phase (products + links DSU, single-threaded C) is 10–115 ms and grows with the number of links.

## Next measured candidates (not done yet)

1. **Collect** is now the largest phase at scale. The person table is sized for the worst case (all events distinct people, about 3× candidates), while the real person count is about E/30, so its random probes miss cache. Options: two-stage software prefetch (candidate line, then person slot), or a small table first with a fall-back to the worst-case table when an occupancy counter crosses 50%.
2. **Prefix** (P4) is a sequential C scan of `psize` slots. It could be parallelised per slot block, with a two-level prefix sum.
3. **Links/DSU** (P0) is single-threaded. Link files could be parsed in parallel into per-file pair arrays before the union pass.
4. 512-bit vs 256-bit blocks in the scanner. 512-bit integer ops can lower the Cascade Lake turbo license. A `ymm` variant (still with `k`-mask compares via AVX-512VL) should be A/B timed on the scorer.
