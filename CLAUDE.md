# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is a native C + x86-64 GNU-assembly solution for the "Shopping Sessions" buildathon track. It implements **`events_only_c_assembly_rewrite_spec.md`** (events, links, products, sessions → `user_daily`) and **`clicks_c_assembly_spec.md`** (clicks, campaigns, credit and assists → `campaign_daily`). Both tables are written, and the official `make fixtures` and `make check-dev` pass.

- `The rules.md` is the authoritative rule set. `fixtures/README.md` lists what each fixture catches.
- `docs/SPEC_REVIEW.md` lists where the spec conflicts with the rules or the build host. Notably, there is **no DuckDB**, so Parquet is written by `src/parquet.c`.
- `docs/IMPLEMENTATION_PLAN.md` covers the phases and the test procedure. `docs/ASM_OPTIMIZATIONS.md` records every asm-level optimisation and its measurements. Keep it updated when you change the asm.

## Commands

```sh
Launch, build and clean only through the Makefile. `PY` must be a Python with polars (use a venv).

```sh
make / make clean             # static src/main (gcc, -O3 -march=cascadelake, -static -no-pie)
make unit                     # asm primitive unit tests (src/tests/unit.c)
make fixtures-native PY=$PY   # official harness, both tables, native binary
make fixtures PY=$PY          # official harness through the Docker image
make run-dev [DEV=dir] [RUN_MEM=24g]   # Docker run, 8 CPUs, timed
make run-native [DEV=dir] [CPUS=n]     # native run into out/
make check-dev ANSWER=../dev-answer PY=$PY   # both tables vs the official answer
make dev DEV=dir SEED=n EVENTS=e CLICKS=c    # generate a dataset
make fuzz-data DEV=fz SEED=n EVENTS=e PY=$PY # adversarial dataset
make check-reference DEV=dir PY=$PY          # compare out/ with the Python reference
make bench DEV=dev20m REVS="main HEAD" RUNS=5   # interleaved wall time of committed binaries
make variant NAME=x VDEFS=.. VASDEFS=..      # alternative build into build/x/main for A/B tests
make clean-out / make clean-dataset DEV=dir
```
```

`scripts/reference_events.py` is an independent Python implementation of the whole rule set, including real click dedup. Use `make check-reference` with it for differential testing. `EVX_PSIZE=<n>` forces a small person table to exercise the overflow/retry path (reruns P1.5 only); `EVX_RSIZE=<n>` forces a small raw-ID table (reruns P1). There is no in-binary timing or profiling: measure with external wall time and `perf`.

## Architecture

The C coordinator lives in `main.c` (with `dict.c` and `parquet.c`). It runs a pthread pool of `NCPU` workers and phase barriers (`pool_run`). Every event semantic is in assembly:

| Phase | Code | Work |
|---|---|---|
| P0 | `dict.c` | manifest window; link files parsed by all workers (`job_links`) **while the coordinator loads products → category IDs in byte order** (the tie-break is the smallest ID) and campaigns; DSU → `LookupEntry` raw ID → canonical (smallest) name runs on worker 0 during P1 |
| P1 | `evx_parse_events` | newline-aligned byte ranges → 32-byte `Rec` per structurally valid row with a non-empty `event_id` (`ki` = ingest seconds + 24 hash bits, `eptr` = event_id pointer + 16 hash bits, `ts_off`, `rid`, `price_cat`), written **straight into its dedup partition** (`(u32)hash >> pshift`, ≥ 64K rows each, 16–1024): the worker's open 4 KB chunk (32 B header + 127 Recs). Chunks come from per-worker 2 MB slabs (`evx_chunk_alloc`, C). **All validation is computed here**: an invalid copy gets `rid = ~0` and still competes in dedup. A valid copy interns its raw `user_id` in the **raw-ID table** (`cmpxchg16b`, slot = rid, probed through a 16-row prefetch ring; claims counted in batches of 64; >50% load → rerun P1 ×8, `EVX_RSIZE` forces it). Branchless quote unwrap |
| P1.5 | `evx_resolve_raw`, `main.c` `job_compact`, `job_rremap` | once per **distinct** raw id, batched: `evx_canon` (links; the DSU finished during P1) → person intern (table `pow2(2 × raw claims)`, so only `EVX_PSIZE` makes P1.5 rerun) → dense person IDs from person-table occupancy (slot order; `counts[]` = slot → dense) → `remap[raw slot]` = dense id, in place; names |
| P2 | `evx_dedup_emit` | per partition, private L2 table of tagged Rec pointers, **no atomics**; smallest `ki` (ingest) wins, ties keep the resident copy; equal 40-bit hashes confirmed on the event_id bytes (`evx_eid_eq`). Then a table walk emits each valid winner as a Win `{ts_off, dense pid, price_cat}` into the worker's chunk list of its **person range** (`pid >> dshift`, ≤ 256 ranges); Win chunks are the previous partition's consumed Rec chunks |
| C1 | `evx_parse_clicks` | parse + validate + drop (no click dedup: copies are identical); `evx_canon` → person-table probe → dense ID; `lock inc` click counts |
| C2/C3 | `main.c`, `evx_scatter_clicks` | click prefix sums; clicks grouped by person (`ClickEv`, Ev-shaped) |
| P5+P6 | `job_session`: `evx_count_range`, `evx_group_range`, `evx_sessionize`, `evx_attribute` | per person range: count events per person, prefix into `offsets`, place the range's Wins into a per-worker L2-resident scratch (**no global `Ev` array**), then sessions + `user_daily` rows, then attribution (sort clicks, two-pointer 7-day window, credit latest, tie → `evx_memcmp`, lazy assist flush; per-worker campaign × day tables) |
| P7 | `parquet.c`, `job_write` | `user_daily`: **one part file per worker, in parallel** (sessions/events INT32); `campaign_daily` reduced from per-worker tables. The binary prints nothing on success |

**`src/evx.h` is the ABI contract.** Struct offsets used by the `.s` files are pinned with `_Static_assert`; change both sides together.

Asm conventions:
- Intel syntax.
- Only `zmm16`–`zmm31` are used (no `vzeroupper` needed).
- Tail reads are fault-suppressing masked loads, never over-reads.
- `evx_memeq` preserves all GPRs except `rax`/`r11`.
- Cascade Lake has no VBMI, so `vpermb` is unavailable.

## Constraints

- The binary must stay fully static (`file src/main`; `ldd` says "not a dynamic executable").
- Keep `-march=cascadelake`.
- The program runs as uid 1000 with a read-only root. It reads `DATA_DIR`, writes only `OUTPUT_DIR/{user_daily,campaign_daily}/`, and calls `umask(0)`.
- At most 8 worker threads (`CPU_BUDGET`).
- Nothing may depend on generator seed specifics. The window length, file counts and whales all vary, and the radix pass count comes from the window span.
- `src/main` is a committed build artifact (`.gitignore` doesn't cover it).
