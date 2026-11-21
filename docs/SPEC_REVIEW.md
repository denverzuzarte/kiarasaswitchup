# Spec review: `events_only_c_assembly_rewrite_spec.md` vs `The rules.md`

I checked every rule in `The rules.md` that touches events, links, products or `user_daily` against the spec. Each item below is either **OK** (the spec matches the rules), **GAP** (the spec is silent or loose and I chose a rules-faithful reading), or **CONFLICT** (the spec cannot be followed as written, or following it costs points).

## Conflicts / blockers

1. **DuckDB C API (spec §2.3, §11): BLOCKER, worked around.** There is no `libduckdb` / `duckdb.h` on the build host. The spec also contradicts itself here. DuckDB is a C++ library, so linking it statically needs `libstdc++`, and §3.3 forbids C++ runtime dependencies. §2.3 forbids switching to C++, Python, a dynamically linked DuckDB, or an *unrelated output format*. I did none of those. Output is still Parquet, produced by a small writer in C (`src/parquet.c`): PLAIN encoding, uncompressed, Thrift-compact footer. The DuckDB dependency is gone, and no "final aggregation" is delegated to it. Aggregation happens in the assembly engine, which §3.2 wants anyway.
2. **RESOLVED by `clicks_c_assembly_spec.md`. Events-only scope (spec §1, §11) vs the scorer (rules → Outputs).** The rules require **both** `/output/user_daily/` and `/output/campaign_daily/`. The spec forbids creating `campaign_daily`, so the scorer will mark this build incorrect until a click pipeline exists. The supplied harnesses (`scripts/run_fixtures.py`, `scripts/compare.py`, `make fixtures`, `make check-dev`) also fail on the missing table. I added `scripts/check_events.py`, which checks only `user_daily`. I followed the spec as requested; this item is for awareness.
3. **"One process, eight workers" (spec §4.1).** This is implemented as a pthread pool sized from `NCPU` (default: online CPUs, capped at 64). The spec's "8" is the scorer's value, not a constant.

## Gaps resolved toward the rules

4. **F3 range (spec §6.3).** "Too large for signed 64-bit" must be judged by **value**, not digit count. Leading zeros are unlimited (`0001791071890000` is valid). The parser skips leading zeros and then does overflow-checked accumulation. Anything that overflows, or is out of the window, is a bad `ts` *after* dedup, never a malformed row.
5. **Timestamp offset as `ts - window_start` (spec §8).** The rules don't bound the window length (only the generator uses 14 days). Offsets are stored as 64-bit values, and the radix sort picks its number of byte passes from the actual window span.
6. **16-bit product ordinal / 8-bit category (spec §5.2).** Products are looked up by an exact-bytes hash, so there's no ordinal to overflow. Category IDs are 8-bit and bounds-checked: more than 255 categories (counting `unknown`) is a hard error, never silent truncation.
7. **Header row.** The rules say every CSV has a header row. The spec only mentions header-only files. The first line of *every* file is skipped unconditionally, even when it looks like data or isn't the real header (L3 has a data row equal to the header text, and it must be treated as data).
8. **`ingest_ts` always valid F1.** It goes through the same F1/F2/F3 parser for defence. If it is somehow unparseable, the event is marked invalid rather than crashing.
9. **CSV quote rule.** "A quote anywhere except around a whole field is malformed" is implemented exactly. A field is *wrapped* iff its length is ≥ 2 and its first and last bytes are `"`. A line is valid iff its total quote count equals 2 × (number of wrapped fields). That rejects `"`, `a"`, `"a"b"`, `"""`, and unbalanced quotes across fields.
10. **Price parsing.** Purchase prices accept unlimited leading zeros (`0250` = 250), ASCII digits only, value 1..10,000,000. `view`/`cart` ignore `price_cents` completely, even `\r` or non-ASCII digits.
11. **Category tie-break.** Category IDs are assigned in byte-wise order of the category names, with `unknown` inserted at its sorted position. "Byte-wise smallest name wins a tie" then becomes "smallest ID wins".

## Items that match the rules (OK)

- Dedup is global, by exact `event_id`, before validation. The smallest `ingest_ts` wins, and equal ingest times keep either copy (§7). Malformed rows and empty `event_id` are dropped before dedup.
- Validation of the kept copy: non-empty user, valid and in-window `ts` (`start <= ts < end`), exact-case type, non-empty product, purchase price rule. Then the late rule: drop iff `ingest - ts > 86_400_000`.
- Links: drop malformed lines and empty sides. DSU, transitive across files. The name is the byte-wise smallest ID, including IDs that appear only in links. Self and repeated links are harmless.
- Sessions: a gap `> 1_800_000` ms splits. Equal timestamps share a session. Each session counts on its start day (UTC).
- `user_daily`: one row per person/day, `revenue_cents` = 0 if no purchases, `top_category` is null with no purchases, and `unknown` competes.
- Strings are never normalised: spaces and case are significant, and `\r` is data.
- `day` is a Parquet `DATE` (INT32, days since epoch). `top_category` is an OPTIONAL byte array.

## Clicks spec (`clicks_c_assembly_spec.md`) vs the rules

- **OK:** click window `[window_start - 7 d, window_end)`; credit = latest click with `click.ts <= start` and `start - click.ts <= 7 d`, ties to the byte-wise smallest `click_id`; `organic` when there's none; assists = every campaign with a qualifying click, at most once per session, never `organic`; per-day counts on the session start day; assisted-only rows carry 0 in the credited columns.
- **OK (§8, no click dedup):** the rules guarantee that copies of a click are identical. Validation, person, credit maximum and assist set are therefore unchanged by duplicates. The Python reference does dedup for real and agrees on every dataset tested.
- **Deviation (§7.2 PIndex):** no separate 32 MiB index is built. After the memory work, the events person table is itself compact (about 1/8 of the candidates, 16-byte slots, 128 MiB at 48M) and already maps canonical name → slot, with `counts[]` reused as slot → dense ID. It's kept alive through the click parse and freed straight after. This is the same probe pattern with one structure fewer.
- **Deviation (§7.2 note):** "the person table is 2^27 slots" no longer applies; see above.
- Attribution is in assembly (`evx_attribute`), so the C contingency of §17 was not needed.
