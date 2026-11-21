# Clicks and Attribution Specification: C Orchestrator + x86-64 Assembly Click Engine

**Status:** High-level implementation specification, companion to
`events_only_c_assembly_rewrite_spec.md`\
**Scope:** Click ingestion, campaigns, session attribution (credit and
assists), and the `campaign_daily` output\
**Target:** Linux `amd64`, Intel Xeon Cascade Lake, 8 CPUs, 24 GB
memory\
**Languages:** C for orchestration and lookup structures; GNU x86-64
assembly for the click hot path and attribution\
**Output:** `/output/campaign_daily/part-00000.parquet`, added next to the
existing `/output/user_daily/part-00000.parquet`\
**Runtime image:** Unchanged: the same static executable in the same
`FROM scratch` image\
**Deadline:** 22 November 2026, 21:30 IST

This document extends the events engine that is already built and
verified (36/36 fixtures on `user_daily`, dev set MATCH, 10M and 20M
measured). It does not redesign the events side. Where this document
touches events code, it says so explicitly and keeps the verified path
unchanged wherever possible.

The coding agent owns instruction selection, register allocation and
exact struct packing. This document defines semantics, phase placement,
data layout at the level of record shapes and sizes, concurrency,
memory budget and optimisation order.

------------------------------------------------------------------------

## 0. Why this is the critical path now

The rules require **both** output tables. The current build writes only
`user_daily`, so the scorer marks it incorrect regardless of speed (see
`SPEC_REVIEW.md` item 2). Until `campaign_daily` exists and is exact, the
events-side performance work earns nothing.

Consequences for planning:

1.  Correctness of `campaign_daily` is the only priority until the
    official `make fixtures` (both tables) and `make check-dev` pass.
2.  Changes to verified events code are kept to the minimum: one new
    call in the sessionization job, two extra fields in `pnames`, and
    the stats/output plumbing in `main.c`. `evx_sessionize` is not
    modified (see §9.1 and D4).
3.  A time-boxed contingency exists for the attribution kernel (§17).

------------------------------------------------------------------------

## 1. Scope

### Included

1.  Read `campaigns.csv` and build the campaign dictionary (ID → dense
    campaign ID, channel).
2.  Read every `clicks/*.csv` part file.
3.  Strictly parse click rows and timestamps with the same CSV dialect
    and timestamp rules as events.
4.  Validate clicks (user, timestamp window, known campaign).
5.  Resolve each click to the canonical person, using the existing link
    DSU, then to the dense person ID used by the events engine.
6.  Group clicks by person and order them by timestamp.
7.  For every session produced by the events engine, choose the credited
    campaign (or `organic`) and the set of assisting campaigns.
8.  Aggregate per `(campaign_id, day)`: credited sessions, purchasing
    sessions, revenue, assisted sessions.
9.  Write `campaign_daily` as one Parquet file.

### Superseded statements in the events spec

The events spec's §1 "Explicitly excluded" list, its §11 "Do not create
`/output/campaign_daily/`", and the matching lines in `CLAUDE.md` no
longer apply. `scripts/check_events.py` remains useful, but the official
harnesses (`make fixtures`, `make check-dev`) become the primary checks
again because both tables now exist.

### Unchanged

Event parsing, event dedup, validation, late filtering, person grouping,
sessionization, `user_daily` aggregation and its Parquet output stay as
implemented and verified.

------------------------------------------------------------------------

## 2. Build and runtime contract

Unchanged from the events spec §2, with these additions:

-   New sources follow the existing layout: `src/*.c`, `src/*.s`. No
    Makefile change is needed because the Makefile globs both.
-   No new libraries. DuckDB remains unavailable on the build host
    (`SPEC_REVIEW.md` item 1). `campaign_daily` is written by
    `src/parquet.c`.
-   The program writes only `OUTPUT_DIR/user_daily/` and
    `OUTPUT_DIR/campaign_daily/`.
-   `/tmp` is 64 MB and must not be used for anything sized by the data.
    All buffers are anonymous mappings, as now.

------------------------------------------------------------------------

## 3. Language and responsibility split

### 3.1 C

-   Load `campaigns.csv` into a `LookupEntry` table (exact bytes → dense
    campaign ID) and a channel `Slice` table (`dict.c`).
-   Build the compact person index `PIndex` (canonical name → dense
    person ID) from `pnames` after events P4 (§7.2).
-   Plan click byte-range tasks, allocate click buffers, run the pool
    phases, compute click prefix sums.
-   Allocate per-worker attribution state; reduce per-worker aggregate
    tables after the last phase.
-   Write `campaign_daily` Parquet; extend diagnostics.

### 3.2 Assembly

-   `evx_parse_clicks`: batch parse, validation, campaign lookup,
    canonical-person resolution, `PIndex` probe, per-person count
    histogram.
-   `evx_scatter_clicks`: counting-sort placement of clicks grouped by
    person.
