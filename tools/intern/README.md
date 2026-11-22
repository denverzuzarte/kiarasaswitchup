# intern: a local LLM as an optimisation intern

The harness pairs an **architect** (Claude or a human) with an **intern**, a local model behind llama-server's OpenAI-compatible API (default: gpt-oss-20b).

- The **architect** makes the structural changes in the repo. They write small, scoped assignments as `tasks/*.json`, review the inbox, and merge what is good.
- The **intern** gets one task at a time and answers with SEARCH/REPLACE edit blocks.
- The **harness** (`intern.py`) is the referee. It never touches the repo; everything happens under `build/intern/`.

Per attempt the harness:

1. Copies the intern's current best tree to `build/intern/work`.
2. Sends the rules, the task, the code regions named by the task and feedback from earlier attempts.
3. Applies the edit blocks. Each SEARCH must match exactly once, and only files listed in `editable` may change.
4. Runs the gates, cheapest first, each with a timeout:
   - build, with no new warnings
   - static-link check
   - `make unit`
   - 36 fixtures (both tables)
   - dev answer, run three ways: normal, `EVX_PSIZE=1024` (forced person-table reruns) and `NCPU=1`
   - fuzz golden output
   - 20M golden output
   - silence on stdout and stderr
5. Benchmarks the candidate interleaved against the current best: alternating order, idle-CPU gated, under `build/intern/bench.lock`. It is accepted only if the median improves by at least `accept_pct` **and** the candidate wins at least `accept_win_frac` of the paired rounds.
6. On acceptance, writes a patch and a report to `build/intern/inbox/`, and the tree becomes the intern's new best (hill climbing). Otherwise the failure (compiler error, failing comparison, timing) is fed back into the next attempt.

`build/intern/ledger.jsonl` records every attempt.

## Setup

```sh
llama-server -m ~/models/gpt-oss-20b-MXFP4.gguf -ngl 99 -c 32768 --jinja --port 8080
python3 tools/intern/intern.py init      # writes build/intern/config.json on the first call
$EDITOR build/intern/config.json         # dataset paths, a Python with polars
python3 tools/intern/intern.py init      # base tree, base binary, golden outputs, sanity gates
```

## Use

```sh
python3 tools/intern/intern.py run 01-links-finish-prefetch     # one task
python3 tools/intern/intern.py run --all --rounds 3             # every open task, keep pushing
python3 tools/intern/intern.py auto --top 3                     # tasks for the 3 hottest functions (perf)
python3 tools/intern/intern.py status
```

## Writing a task

```json
{
  "goal": "what to make faster, why it is slow (counters), what must not change",
  "editable": ["src/dict.c"],
  "context": [{"file": "src/dict.c", "function": "links_finish"},
              {"file": "src/evx.h"},
              {"file": "src/main.c", "start": "^static void job_write", "end": "^}"}],
  "hints": ["..."],
  "attempts": 6
}
```

`function` extracts an asm function (from the comment block before `.globl` to `.size`) or a C definition. Keep tasks small: a 20B model does well on one function and one idea, and badly on cross-file restructuring. That work belongs to the architect.

## Review rule

An accepted patch is *verified* (correct and faster on this host), not *approved*. The architect still reads every diff before it goes into the repo. Watch for dataset-specific shortcuts that the gates can't see, and keep in mind that the scorer is Cascade Lake, not this Zen 5 host.

## Sharing the machine

Any other benchmark should hold the same lock, so timings never overlap:
`flock build/intern/bench.lock make bench ...`
