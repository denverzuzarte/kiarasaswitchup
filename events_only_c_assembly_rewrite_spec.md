# Events-Only Rewrite Specification: C Orchestrator + x86-64 Assembly Event Engine

**Status:** Revised high-level implementation specification\
**Scope:** Events, linked identities, products/categories,
sessionization, and the events-derived `user_daily` output only\
**Target:** Linux `amd64`, Intel Xeon Cascade Lake, 8 CPUs, 24 GB
memory\
**Languages:** C for orchestration and supporting infrastructure; GNU
x86-64 assembly for the event-processing engine\
**Output:** One Parquet file containing the `user_daily` table\
**Runtime image:** Static executable in the supplied `FROM scratch`
Docker image

This replaces the earlier C++-centred specification. C++ is not part of
the intended implementation. C++ examples in earlier discussion were
explanations of data-structure ideas, not a language requirement.

The coding agent is responsible for the low-level implementation and
instruction selection. This document defines the system boundary,
correctness rules, high-level data layout, concurrency model, build
contract, and optimization priorities.

------------------------------------------------------------------------

## 1. Scope

### Included

1.  Read the manifest and product dictionary.
2.  Read and resolve all linked user IDs using DSU.
3.  Read every `events/*.csv` part file.
4.  Strictly parse event CSV rows and timestamps.
5.  Globally deduplicate events by `event_id`.
6.  Validate the winning event copies and apply late-event filtering.
7.  Convert raw users to canonical people.
8.  Group and order events by canonical person and timestamp.
9.  Sessionize events.
10. Assign each session to its UTC start day.
11. Aggregate user/day session counts, event counts, purchase revenue,
    and top category.
12. Write one Parquet file for the events-derived `user_daily` table.

### Explicitly excluded

-   `clicks/*.csv` processing.
-   Click deduplication and validation.
-   Campaign attribution and assists.
-   `campaigns.csv`.
-   `campaign_daily`.
-   Any click-related SQL or output.

The supplied rules describe both `user_daily` and `campaign_daily`; this
implementation intentionally implements only the events-derived
`user_daily` side, as requested. Do not accidentally add a partial click
pipeline.

------------------------------------------------------------------------

## 2. Source constraints and build contract

The supplied `Makefile`, `Dockerfile`, and `The rules.md` are part of
this specification.

### 2.1 Makefile constraints

The Makefile establishes:

-   `gcc` is the compiler and linker driver.
-   C sources are `src/*.c`.
-   GNU assembler sources are `src/*.s`.
-   The executable target is `src/main`.
-   C compilation uses `-O3 -march=cascadelake -mtune=cascadelake`.
-   Assembly is assembled for the same Cascade Lake target.
-   The executable is linked statically and without PIE.
-   `pthread` and `libm` are currently the declared system libraries.
-   The default container resource settings are `CPUS=8` and `MEM=24g`.
-   The target platform is `linux/amd64`.

The implementation must keep the existing source layout and
Makefile-driven build unless a necessary build dependency requires a
documented Makefile change.

### 2.2 Dockerfile constraints

The supplied Dockerfile is:

``` dockerfile
FROM scratch
COPY src/main /main
USER 1000:1000
WORKDIR /
CMD ["/main"]
```

Consequences:

-   The final executable must be a statically linked Linux AMD64
    executable.
-   It cannot rely on a shell, Python, package manager, dynamic loader,
    shared libraries, or other runtime utilities being present in the
    image.
-   The program must work as an unprivileged UID/GID 1000.
-   Input is mounted read-only at `/data`.
-   Output and scratch paths must be writable in the runtime
    environment; do not assume `/` is writable.
-   The executable must use the paths and environment contract supplied
    by the runner.

Do not change the image to a general-purpose Linux distribution merely
to make a dependency work.

### 2.3 DuckDB C API integration is a build dependency, not an assumption

The earlier design selected DuckDB's C API for Parquet output. That
remains the intended output path **only if a compatible static DuckDB
library is available to the build**.

The current Makefile lists only `-pthread -lm`; it does not currently
link DuckDB. The coding agent must inspect the provided build
environment and available libraries, then make the smallest required
Makefile/linker change. The final binary must still satisfy the
`scratch` static-runtime constraint.