-   `evx_attribute`: per person, re-derive session boundaries from the
    sorted events, sort that person's clicks, and compute credit,
    assists and campaign/day aggregates.
-   `evx_memcmp`: byte-wise ordering primitive (unsigned bytes, shorter
    prefix sorts first) for the `click_id` tie-break.

The batch-oriented ABI rule from the events spec applies: one C→asm call
per task (byte range, slot range, or person block), never per row or
field.

### 3.3 No C++

Unchanged.

------------------------------------------------------------------------

## 4. Phase placement

The click pipeline slots into the existing phase graph. New phases are
marked ★.

```
P0   C    manifest, products, links DSU, ★campaigns
P1   asm  evx_parse_events          (unchanged)
P2   asm  evx_dedup_insert          (unchanged)
P3   asm  evx_collect               (unchanged)
P4   C    dense persons, prefix sums; ★pnames carry the name tag
P5   asm  evx_scatter               (unchanged)
★PI  C    build PIndex from pnames                        [parallel CAS]
★C1  asm  evx_parse_clicks  → ClickCand[], lock-inc ccount[pid]   [parallel, dynamic queue]
★C2  C    prefix sums of ccount → coffsets, max clicks per person
★C3  asm  evx_scatter_clicks → ClickEv[] grouped by person         [parallel]
P6   asm  per block: evx_sessionize (unchanged) then ★evx_attribute  [parallel, dynamic queue]
P7   C    user_daily Parquet (unchanged), ★reduce + campaign_daily Parquet
```

Dependencies that force this order:

-   C1 needs the link lookup (P0) and `PIndex`, which needs final dense
    person IDs (P4).
-   Clicks of people with no surviving events can never credit or
    assist anything, so C1 drops them. That requires knowing who has
    surviving events (end of P3/P4).
-   `evx_attribute` needs each person's events sorted, which
    `evx_sessionize` does in place (verified: the radix result is copied
    back into `events`), and needs clicks grouped (C3).

Placing C1–C3 after P5 also keeps the memory peak where it is now. The
candidate store, dedup table and person table are already freed by then
(§13).

PI, C1 and C3 are separate barriers in the baseline. Merging PI with P5
into one pool phase (two queues drained in order by the same job) is a
measured optimisation, not a baseline requirement (§15).

------------------------------------------------------------------------

## 5. Metadata

### 5.1 Click window

From the manifest (already parsed):

-   `cw_start = window_start - 604,800,000`
-   `cw_end = window_end`
-   A click timestamp is in-window iff `cw_start <= ts < cw_end`.

Exactly `window_start - 7 days` is in; `window_end` is out.

### 5.2 Campaigns

`campaigns.csv`: `campaign_id,channel`, clean, `campaign_id` unique.
Parse it with `evx_split_line` anyway (quotes are syntax), and skip the
first line unconditionally, as for every CSV.

-   Key: exact `campaign_id` bytes. No trimming or case folding. Case
    variants in clicks are unknown campaigns.
-   Dense ID `0 .. ncamp-1` in file order. Order does not matter: no
    campaign tie-break exists in the rules.
-   `organic` gets ID `ncamp`. It is never a key in the lookup table, so
    a click can never resolve to it.
-   Channels are kept as `Slice`s into the mapped file.
-   Bound check: `ncamp + 1` must fit the chosen ID width. The generator
    has 2,000 campaigns and 5 channels, but nothing is hard-coded.

Table size at 2,000 campaigns: 4,096 slots × 32 B = 128 KiB. That is
L2-resident, so every click lookup is a cache hit after warm-up.

------------------------------------------------------------------------

## 6. Strict click input semantics

### 6.1 Input files

Every `clicks/*.csv` file, including header-only files. Columns:

`click_id,user_id,campaign_id,ts`

The first line of every file is skipped unconditionally, even if it
looks like data (`SPEC_REVIEW.md` item 7).

### 6.2 CSV dialect

Identical to events: LF ends a line, CR is data, whole-field quotes are
syntax, no embedded comma/quote/newline, empty means null, wrong field
count is malformed, a stray quote is malformed, an empty line is
malformed, spaces are significant, digits are ASCII only.

Reuse the events scanner's block classification and quote rule
(ASM_OPTIMIZATIONS §2). Field count is 4 instead of 7.

### 6.3 Timestamps

Identical forms and rejection rules as events (F1, F2, F3; `evx_parse_ts`).
A `ts` ending in `\r` is a bad `ts`.

### 6.4 Every failure is a drop

For clicks, unlike events, there is no reason to distinguish "malformed
before dedup" from "invalid after dedup". Section 8 shows dedup is a
no-op, so every failing row is simply dropped at parse time:

| Check (cheap first)                              | Failure       |
| ------------------------------------------------ | ------------- |
| Line splits into exactly 4 fields, quote rule OK | drop          |
| `click_id` non-empty                             | drop          |
| `user_id` non-empty                              | drop          |
| `campaign_id` found in the campaign table        | drop          |
| `ts` parses (F1/F2/F3) and is in the click window | drop          |
| Canonical person has surviving events (`PIndex` hit) | drop (exact: no sessions to credit) |

