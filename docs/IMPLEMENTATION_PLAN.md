# Implementation plan and test procedure (events-only rewrite)

These are my working instructions for building and verifying the rewrite, one phase at a time. Every phase ends with a concrete test that must pass before moving on.

## Source layout

| File | Language | Owns |
|---|---|---|
| `src/evx.h` | C header | The **ABI contract** between C and assembly: struct layouts and offsets. Every asm offset constant mirrors a `_Static_assert` here. |
| `src/evx_util.s` | asm | `evx_hash`, `evx_memeq`, `evx_split_line` (the CSV dialect), `evx_parse_ts` (F1/F2/F3) |
| `src/evx_parse.s` | asm | `evx_parse_events`: batch event parse → validation flags → lookups → candidate records |
| `src/evx_dedup.s` | asm | `evx_dedup_insert` (lock-free global winner table), `evx_collect` (valid winners + person interning + histogram), `evx_scatter` (group by person) |
| `src/evx_session.s` | asm | `evx_sessionize`: per-person ts sort, session split, day split, user/day aggregates, top category |
| `src/main.c` | C | env, phases, pthread pool, task queues (silent unless an error occurs) |
| `src/dict.c` | C | manifest, products + category ranking, links DSU → lookup tables |
| `src/parquet.c` | C | In-RAM Parquet builder (PLAIN, uncompressed, Thrift-compact footer); groups encoded and pwritten at their offsets in parallel |
| `src/tests/unit.c` | C | Unit tests of the asm primitives (`make unit`) |
| `scripts/check_events.py` | Python | Fixture and dev-set checker for `user_daily` only |

## Data flow

```
P0  C     manifest, products (cat ids in byte order), links → DSU → link LookupTable
P1  asm   evx_parse_events  per newline-aligned byte range  → Cand[] (64 B)   [parallel, dynamic queue]
P2  asm   evx_dedup_insert  per Cand sub-range → global CAS table of Cand*   [parallel]
P3  asm   evx_collect       per dedup-slot range → Win[] (16 B) + person CAS table + lock-inc counts [parallel]
P4  C     prefix sum of counts → offsets/cursors, max person size
P5  asm   evx_scatter       per Win range → Ev[] grouped by person (lock xadd cursor) [parallel]
P6  asm   evx_sessionize    per person-slot block → Row[] (32 B)              [parallel, dynamic queue]
P7  C     Parquet writer → $OUTPUT_DIR/user_daily/part-00000.parquet
```

Every arrow is a phase barrier (pool barrier). No structure is read in a phase while another phase can still write it.

## Phase-by-phase instructions

### Phase A: ABI header and asm primitives (`evx.h`, `evx_util.s`)
1. Write the struct layouts with `_Static_assert(offsetof(...))`.
2. Implement `evx_hash` (8-byte word multiply-xorshift with a masked tail load; murmur fmix64 final).
3. Implement `evx_memeq` (masked 64-byte compares).
4. Implement `evx_split_line` (AVX-512BW comma/quote masks; quote-count validity rule).
5. Implement `evx_parse_ts` (F1/F2 template check with SIMD; F3 scalar with overflow-checked accumulation; Hinnant days-from-civil).

**Test A:** `make unit` must pass. Cases: every timestamp example in the rules, every E17/E18 case, leap years 2028/2100/2000, `+24:00`, `+05:60`, `+0200`, `-00:00`, 21-digit F3, leading zeros, and CSV lines from E13/E15/E19/E20/L3.

### Phase B: dictionaries (`dict.c`)
1. Manifest: find `window_start` / `window_end`, parse them with `evx_parse_ts`.
2. Products: split with `evx_split_line`, collect distinct categories, add `unknown`, sort byte-wise, and assign IDs in sorted order (bounds ≤ 255). Build the product LookupTable (`val_len` = cat id).
3. Links: split each line; drop malformed lines and lines with an empty side. Intern IDs, then DSU (union by size, path halving), then the smallest name per root. Build the link LookupTable: key = raw ID, val = canonical name, `val_tag` = hash>>32.

**Test B:** run the fixtures (L1–L3 exercise this) through the full pipeline.

### Phase C: parse + dedup (`evx_parse.s`, `evx_dedup.s`)
1. `evx_parse_events` walks lines, rejects malformed lines and empty eid, computes all validity flags per row, and does lookups only for rows that are still valid.
2. `evx_dedup_insert`: open addressing over `Cand*` tagged with 16 hash bits. CAS insert, CAS replace on a smaller ingest.