If the required static DuckDB library is not available, report that as a
build blocker rather than silently switching to C++, Python, a
dynamically linked DuckDB library, or an unrelated output format.

------------------------------------------------------------------------

## 3. Language and responsibility split

### 3.1 C: coordinator and supporting infrastructure

C is responsible for:

-   Process entry point and argument/environment handling.
-   Discovering and scheduling input files.
-   Worker-thread lifecycle and synchronization.
-   Reading file data or mapping input files into memory.
-   Allocating and owning shared buffers.
-   Loading `manifest.json`.
-   Loading `products.csv` and building the product/category dictionary.
-   Parsing `links/*.csv` and constructing DSU identity mappings.
-   Providing the assembly engine with buffers, lookup structures, and
    memory regions.
-   Managing phase barriers and error propagation.
-   Managing DuckDB C API calls and Parquet output.
-   Diagnostics, counters, timing, and resource reporting.

C is the orchestrator. It should not reimplement the assembly event
engine's hot-path semantics in a second code path.

### 3.2 GNU x86-64 assembly: event engine

Assembly is the primary implementation language for the event-processing
hot path.

The assembly event engine owns the performance-critical event
transformations:

-   Strict event-row parsing for the specified CSV dialect.
-   Event timestamp parsing and UTC conversion.
-   Candidate construction and compact representation.
-   Global `event_id` deduplication semantics.
-   Post-dedup event validation.
-   Late-event filtering.
-   Canonical-person assignment using the mapping supplied by C.
-   Compact event materialization.
-   Per-person event ordering/grouping support.
-   Session boundary detection and sessionization.
-   Session-level event counts and purchase-revenue accumulation where
    appropriate.

C may own memory allocation, file scheduling, locks, and the worker
pool. That does not transfer the event semantics out of the assembly
engine. The coding agent should define a stable C-callable assembly
interface and keep the hot path batch-oriented rather than making a
C-to-assembly call for every field or every byte.

The assembly implementation must follow the System V AMD64 ABI. The
coding agent owns register allocation, vectorization, alignment,
unrolling, branch layout, and other instruction-level decisions.

### 3.3 No C++ implementation

Do not introduce:

-   `.cpp` source files.
-   `g++` as the main compiler.
-   STL containers or C++ runtime dependencies.
-   C++ exceptions, templates, or object lifetimes.

Earlier references to `std::map`, `std::vector`, and C++ structs were
conceptual examples only. Translate the data-structure intent into
C-owned buffers and assembly-compatible layouts.

------------------------------------------------------------------------

## 4. Process and worker architecture

### 4.1 One process, eight workers

Use one process with a C coordinator and a fixed pool of up to eight
worker threads.

Do not spawn a second process for the preprocessing stage. The event
pipeline needs shared identity mappings, deduplication state, compact
event buffers, and phase synchronization. A second process would require
IPC or shared memory without providing a necessary benefit.

The coordinator:

1.  Initializes metadata and dictionaries.
2.  Resolves links.
3.  Creates the worker pool.
4.  Schedules event-file tasks.
5.  Waits for all event ingestion and deduplication tasks.
6.  Runs/dispatches post-dedup validation and compaction.
7.  Dispatches independent person ranges for sessionization.
8.  Waits for all sessions and aggregates to complete.
9.  Loads the final event/session facts into DuckDB.
10. Writes the Parquet output and shuts down workers cleanly.

A worker never joins itself or kills the process. It completes its task,
reports success/failure, and returns to the pool. The coordinator owns
joins and shutdown.

### 4.2 Dynamic scheduling

The generator creates a variable number of files with highly uneven
sizes, including header-only files. Do not statically assign one fixed
set of files to each worker.

Use a shared task queue. Initially, one task can represent one whole
file. If a large file creates a long tail while other workers are idle,
the coding agent may add newline-aligned byte-range tasks. The CSV
dialect guarantees no embedded newlines in fields.

### 4.3 Phase barriers

The following dependencies must be explicit:

1.  Manifest, products, and links are ready.
2.  All event files have been parsed and global deduplication is
    complete.