F3 values too long for 64 bits are out of window, so they are dropped.
That is the same outcome as malformed, and for clicks the distinction
has no observable effect.

Order the checks so that rows that fail never pay for the two random
memory probes (link table, `PIndex`). Those probes are the dominant
per-row cost (§14).

------------------------------------------------------------------------

## 7. Identity resolution for clicks

### 7.1 Canonical name

Use exactly the canonicalisation sequence that `evx_parse_events` uses:
hash the raw `user_id`, probe the link `LookupEntry` table, and either
take `val` / `val_tag` (linked ID) or the raw ID and its own tag
(unlinked ID). Factor that sequence into a shared internal routine
rather than writing a second copy. Clicks and events must agree on the
canonical name and its 32-bit tag bit for bit, or a person's clicks will
miss their sessions.

### 7.2 `PIndex`: canonical name → dense person ID

The events P3 person table is sized for the worst case (≈ 2 × candidates
slots; 2^27 slots ≈ 1 GiB at 48M events) and is freed in P4. Do not keep
it for clicks. Build a compact index instead:

-   In P4, store the person's name tag in the unused `pad` field of
    `pnames[k]` (`persons[p]->pname_tag`). This is a one-line change to
    `main.c`.
-   `PIndex` has `2^ceil(log2(2 × npersons))` slots of `u64`:
    `(tag32 << 32) | (pid + 1)`, with `0` meaning empty. Index =
    `tag & mask`, linear probing.
-   Build it in parallel: workers take dense-ID ranges and CAS-claim
    slots. Names are unique per dense ID, so inserts never compare names.
-   Probe: match `tag32` from the slot alone, then confirm with
    `evx_memeq` against `pnames[pid]`. With about 4M slots, about 10 tag
    bits above the index discriminate, so name compares on a tag false
    positive are rare.

Size at 48M events: about 1.6M people (E/30 before link merges, fewer
after) → 4,194,304 slots × 8 B = **32 MiB**. That is 32× smaller than
the retained person table, and with `MADV_HUGEPAGE` it is 16 huge pages,
so TLB misses effectively disappear.

------------------------------------------------------------------------

## 8. Click deduplication: proven unnecessary

The rules say copies of a click have identical values ("keep any one").
Under that guarantee, global dedup has **no observable effect**:

-   Validation is a function of the row's values, so all copies pass or
    fail together.
-   All copies carry the same `user_id`, so they land in the same
    person.
-   Credit takes the maximum by `(ts, then smallest click_id)` over the
    person's qualifying clicks. Adding an identical copy does not change
    the maximum.
-   Assists are a *set* of campaigns per session. A repeated element
    does not change the set.
-   The window counters in §9.3 test `count > 0`. Duplicates change
    counts, never the `> 0` predicate.

Therefore the baseline **does not build a global click-ID table**. This
removes a 12M-key concurrent hash table (2^25 slots × 8 B = 256 MiB of
random CAS traffic, plus the probes), which would be the single most
expensive click structure.

Risk and mitigation: this relies on the rules' guarantee, not on the
generator. If a hidden input broke it (same `click_id`, different
values), "keep any one" would itself be ambiguous, so a correct expected
output could not depend on which copy survives. The Python reference
used for differential testing (§16) performs real dedup. Any divergence
shows up there.

------------------------------------------------------------------------

## 9. Attribution

### 9.1 Where it runs

`evx_attribute` runs in P6, in the same job as `evx_sessionize`, on the
same person block, immediately after it:

```
job_session: for each block from the dynamic queue:
    evx_sessionize(ctx, block)      # unchanged; sorts each person's Ev in place
    evx_attribute(actx, block)      # reads the now-sorted Ev while still in L2
```

Blocks hold at most 16,384 events (256 KiB of `Ev`), so for ordinary
blocks the second read hits L2. Only whale blocks re-read from L3/DRAM.

`evx_attribute` re-derives session boundaries with the identical rule
(`ts_off - prev > 1,800,000` opens a session; the day is
`floor((window_start + start_off) / 86,400,000)`). The duplicated rule is
guarded on every run by the exact self-check
`Σ user_daily.sessions == Σ campaign_daily.sessions` and the equivalent
for revenue (§16.3).

### 9.2 Coordinates

-   Session start: `s = ts_off` (event frame, offset from `window_start`).
-   Click key: `k = click.ts - cw_start` (click frame, ≥ 0 by validation).
-   Because `cw_start = window_start - W7` with `W7 = 604,800,000`, the
    rule `T - W7 <= click.ts <= T` becomes:

    **`s <= k <= s + W7`**

    This has no subtraction and no signed arithmetic. The lower bound is
    inclusive (exactly 7 days counts) and the upper bound is inclusive
    (a click at the start counts; 1 ms after does not).

