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
make clean-out / make clean-dataset DEV=dir
```
```

`scripts/reference_events.py` is an independent Python implementation of the whole rule set, including real click dedup. Use `make check-reference` with it for differential testing. `EVX_PSIZE=<n>` forces a small person table to exercise the overflow/retry path.

## Architecture

The C coordinator lives in `main.c` (with `dict.c` and `parquet.c`). It runs a pthread pool of `NCPU` workers and phase barriers (`pool_run`). Every event semantic is in assembly:

| Phase | Code | Work |
|---|---|---|
| P0 | `dict.c` | manifest window, products → category IDs **in byte order** (the tie-break is the smallest ID), links → DSU → `LookupEntry` table raw ID → canonical (smallest) name |
| P1 | `evx_parse_events` | newline-aligned byte ranges → 56-byte `Cand` (dense, per-worker arena chunks) per structurally valid row with a non-empty `event_id`; **all validation is computed here as a flag**, but the row still competes in dedup |
| P2 | `evx_dedup_insert` | lock-free global CAS table of `Cand*` (16-bit tag in the top bits); the smallest `ingest` wins |
| P3 | `evx_collect` | walk winners, drop invalid ones, `evx_canon` (links), intern the name in the person table (16-byte `cmpxchg16b` slots, sized ~1/16 of candidates, rerun ×8 on >50% load), `lock inc` counts |
| P4 | `main.c` | compact slots → dense person IDs (`counts[]` reused as the remap), prefix-sum offsets, extract names, free big buffers |
| P5 | `evx_scatter` | `lock xadd` cursors → each person's events contiguous (`Ev`, 16 B) |
| C1 | `evx_parse_clicks` | parse + validate + drop (no click dedup: copies are identical); `evx_canon` → person-table probe → dense ID; `lock inc` click counts |
| C2/C3 | `main.c`, `evx_scatter_clicks` | click prefix sums; clicks grouped by person (`ClickEv`, Ev-shaped) |
| P6 | `evx_sessionize` then `evx_attribute` per block | sessions + `user_daily` rows; then re-derive sessions, sort clicks, two-pointer 7-day window, credit (latest, tie → `evx_memcmp`), lazy assist flush; per-worker campaign × day tables |
| P7 | `parquet.c` | `user_daily` built in RAM straight from the `Row` arrays: row groups = runs of session tasks, encoded on all workers, then the workers `pwrite` each group at its final offset. `campaign_daily` is reduced from per-worker tables and encoded the same way |

**`src/evx.h` is the ABI contract.** Struct offsets used by the `.s` files are pinned with `_Static_assert`; change both sides together.

Asm conventions:
- Intel syntax.
- Only `zmm16`–`zmm31` are used (no `vzeroupper` needed).
- Tail reads are fault-suppressing masked loads, never over-reads.
- `evx_memeq` preserves all GPRs except `rax`/`r11`.
- Cascade Lake has no VBMI, so `vpermb` is unavailable.

The binary prints nothing unless something fails (no stats, no profiler: the `dopt` branch removed them along with the partitioned-dedup and variant-switch code; `main` still has `EVX_PROFILE=1` / `make profile`).

## Constraints

- The binary must stay fully static (`file src/main`; `ldd` says "not a dynamic executable").
- Keep `-march=cascadelake`.
- The program runs as uid 1000 with a read-only root. It reads `DATA_DIR`, writes only `OUTPUT_DIR/{user_daily,campaign_daily}/`, and calls `umask(0)`.
- At most 8 worker threads (`CPU_BUDGET`).
- Nothing may depend on generator seed specifics. The window length, file counts and whales all vary, and the radix pass count comes from the window span.
- `src/main` is a committed build artifact (`.gitignore` doesn't cover it).