3.  Winning candidates have been validated and compacted.
4.  Events are grouped and sorted by canonical person and timestamp.
5.  Sessionization is complete.
6.  User/day aggregation is complete.
7.  Parquet output is closed and validated.

No stage may consume a structure while another worker can still mutate
it.

------------------------------------------------------------------------

## 5. Metadata and identity preparation

### 5.1 Manifest

Read `manifest.json` before event timestamp validation.

Store `window_start` and `window_end` as UTC epoch milliseconds. They
differ by dataset and must never be hard-coded.

An event timestamp is in-window iff:

`window_start <= ts < window_end`

### 5.2 Products and categories

Read `products.csv`. It is clean and `product_id` is unique.

Build a compact lookup from exact product ID bytes to a category ID.
Also retain the category-name table.

Rules:

-   Product IDs and category names are not trimmed or normalized.
-   A nonempty event `product_id` missing from `products.csv` is valid
    and receives category `unknown`.
-   An empty event `product_id` makes the event invalid after
    deduplication.
-   `unknown` participates in top-category revenue comparison.
-   Category ties are resolved by byte-wise smallest category name.

The generator has 50,000 products and 20 categories. A 16-bit product
ordinal and an 8-bit category ID are sufficient for the generated
cardinalities, but the coding agent must bounds-check dictionary sizes
rather than silently truncate.

Do not add a product-popularity prepass to the initial design. The
popularity distribution is a property of generated data, not a
correctness rule. The baseline should use a compact exact product
lookup. A hot-product cache or popularity-ordered auxiliary structure is
a later measured optimization only.

### 5.3 Linked IDs and DSU

Links are required even though clicks are excluded.

Read every `links/*.csv` file before event processing. Drop malformed
link rows and rows where either ID is empty. Every remaining pair joins
the two IDs transitively.

Use DSU with path compression and union by size/rank. Track the
byte-wise smallest ID per component independently of the DSU root.

Important rules:

-   Duplicate, reversed, and self-links are harmless.
-   IDs compare exactly, case-sensitive and untrimmed.
-   An ID that occurs only in links still participates and can determine
    the canonical person name.
-   An ID absent from all valid links is its own person.
-   Canonical person name is the byte-wise smallest ID in the component.

C owns the DSU and produces a lookup usable by the assembly event
engine. The assembly engine must assign every surviving event to the
canonical person, not merely to its raw `user_id`.

------------------------------------------------------------------------

## 6. Strict event input semantics

### 6.1 Input files

Read every `events/*.csv` file. The number and size of files vary. Some
files contain only a header.

Columns:

`event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents`

### 6.2 CSV dialect

The event parser must implement these exact rules:

-   UTF-8, comma-separated.
-   LF (`\n`) ends a line. CR (`\r`) is ordinary field data and must not
    be stripped.
-   Fields may be unquoted or wrapped in one pair of double quotes.
-   Quotes are syntax and are not part of the field value.
-   No field contains an embedded comma, quote, or newline.
-   Empty field means null.
-   Wrong field count is malformed.
-   A quote anywhere except around a whole field is malformed.
-   Empty line is malformed.
-   Spaces are significant and must not be trimmed.
-   ASCII digits are required wherever the rules say digits.

Malformed rows are dropped before deduplication. Empty `event_id` is
also dropped before deduplication.

### 6.3 Timestamp forms

`ts` accepts:

-   F1: `YYYY-MM-DDTHH:MM:SSZ`.
-   F2: `YYYY-MM-DDTHH:MM:SS+HH:MM` or `YYYY-MM-DDTHH:MM:SS-HH:MM`;
    subtract the offset to get UTC.
-   F3: ASCII digits only, interpreted as epoch milliseconds. Leading
    zeros are allowed.

Convert valid values to integer UTC epoch milliseconds.

Reject invalid forms: empty, fractions, lowercase `z`/`t`, missing zone,
spaces instead of `T`, impossible calendar dates, invalid hour/second,
invalid offsets, offsets without a colon, or non-digits in F3.

An F3 number too short or too large for signed 64-bit representation is
syntactically digits-only but outside the event window. It must not be
treated as a malformed CSV row before deduplication.