Store `k` as `u64`. The radix pass count for clicks comes from the click
span `window_end - cw_start - 1`, computed like the events
`radix_passes`. It is a separate value from the events pass count.

### 9.3 Per-person algorithm

Inputs for person `p`: sorted events `Ev[off[p] .. off[p+1])`; clicks
`ClickEv[coff[p] .. coff[p+1])`, sorted by `k` inside `evx_attribute`
(insertion sort ≤ 32, else LSD radix; `ClickEv` has the same 16-byte
shape as `Ev`, so the existing sort code can be reused or copied as is).

Pass 1, the session scan (sequential over the sorted events), writes one
`SessRec` per session into per-worker scratch: `start s`, `day_rel`
(day − day0), `revenue` (u64; the sum of the 24-bit price field).
`has_purchase ⇔ revenue > 0`, because a valid purchase is priced at
least 1. It also fills `day_first[d]` = index of the first session with
`day_rel >= d`.

Pass 2, attribution. Per-worker arrays `cnt[ncamp]` (u32) and
`enter[ncamp]` (u32) are zero between persons.

```
lo = hi = 0
for i in 0 .. m-1:                         # sessions in start order
    s = S[i].start
    while hi < n and C[hi].k <= s + W7:    # clicks entering the window
        c = C[hi].camp
        if cnt[c]++ == 0: enter[c] = i
        hi++
    while lo < hi and C[lo].k < s:         # clicks leaving the window
        c = C[lo].camp
        if --cnt[c] == 0: flush(c, enter[c], i)
        lo++
    if hi > lo:                            # credit
        j = hi-1; best = j; t = C[j].k
        while j > lo and C[j-1].k == t:    # equal-ts group only
            j--; if memcmp(cid(C[j]), cid(C[best])) < 0: best = j
        credited = C[best].camp
    else:
        credited = ORGANIC
    A[credited][S[i].day_rel].sessions   += 1
    A[credited][S[i].day_rel].purchasing += (S[i].revenue > 0)
    A[credited][S[i].day_rel].revenue    += S[i].revenue
for j in lo .. hi-1:                       # close what is still open
    c = C[j].camp
    if cnt[c] > 0: flush(c, enter[c], m); cnt[c] = 0

flush(c, a, b):                            # campaign c assisted sessions [a, b)
    for d in S[a].day_rel .. S[b-1].day_rel (skip if a >= b):
        x = min(b, day_first[d+1]) - max(a, day_first[d])
        if x > 0: A[c][d].assisted += x
```

Why this is exact:

-   `lo <= hi` always holds. A click with `k < s` also has `k <= s + W7`,
    so `hi` has already passed it.
-   Campaign `c` assists session `i` iff `cnt[c] > 0` after both loops
    for `i`. The `hi` loop runs first, so a campaign whose old click
    leaves and new click enters on the same session stays continuously
    active. A click that enters and leaves on the same `i` (it qualifies
    for no session) flushes the empty interval `[i, i)`.
-   Each campaign assists a session at most once by construction, and
    `organic` never enters `cnt`.
-   The credited click is in `[lo, hi)` and has the maximum `k`. Ties at
    that `k` are resolved by byte-wise smallest `click_id`. Only the
    equal-`k` group at the top of the window is compared, which is
    usually a single click. The generator plants same-millisecond pairs
    specifically to test this.
-   The final loop over `[lo, hi)` both flushes and resets `cnt`, so no
    `O(ncamp)` clear is needed per person. Clicks at `>= hi` never
    touched `cnt`.

Cost: sorting plus `O(m + n)` pointer work, plus `O(days spanned)` per
flush. Assist work no longer scales with `sessions × active campaigns`
(see D5).

### 9.4 Persons without clicks

All sessions go to `organic`. The same loop handles this (`n = 0`), so
no separate path is required. A fast path that skips pass 2 is allowed
if measured.

------------------------------------------------------------------------

## 10. Aggregation tables

Per worker, dense over `(campaign incl. organic) × day_rel`:

-   `ndays = floor((window_end - 1) / D) - floor(window_start / D) + 1`,
    with `D = 86,400,000` and floor division correct for negative values,
    as `day0` already is. Do not assume midnight-aligned windows: the
    fuzzer already generates non-midnight windows.
-   Cell: `sessions` u32, `purchasing` u32, `assisted` u32, `revenue`
    u64. Index `c * ndays + d`, so a flush walks one campaign's days
    contiguously.
-   Size: 2,001 × 15 × 24 B ≈ **720 KiB per worker**. It is lazily
    committed; only touched cells fault in.
-   u32 per-worker counters are safe while total valid events < 2^32
    (assert once in C). The reduction is in u64.

Do not use one shared table with atomics. The 3 to 5 whale campaigns take
30% of clicks, and `organic` takes about 40% of sessions, so their cells
would become cache lines contended by all 8 workers (D6).

Reduction (P7, C): sum the `nw` tables cell-wise, then emit a row iff
`sessions > 0 || assisted > 0`. That is at most `(ncamp + 1) × ndays`
rows (≈ 30K), so this is microseconds of work.