**Test C:** E4, E5, E6, E10, E12, E13, E17, E18, E19, E20.

### Phase D: collect / scatter / sessionize
1. `evx_collect` interns the canonical name into the person table (slot index = person id) and lock-increments `counts[pid]`.
2. C does the prefix sum.
3. `evx_scatter` places each event at `lock xadd cursor[pid]`.
4. `evx_sessionize`: insertion sort (n ≤ 32) or LSD radix (one histogram pass for all digits), then session/day/category aggregation.

**Test D:** E1, E2, E3, E8, E9, E14, E16, L1, L2, L3.

### Phase E: Parquet (`parquet.c`)
Write PLAIN pages and the Thrift-compact FileMetaData. `day` is INT32 + DATE, and `top_category` is OPTIONAL with RLE definition levels.

**Test E:** polars reads the file and the schema matches exactly. This is covered by `scripts/check_events.py`.

### Phase F: end-to-end
1. `make && python scripts/check_events.py fixtures -- src/main`: every case's `user_daily` must match.
2. `make run-dev` (the Docker image, scorer flags), then `python scripts/check_events.py --compare out ../dev-answer`, which must print MATCH for `user_daily`.
3. Record the timings and peak RSS printed on stderr in `docs/ASM_OPTIMIZATIONS.md`.

## Test commands

```sh
make unit                                                     # asm primitive unit tests
make && $PY scripts/check_events.py fixtures -- src/main      # all fixtures, user_daily only, no Docker
DATA_DIR=fixtures/E16/input OUTPUT_DIR=/tmp/o src/main        # one fixture by hand
make build && $PY scripts/check_events.py fixtures -- scripts/docker_run.sh   # through the image
make run-dev && $PY scripts/check_events.py --compare out ../dev-answer      # 1M dev set
```

`$PY` is a Python with `polars` installed.

## Status / results (2026-11-22)

| Phase | Test | Result |
|---|---|---|
| A | `make unit` | 96/96 checks pass |
| B–E | `make check-fixtures` (native) | 36/36 fixtures, `user_daily` exact |
| E | Docker image, `check_events.py fixtures -- scripts/docker_run.sh` | 36/36 |
| F | dev set (seed 1, 1M events) vs `../dev-answer`, native and Docker | MATCH, 117,038 rows |
| F | `generate` seeds 2, 3, 4 (1M) and 11 (3M) vs `scripts/reference_events.py` | MATCH |
| F | `scripts/fuzz_events.py` seeds 1–8 (block-edge lines, >2^32 ms windows, non-midnight windows, no trailing newline, empty files, dirt) vs reference | MATCH |
| F | `NCPU` = 1, 2, 3, 8, 16 on dev | identical (MATCH) |
| F | 10M events (seed 12), Docker | 1.04–1.10 s, 2.57 GiB peak RSS; output internally consistent |

Errors found and fixed during testing:
- The fuzzer itself first produced equal-`ingest_ts` copies with different users, which the rules exclude. It was fixed to use distinct delays; the engine was not at fault.
- Memory: per-person arrays were sized by hash-table slots (about 3× candidates). They are now compacted to dense person IDs, and buffers are freed per phase (about 1 GB saved at 20M events).

The reference implementation was itself validated against `../dev-answer` (MATCH) before being used as an oracle.

## Clicks (clicks_c_assembly_spec.md), 2026-11-22

| Stage | Test | Result |
|---|---|---|
| S1–S2 | `make unit` (adds `evx_memcmp` cases) | 110/110 |
| S3 | `make fixtures-native` and `make fixtures` (official harness, Docker, both tables) | 36/36 both |
| S3 | `make run-dev` then `make check-dev ANSWER=../dev-answer` | MATCH, both tables (117,038 + 27,400 rows) |
| S4 | generator seeds 2, 3, 4 (1M) and 11 (3M) vs full reference (`make check-reference`) | MATCH |
| S4 | `make fuzz-data` seeds 1–8 with click edge cases vs reference | MATCH |
| S4 | `CPUS` = 1, 3, 8 | identical (MATCH) |
| S4 | 48M events + 12M clicks, Docker, 8 CPU / 24 GB | 5.13 s warm, 7.06 s cold, 8.2 GiB peak, invariants OK |

Errors found during testing:
- The Parquet footer still had a hard-coded 6-column list (it broke the 7-column table); fixed.
- The fuzzer generated one `click_id` for different clicks, which the rules exclude; fixed. The engine was not at fault.