`ingest_ts` is always valid F1.

------------------------------------------------------------------------

## 7. Event deduplication and validation ordering

This ordering is mandatory.

### 7.1 Deduplication key and winner

Deduplicate globally across **all** event files by exact `event_id`.

Keep the copy with the smallest `ingest_ts`. Equal `ingest_ts` copies
are guaranteed identical.

A duplicate can occur in another file. Per-file deduplication is
incorrect.

### 7.2 What may be discarded before deduplication

Discard immediately:

-   Malformed CSV rows.
-   Rows with empty `event_id`.

Do **not** discard a structurally valid row before deduplication merely
because any of these fields are invalid:

-   `user_id`.
-   `ts` syntax or event-window membership.
-   `event_type`.
-   `product_id`.
-   Purchase `price_cents`.
-   Late-arrival condition.

The parser may evaluate these conditions early and store status flags,
but the row must remain eligible to win deduplication.

Example:

  Copy             `event_id`   `user_id`   `ts`      `ingest_ts`
  ---------------- ------------ ----------- --------- -------------
  Earlier winner   e1           empty       invalid   10:00
  Later copy       e1           u17         valid     10:05

The earlier copy wins, then fails validation. The later copy does not
rescue it.

### 7.3 Streaming deduplication

A separate full-file deduplication pass is not required. The event
engine may maintain a global winner table during ingestion.

For every structurally valid row with nonempty `event_id`:

1.  Parse fields and record parse/validation status.
2.  Find the current winner for `event_id`.
3.  Insert if absent.
4.  Replace only if this row has a smaller `ingest_ts`.
5.  Retain either copy if `ingest_ts` is equal, because equal-ingest
    copies are identical.

This must be global across files and safe under the chosen worker
concurrency model. The coding agent decides the exact
hash-table/sharding/locking implementation.

### 7.4 Validate only the winner

After all event files have been processed and the winner table is final,
reject a winner unless:

1.  `user_id` is nonempty.
2.  `ts` parsed successfully and satisfies the event window.
3.  `event_type` is exactly `view`, `cart`, or `purchase`.
4.  `product_id` is nonempty.
5.  If it is a purchase, `price_cents` consists only of ASCII digits and
    has a value from 1 through 10,000,000.

For `view` and `cart`, ignore `price_cents` completely.

Then apply late-event filtering:

`drop iff ingest_ts - ts > 86,400,000 ms`

Exactly 24 hours is retained. An `ingest_ts` earlier than `ts` is
retained.

------------------------------------------------------------------------

## 8. Compact event representation and memory locality

The goal is to keep the sessionization hot loop focused on the fields it
needs. The coding agent owns the exact assembly layout and packing.

High-level design:

-   Store the canonical person as a compact integer ID or make it
    implicit in a per-person range.
-   Store event timestamp as an offset from `window_start` when the
    validated event is known to be in-window.
-   Store product ordinal compactly.
-   Store purchase price in a bounded integer type.
-   Store event type as a small numeric enum.
-   Keep event metadata separate from the minimal sessionization record
    if that reduces cache traffic.
-   Prefer contiguous buffers and offsets over one heap allocation per
    person.
-   Avoid pointers in the hot record where an index or implicit range
    will do.

The generator has approximately `E / 30` generated people before
identity merging, but this is an average only. Event activity is
Zipf-like, there are five whale users receiving about 1--2% of all
events each, and shared-device links merge many people. Never allocate a
fixed event capacity based on the average.

The event engine should expose a compact, sequential representation
suitable for the sessionization loop. The exact field packing, bit
layout, vector width, and instruction selection are delegated to the
coding agent.

------------------------------------------------------------------------

## 9. Grouping and sessionization

### 9.1 Group by canonical person

All surviving events from every raw ID in one DSU component belong to
the same person.

Group by canonical person and order by UTC `ts`. The event order must
not depend on file order or input row order.

### 9.2 Session boundary

Within each person's timestamp-ordered events, start a new session only
when:

`current_ts - previous_ts > 1,800,000 ms`