------------------------------------------------------------------------

## 11. `campaign_daily` output

`/output/campaign_daily/part-00000.parquet`, exactly these columns:

| Column                | Parquet type             | Source                                  |
| --------------------- | ------------------------ | --------------------------------------- |
| `campaign_id`         | BYTE_ARRAY (STRING)      | campaign `Slice`, or `"organic"`        |
| `channel`             | BYTE_ARRAY (STRING)      | channel `Slice`, or `"organic"`         |
| `day`                 | INT32 + DATE             | `day0 + d`                              |
| `sessions`            | INT64                    | credited sessions                       |
| `purchasing_sessions` | INT64                    | credited sessions with revenue > 0      |
| `revenue_cents`       | INT64                    | credited revenue                        |
| `assisted_sessions`   | INT64                    | assists; always 0 for `organic`         |

No column is nullable. An assisted-only campaign/day row has 0 in the
three credited columns. Generalise `parquet.c` to a column-descriptor
writer (string, date, int64, optional string) rather than writing a
second hand-rolled file writer. Both outputs are small, so they can be
written sequentially.

------------------------------------------------------------------------

## 12. Record shapes (ABI sketch for `evx.h`)

The coding agent owns the final packing. These shapes satisfy the size
and locality goals. Every offset gets a `_Static_assert`, as now.

| Record      | Size   | Fields                                                          | Lifetime          |
| ----------- | ------ | --------------------------------------------------------------- | ----------------- |
| `ClickCand` | 32 B   | `u64 k`, `u32 pid`, `u32 camp`, `ptr cid`, `u32 cid_len`, pad    | C1 → end of P6 (cold: `cid` is read only for tie groups) |
| `ClickEv`   | 16 B   | `u64 k`, `u32 camp`, `u32 cand_idx`                             | C3 → end of P6    |
| `SessRec`   | 24 B   | `i64 start`, `u32 day_rel`, pad, `u64 revenue`                  | per-person scratch |
| `PIndex` slot | 8 B  | `tag32 << 32 | (pid + 1)`                                       | PI → C1           |
| Agg cell    | 24 B   | `u32 sessions, purchasing, assisted`, `u64 revenue`             | per worker, P6 → P7 |

`ClickEv` deliberately has the shape of `Ev` (an 8-byte sort key at offset
0, 16 bytes in total), so the proven insertion/radix sort applies
unchanged. `cand_idx` is a u32 index into the `ClickCand` buffer: assert
that the reserved candidate count is below 2^32.

Contexts: `ClickParseCtx` (window bounds, link table, campaign table,
`PIndex`, `pnames`, `ccount`) and a per-worker `AttrCtx` (events,
offsets, `ClickEv`, `ClickCand`, `coff`, click sort scratch,
`SessRec` scratch, `day_first`, `cnt`, `enter`, agg table, `day0`,
`base_ms`, click radix passes, `ncamp`, `ndays`).

------------------------------------------------------------------------

## 13. Data structures and constants at full scale

Assumptions: 48M events, 12M clicks (+1–3% duplicate rows), about 1.6M
people, 2,000 campaigns, a 14-day window (15 day buckets if not
midnight-aligned), 8 workers. Sizes are computed from those counts;
timings are not claimed here.

| Structure                     | Count × size                     | Bytes        | Access pattern / residency            |
| ----------------------------- | -------------------------------- | ------------ | ------------------------------------- |
| Campaign lookup               | 4,096 × 32 B                     | 128 KiB      | random, L2-resident                   |
| Channel / campaign slices     | 2,001 × 16 B                     | 32 KiB       | P7 only                               |
| `PIndex`                      | 4,194,304 × 8 B                  | 32 MiB       | random, 1 probe/click, huge pages     |
| `ClickCand` (touched)         | ~12.2M × 32 B                    | ~390 MiB     | sequential write C1, sequential read C3, rare random read P6 |
| `ClickCand` (reserved)        | click bytes / 7 × 32 B           | virtual only | `MAP_NORESERVE`; ≥ 7 bytes per surviving row |
| `ccount`                      | 1.6M × 4 B                       | 6.4 MiB      | `lock inc`, random                    |
| `coff` + cursor               | 2 × 1.6M × 8 B                   | 25.6 MiB     | sequential (C2), `lock xadd` (C3)     |
| `ClickEv`                     | ~12M × 16 B                      | ~192 MiB     | scatter write C3, sequential read P6  |
| Click sort scratch / worker   | max clicks per person × 16 B     | ≤ a few MiB  | whales only                           |
| `SessRec` scratch / worker    | max events per person × 24 B     | lazily touched | whales only touch the tail          |
| `cnt` + `enter` / worker      | 2 × 2,000 × 4 B                  | 16 KiB       | L1-resident                           |
| `day_first` / worker          | (ndays + 2) × 4 B                | < 1 KiB      | L1                                    |
| Agg table / worker            | 2,001 × 15 × 24 B                | ~720 KiB     | L2                                    |
| Output                        | ≤ 30,015 rows                    | < 2 MiB      | P7                                    |

