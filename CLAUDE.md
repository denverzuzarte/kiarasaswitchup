# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

This is a native C + x86-64 GNU-assembly solution for the "Shopping Sessions" buildathon track. It implements **`events_only_c_assembly_rewrite_spec.md`**: events, links, products, sessionization, and **only** the `user_daily` Parquet output. Clicks, campaigns and `campaign_daily` are intentionally not implemented, so the official scorer and `make fixtures`/`make check-dev` (which require both tables) will fail until a click pipeline is added.

- `The rules.md` is the authoritative rule set. `fixtures/README.md` lists what each fixture catches.
- `docs/SPEC_REVIEW.md` lists where the spec conflicts with the rules or the build host. Notably, there is **no DuckDB**, so Parquet is written by `src/parquet.c`.
- `docs/IMPLEMENTATION_PLAN.md` covers the phases and the test procedure. `docs/ASM_OPTIMIZATIONS.md` records every asm-level optimisation and its measurements. Keep it updated when you change the asm.

## Commands

```sh
make                      # static src/main (gcc, -O3 -march=cascadelake, -static -no-pie)
make unit                 # asm primitive unit tests (src/tests/unit.c)
make check-fixtures PY=<python with polars>   # all 36 fixtures, user_daily only, no Docker
make build                # docker image scratch_build:latest (FROM scratch, just /main)
$PY scripts/check_events.py fixtures -- scripts/docker_run.sh     # fixtures through the image
scripts/docker_run.sh dev out && (cd scripts && $PY check_events.py --compare ../out ../../dev-answer)
DATA_DIR=fixtures/E16/input OUTPUT_DIR=/tmp/o src/main            # one case by hand
```

Differential testing against an independent Python implementation of the rules:

```sh
$PY scripts/fuzz_events.py /tmp/fz 3 200000           # adversarial dataset (block edges, long windows, dirt)
mkdir -p /tmp/ref && $PY scripts/reference_events.py /tmp/fz /tmp/ref/user_daily.csv
DATA_DIR=/tmp/fz OUTPUT_DIR=/tmp/out src/main && (cd scripts && $PY check_events.py --compare /tmp/out /tmp/ref)
generate --seed N --events E --clicks C --output DIR   # real generator, on PATH
```

polars is not installed system-wide; use a venv. `check_events.py` must run from `scripts/` when using `--compare` (it imports `run_fixtures`).

## Architecture

The C coordinator lives in `main.c` (with `dict.c` and `parquet.c`). It runs a pthread pool of `NCPU` workers and phase barriers (`pool_run`). Every event semantic is in assembly:

| Phase | Code | Work |
|---|---|---|
| P0 | `dict.c` | manifest window, products → category IDs **in byte order** (the tie-break is the smallest ID), links → DSU → `LookupEntry` table raw ID → canonical (smallest) name |
| P1 | `evx_parse_events` | newline-aligned byte ranges → 64-byte `Cand` per structurally valid row with a non-empty `event_id`; **all validation is computed here as a flag**, but the row still competes in dedup |
| P2 | `evx_dedup_insert` | lock-free global CAS table of `Cand*` (16-bit tag in the top bits); the smallest `ingest` wins |
| P3 | `evx_collect` | walk winners, drop invalid ones, intern the canonical name in the person CAS table (slot = person ID), `lock inc` counts |
| P4 | `main.c` | compact slots → dense person IDs (`counts[]` reused as the remap), prefix-sum offsets, extract names, free big buffers |
| P5 | `evx_scatter` | `lock xadd` cursors → each person's events contiguous (`Ev`, 16 B) |
| P6 | `evx_sessionize` | per-person sort (insertion ≤ 32, else LSD radix), 30-min gaps, start-day rows, top category |
| P7 | `parquet.c` | `$OUTPUT_DIR/user_daily/part-00000.parquet` (PLAIN, uncompressed; `day` = INT32 DATE) |

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
- The program runs as uid 1000 with a read-only root. It reads `DATA_DIR`, writes only `OUTPUT_DIR/user_daily/`, and calls `umask(0)`.
- Nothing may depend on generator seed specifics. The window length, file counts and whales all vary, and the radix pass count comes from the window span.
- `src/main` is a committed build artifact (`.gitignore` doesn't cover it).