-   Exactly 30 minutes remains in the current session.
-   30 minutes plus 1 ms starts a new session.
-   Equal timestamps stay in the same session, regardless of read order.

### 9.3 Session start and day

A session starts at the first event's timestamp.

Its day is the UTC calendar date of that start. All events and purchases
in the session count on that start day, including events after midnight.

### 9.4 Session aggregates

For each session, derive:

-   canonical person ID,
-   session start timestamp,
-   UTC session-start day,
-   event count across all event types,
-   purchase revenue in cents,
-   purchase facts needed for category aggregation.

Use a sufficiently wide accumulator for total revenue. Do not assume the
sum of many valid 32-bit purchase prices fits in 32 bits.

Person ranges are independent and can be sessionized in parallel. Within
one person, the scan is sequential because each boundary depends on the
previous timestamp. Use dynamic scheduling so whale-person ranges do not
leave workers idle.

------------------------------------------------------------------------

## 10. User/day aggregation and category rules

The output is the events-derived `user_daily` table only.

One row per canonical person and UTC session-start day with at least one
session.

Columns:

  -----------------------------------------------------------------------
  Column                  Type                    Meaning
  ----------------------- ----------------------- -----------------------
  `user_id`               string                  Byte-wise smallest ID
                                                  in the person's DSU
                                                  component

  `day`                   Parquet DATE            UTC date of session
                                                  start

  `sessions`              integer                 Number of sessions
                                                  starting that day

  `events`                integer                 Number of surviving
                                                  events in those
                                                  sessions

  `revenue_cents`         integer                 Sum of purchase prices
                                                  in those sessions; zero
                                                  if none

  `top_category`          nullable string         Category with greatest
                                                  summed purchase revenue
                                                  for that person/day
  -----------------------------------------------------------------------

Category rules:

-   Use `products.csv`.
-   A nonempty product ID absent from the file maps to `unknown`.
-   `unknown` competes like any other category.
-   Sum purchase price by category across all sessions for the same
    person/day.
-   Choose the category with greatest summed purchase revenue.
-   Break ties using byte-wise smallest category name.
-   If the person/day has no purchases, `top_category` is NULL.

Do not calculate `top_category` independently per session. It is a
person/day aggregate.

------------------------------------------------------------------------

## 11. Parquet output

Produce exactly one Parquet file for this events-only implementation:

`/output/user_daily/part-00000.parquet`

It must contain exactly the six columns in Section 10, with no extra
columns. `day` must be a Parquet DATE, not a string or timestamp.
`top_category` is nullable.

The original rules allow one or more Parquet files directly inside
`/output/user_daily/`; one file is a valid implementation and is the
requested target. Do not create `/output/campaign_daily/` because
campaign output is outside this scope.

Use DuckDB through its C API for the final aggregation/output if the
static library is available and can be linked into the static
executable. The C layer owns DuckDB setup, staging, query execution, and
output closure. The assembly engine supplies validated/sessionized event
facts.

Do not let DuckDB re-parse the original event CSVs. The strict event
parser and deduplication semantics belong to the assembly event engine.

------------------------------------------------------------------------

## 12. High-level optimization priorities

The coding agent owns low-level tuning. Optimize in this order:

1.  **Correctness first:** pass every supplied event-relevant edge case.
2.  **Avoid repeated parsing:** parse each physical event row once;
    maintain a global winner table.
3.  **Batch interfaces:** pass file buffers or batches to assembly
    rather than crossing the C/assembly boundary for every field.
4.  **Global dedup locality:** measure the event-ID table; it may
    dominate memory and random-access cost.
5.  **Compact post-validation records:** keep the sessionization working
    set small.
6.  **Contiguous person ranges:** use offsets into flat buffers; avoid
    per-person allocations.
7.  **Parallelize independent work:** files during ingestion, then
    person ranges during sessionization.
8.  **Balance skew:** dynamically schedule files/ranges because file
    sizes and person activity are Zipf-skewed.
9.  **Use the target CPU deliberately:** build for Cascade Lake as
    specified; the coding agent may use its supported vector instruction
    set where beneficial.