Memory peak: the click side adds about 650 MiB, live only after P5. By
then the events candidate store, dedup table and person table are freed,
and the live events data is `Ev` (48M × 16 B = 768 MiB) plus offsets and
names. The process peak therefore stays at events P3. That is about
4.8 GiB measured at 20M; linear extrapolation gives about 11–12 GiB at
48M, which is an estimate to be measured, well inside 24 GB.

------------------------------------------------------------------------

## 14. Concurrency and scheduling

-   **C1**: newline-aligned byte-range tasks over all click files, with
    the same chunking rule as events (≥ 1 MiB, about 16 tasks per
    worker), dynamic queue. Files are Zipf-sized (the largest holds
    about a fifth of the rows), and 1 to 3 are header-only.
-   **`ccount` histogram**: `lock inc` during C1, as events do in P3.
    Whale people concentrate increments on a few lines. That costs a few
    tens of thousands of contended increments, which is acceptable.
    Per-worker histograms (8 × 6.4 MiB plus a reduction) are the
    alternative if contention shows up in profiles.
-   **C3**: tasks = C1 task outputs. `lock xadd` cursors per person,
    identical to `evx_scatter`.
-   **P6 blocks**: weight a block by `events + clicks` (currently events
    only). Close a block at 16,384 weighted items or 4,096 people. Whale
    persons get a block of their own, as now.
-   **Barriers**: PI → C1 → C2 → C3 → P6 → P7. No structure is read while
    another worker can write it. `cnt`, `enter`, scratch and agg tables
    are per worker, indexed by `wid`, like `SessCtx`.
-   **CPU budget**: the same pool, the same `CPU_BUDGET = 8`. No extra
    threads.

------------------------------------------------------------------------

## 15. Optimisation priorities (click side)

1.  **Correctness first.** Official fixtures and the dev set on both
    tables.
2.  **No global click dedup** (§8).
3.  **Drop early and cheap.** Order checks so that dropped rows skip the
    link probe and the `PIndex` probe (§6.4).
4.  **Hide the two random probes per click.** The link table (hundreds
    of MiB) and `PIndex` (32 MiB) are DRAM/LLC misses per row. Batch 8
    to 16 rows: hash all of them, `prefetcht0` link slots, then resolve
    and prefetch `PIndex` slots, then probe. This is the same technique
    as dedup's `prefetchw` 8 ahead. Expect C1 to dominate click-side
    time, because events P1 measures about 250 ns of CPU per row with
    the same probe pattern.
5.  **Cache-hot attribution.** Attribute each block right after
    sessionizing it (§9.1).
6.  **Sort by integer key only.** `click_id` bytes are touched only in
    equal-ts top groups.
7.  **Page faults.** Events at 20M show sys time above user time from
    first-touch faults. Allocate `ClickCand` and `ClickEv` with
    `MADV_HUGEPAGE` (`big_alloc` already does). Consider reusing a
    freed events buffer (for example the `wins` region) for `ClickEv` to
    avoid fresh faults.
8.  **Measured only, not baseline:**
    -   merging PI into the P5 pool phase and C3 into C2's tail;
    -   lexing clicks during the single-threaded P0, which leaves 7
        cores idle for 10–115 ms (more at 48M links): lex and validate,
        then resolve persons later. This costs 16 more bytes per click
        and a second pass;
    -   storing dense `pid` directly in link-table entries after P4, to
        skip the `PIndex` probe for linked IDs;
    -   fusing attribution into `evx_sessionize`, which saves one L2-hot
        re-scan but modifies verified code;
    -   a `ymm` scanner variant (shared with events item 4 in
        `ASM_OPTIMIZATIONS.md`).
9.  **Record** in stderr stats: click files, bytes, rows, dropped by
    reason, valid clicks, clicks for event-less people, persons with
    clicks, max clicks per person, sessions credited non-organic,
    organic sessions, assist increments, output rows, and phase times
    for PI, C1, C2, C3 and attribution.

------------------------------------------------------------------------

## 16. Correctness tests and invariants

### 16.1 Harnesses (in order)

1.  `make unit`: add `evx_memcmp` cases (prefix ordering, high-bit bytes
    > 0x7F compare unsigned, equal) and direct `evx_attribute` cases on
    hand-built `Ev`/`ClickEv` arrays.
2.  `make fixtures` (official, Docker, **both tables**). All 36 must
    pass. Native: `scripts/run_fixtures.py fixtures -- src/main`.
3.  `make run-dev && make check-dev`: the dev answer includes
    `campaign_daily`.
4.  Extend `scripts/reference_events.py` into a full reference (clicks,
    real dedup, credit, assists). Compare on generator seeds 2, 3, 4 and
    11, and on 10M+ runs.