10. **Avoid speculative popularity logic:** no product popularity
    prepass in the baseline. Benchmark a hot-set cache only if category
    lookup appears in profiles as a meaningful bottleneck.
11. **Avoid oversubscription:** keep the total active CPU work within
    the 8-vCPU budget.
12. **Measure:** record phase times, peak RSS, event count, unique event
    IDs, duplicate replacements, valid events, sessions, and output
    rows.

The coding agent should make low-level choices based on measurements
rather than implementing every possible optimization in advance.

------------------------------------------------------------------------

## 13. Correctness tests and invariants

The event implementation must test at least:

-   Every event file is read, including header-only files.
-   A duplicate in one event file and its copy in another file
    deduplicate globally.
-   The smallest `ingest_ts` wins.
-   An invalid earliest winner causes the event to be dropped; a later
    valid copy cannot rescue it.
-   Empty `event_id` is discarded before deduplication.
-   Empty `user_id` is rejected after deduplication.
-   Invalid timestamp syntax is retained as a candidate through
    deduplication, then rejected if it wins.
-   Timestamp F1, F2 and F3 parsing, including offsets and leading
    zeros.
-   Invalid calendar dates and invalid offsets.
-   F3 overflow is treated as outside-window, not as malformed CSV.
-   Event window start is inclusive and end is exclusive.
-   Exactly 24-hour lateness is kept; 24 hours plus 1 ms is dropped.
-   Ingest time before event time is kept.
-   `view` and `cart` ignore invalid `price_cents`.
-   Purchase price must be ASCII digits and in the inclusive allowed
    range.
-   Unknown nonempty products map to `unknown`; empty products
    invalidate the event.
-   Links are transitive and may span multiple files.
-   A linked-but-unused ID can determine the canonical person name.
-   Byte-wise canonical-name ordering is respected.
-   Exactly 30 minutes remains one session; 30 minutes plus 1 ms splits.
-   Equal timestamps remain together.
-   Sessions crossing midnight are assigned to their start date.
-   Top category aggregates over the whole person/day.
-   Category ties use byte-wise order.
-   No-purchase person/day has NULL `top_category`.

Run the supplied fixture harness with the events-relevant fixtures, then
run the public development dataset and compare the single `user_daily`
Parquet output with the corresponding expected output if available.

The generator's seeds change windows, whales, dirt, and file
partitioning. Never hard-code seed-specific observations.

------------------------------------------------------------------------

## 14. Completion criteria

The rewrite is ready when:

1.  `make` produces `src/main`.
2.  The binary is a static Linux AMD64 executable with no runtime
    shared-library dependency.
3.  The supplied `scratch` Docker image starts successfully as UID 1000.
4.  The program reads from `/data` and writes only to the designated
    output location.
5.  All event-related correctness fixtures pass.
6.  The output has the exact `user_daily` schema and is a valid Parquet
    file.
7.  The one-file output can be read by the supplied comparison harness.
8.  Peak memory and runtime are measured under the 8-CPU/24-GB target.
9.  No click or campaign pipeline is accidentally included.

------------------------------------------------------------------------

## 15. Key decisions at a glance

  -----------------------------------------------------------------------
  Area                                Decision
  ----------------------------------- -----------------------------------
  Scope                               Events-only

  Output                              One `user_daily` Parquet file

  Orchestration                       C

  Event engine                        GNU x86-64 assembly

  C++                                 Not used

  CPU target                          Intel Xeon Cascade Lake

  Parallelism                         One process, up to 8 worker threads

  Input scheduling                    Dynamic file task queue

  Links                               C DSU prepass

  Products                            C-loaded exact product/category
                                      dictionary

  Event dedup                         Global by `event_id`, across all
                                      files

  Dedup ordering                      Before event validation

  Event hot path                      Assembly, batch-oriented C ABI

  Event storage                       Compact contiguous buffers and
                                      person offsets

  Sessionization                      Assembly engine; parallel across
                                      independent people

  Output aggregation                  DuckDB C API, conditional on static
                                      library availability

  Runtime                             Statically linked executable in
                                      `FROM scratch`

  Memory target                       24 GB

  Product popularity prepass          Excluded from baseline
  -----------------------------------------------------------------------