5.  Extend `scripts/fuzz_events.py` with the click cases below.

### 16.2 Click edge cases to cover

-   Every click file read, including header-only files; a duplicate
    click in another file, quoted differently.
-   Empty `click_id` (dropped), empty `user_id` (dropped), `user_id`
    upper-cased (a different person).
-   Unknown campaign, including case variants and a leading/trailing
    space.
-   Click `ts` in all of F1, F2 (date-changing offsets), F3 with leading
    zeros, and `\r` on `ts`; F3 overflow.
-   Click window: exactly `window_start - 7d` (in),
    `window_start - 7d - 1 ms` (out), `window_end - 1 ms` (in, qualifies
    for nothing unless a session starts at or after it),
    `window_end` (out).
-   Attribution boundaries: a click exactly 7 days before the start
    (credits), 7 days + 1 ms (does not), exactly at the start (credits),
    1 ms after (does not).
-   Same-millisecond clicks with different `click_id`s: byte-wise order
    (`"c10"` < `"c9"`, `"C1"` < `"c1"`, `" c"` < `"c"`, a prefix sorts
    first, bytes ≥ 0x80).
-   Clicks from a non-first linked ID; clicks from an ID that appears
    only in links; clicks for a person with no surviving events (no
    effect).
-   One click crediting several sessions; a campaign assisting without
    being credited (an assisted-only row with zero credited columns); a
    campaign with several qualifying clicks in one session (counted
    once).
-   Sessions crossing midnight: credit and assists count on the start
    day.
-   A person with sessions on non-adjacent days, so `flush` spans empty
    day buckets.
-   A whale person with many campaigns active simultaneously (stresses
    `flush` and the tie group).

### 16.3 Run-time invariants (cheap, always on, printed to stderr)

-   `Σ campaign_daily.sessions == Σ user_daily.sessions`. This guards the
    re-derived session rule.
-   `Σ campaign_daily.revenue_cents == Σ user_daily.revenue_cents`.
-   For every non-organic row: `assisted_sessions >= sessions`, because
    the credited campaign also assists that session. This is a per-row
    check, much stronger than the summed version in the rules.
-   The `organic` row has `assisted_sessions == 0`.
-   No duplicate `(campaign_id, day)` keys, by construction of the dense
    table.

A violated invariant is printed loudly. The binary still writes its
output (exiting non-zero gains nothing at scoring), but the test
scripts treat the message as a failure.

------------------------------------------------------------------------

## 17. Delivery plan and contingency

It is about 01:00 IST and the deadline is 21:30 IST.

| Stage | Content                                                                                | Gate |
| ----- | -------------------------------------------------------------------------------------- | ---- |
| S1    | C: campaigns dict, `pnames` tag, `PIndex`, click buffers/phases, generic Parquet writer, `campaign_daily` with all sessions organic | Pipeline runs end to end; session and revenue invariants hold |
| S2    | asm: `evx_parse_clicks`, `evx_scatter_clicks`, `evx_memcmp` + unit tests              | `make unit`; click counts match the reference on dev |
| S3    | asm: `evx_attribute`                                                                    | Official 36 fixtures (both tables), `check-dev` MATCH |
| S4    | Differential seeds + fuzz; Docker run at 48M/12M; record times and peak RSS           | All MATCH; RSS < 24 GB with margin |
| S5    | Measured optimisations from §15.8, strictly one at a time, each re-verified            | No regression |
| —     | Commit and submit a verified SHA by about 20:30 IST                                   | — |

**Contingency (recommended):** if `evx_attribute` in assembly is not
fixture-clean by about 16:00 IST, ship the same kernel in C behind the
identical ABI and keep porting afterwards.

-   Reasoning: attribution is `O(sessions + clicks)` with L1/L2-resident
    state. Its cost is small next to C1 parsing, so a `-O3` C version
    should cost little wall time. A missing or wrong `campaign_daily`
    costs the whole score.
-   This departs from the "event semantics only in assembly" principle
    for one kernel, as a time-boxed exception. Record it in
    `SPEC_REVIEW.md` if it is used.

------------------------------------------------------------------------

## 18. Design decisions and alternatives considered

**D1. No global click dedup.** *Chosen:* skip it, because it is
provably a no-op (§8). *Alternative:* a lock-free CAS table like events
P2 (about 256 MiB, 12M random CAS operations). *Why not:* pure cost,
zero semantic effect under the rules. *Risk:* the rules' guarantee is
violated; this is covered by the reference oracle.

**D2. Validate at parse time and drop immediately.** For events this
would be wrong (dedup must come first). For clicks it is exact, because
all copies are identical, so validation order cannot change which copy
"wins". This halves the stored click volume compared with keeping
candidates plus flags.

**D3. Drop clicks of people without surviving events in C1.** Exact:
such a person has no sessions. It shrinks `ClickCand` and `ClickEv` and
removes those clicks from the C3 and P6 work.

**D4. A separate `evx_attribute` that re-derives sessions, run per
block right after `evx_sessionize`.** *Alternative A:* modify
`evx_sessionize` to emit sessions or call attribution directly. That
saves an L2-hot re-scan of `Ev` but changes verified code under
deadline. *Alternative B:* materialise a global session table (about
10M × 24 B) and do a global sort-merge join with clicks, as a SQL engine
would. *Why not:* a second grouping and an extra global
materialisation, when both inputs are already grouped by the same dense
person ID. *Chosen* for minimum regression risk and near-fused cache
behaviour. The duplicated gap rule is guarded exactly by the Σ-sessions
invariant on every run.

**D5. Assists by lazy interval flush.** *Alternative:* keep an active
campaign list and add 1 for every active campaign per session. That
costs `Σ sessions × active campaigns`. It is cheap for typical people
but unbounded in principle (a person with hundreds of sessions and
hundreds of active campaigns from whale-campaign traffic and merged
shared-device identities). The flush costs `O(clicks × days spanned)`,
with the same per-person state (`cnt`) plus one `enter[]` array and a
tiny `day_first[]`.

**D6. Per-worker dense aggregate tables.** *Alternative:* shared table
with `lock add`. *Why not:* whale campaigns and `organic` concentrate
writes on a few cache lines, which become contended by 8 workers. The
dense tables are about 720 KiB each and the reduction is about 30K cells.

**D7. Compact `PIndex` instead of retaining the P3 person table.**
32 MiB instead of about 1.5 GiB (table plus remap) at 48M. Fewer misses,
nearly no TLB misses, and the P4 frees stay as they are.

**D8. Click key in the click frame.** `k = ts - cw_start` makes the
qualifying interval `[s, s + W7]` with unsigned keys only. This avoids
negative offsets for clicks up to 7 days before the window, and keeps
the radix sort on non-negative keys.

**D9. Integer-key sort, byte compare only on ties.** Sorting by
`(ts, click_id)` would put string compares in the sort. Only the
equal-ts group at the top of each window needs `click_id`, which keeps
`click_id` out of the hot record (`ClickEv` stays 16 B, Ev-shaped).

**D10. Clicks after events in the phase graph.** *Alternative:* parse
clicks inside P1's queue for load balance. *Why not (baseline):* person
resolution needs dense IDs (P4). Splitting C1 into lex-early and
resolve-late costs a second pass and 16 B per click. It is kept as a
measured option (§15.8) because the single-threaded P0 is the one place
where cores sit idle.

------------------------------------------------------------------------

## 19. Completion criteria

1.  `make` produces a static `src/main`, with no shared-library
    dependencies.
2.  The image runs as UID 1000 and writes only the two output folders.
3.  `make fixtures` passes all 36 fixtures on both tables, through
    Docker.
4.  `make check-dev` reports MATCH for both tables.
5.  Differential seeds and fuzz cases MATCH the full Python reference.
6.  All §16.3 invariants hold on every run, including at 48M/12M.
7.  Peak RSS and wall time at 48M/12M are measured under 8 CPU / 24 GB
    and recorded in `ASM_OPTIMIZATIONS.md`.
8.  `CLAUDE.md`, `SPEC_REVIEW.md` and `IMPLEMENTATION_PLAN.md` are
    updated: the events-only notes are removed, and the clicks phases
    and tests are added.

------------------------------------------------------------------------

## 20. Key decisions at a glance

| Area                     | Decision                                                                   |
| ------------------------ | -------------------------------------------------------------------------- |
| Scope                    | Clicks, campaigns, attribution, `campaign_daily`; events side unchanged    |
| Output                   | `campaign_daily/part-00000.parquet` via generalised `parquet.c`            |
| Click dedup              | None (provably a no-op under the rules); reference oracle dedups           |
| Validation               | At parse time; every failure is a drop                                     |
| Click window             | `window_start - 7d <= ts < window_end`                                     |
| Person resolution        | Shared canonicalisation routine → compact `PIndex` (32 MiB) → dense pid    |
| Event-less people        | Their clicks dropped in C1                                                 |
| Grouping                 | Counting sort by dense pid (`lock inc`/`lock xadd`), as events             |
| Click record             | 16 B, Ev-shaped `{k, camp, cand_idx}`; `click_id` in a cold 32 B record    |
| Sessions for attribution | Re-derived per block right after `evx_sessionize` (L2-hot); not modified   |
| Credit                   | Two-pointer window `[s, s + 7d]`; latest `k`; tie → smallest `click_id`    |
| Assists                  | Lazy interval flush per campaign, `O(clicks × days)`                       |
| Aggregation              | Per-worker dense `(campaign + organic) × day` tables; C reduction          |
| Concurrency              | Same 8-worker pool; dynamic queues; blocks weighted by events + clicks     |
| Guard rails              | Σ sessions, Σ revenue, per-row `assisted >= sessions` on every run         |
| Contingency              | C `evx_attribute` behind the same ABI if asm is not fixture-clean by about 16:00 IST |
