#!/usr/bin/env python3
"""
Events-only checker: like run_fixtures.py / compare.py, but only for the
user_daily table (the events-only spec does not produce campaign_daily).

  check_events.py FIXTURES_DIR -- CMD...        run CMD on every fixture
  check_events.py --compare OUT_DIR ANSWER_DIR  exact diff of user_daily

Reuses the loaders and schema of run_fixtures.py, so the type and schema
checks are the same as the supplied harness.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import polars as pl

from run_fixtures import SCHEMAS, load_actual, load_expected, rows

TABLE = "user_daily"
SCHEMA = SCHEMAS[TABLE]


def load(folder):
    folder = Path(folder)
    csv = folder / f"{TABLE}.csv"
    if csv.is_file():
        return load_expected(csv, SCHEMA)
    return load_actual(folder / TABLE, SCHEMA)


def compare(left, right):
    a, b = load(left), load(right)
    keys = ["user_id", "day"]
    dup_a = a.height - a.n_unique(subset=keys)
    dup_b = b.height - b.n_unique(subset=keys)
    only_a = a.join(b, on=list(SCHEMA), how="anti", nulls_equal=True)
    only_b = b.join(a, on=list(SCHEMA), how="anti", nulls_equal=True)
    print(f"{TABLE}: rows {a.height} vs {b.height}; only in A {only_a.height}; "
          f"only in B {only_b.height}; duplicate keys {dup_a} vs {dup_b}")
    if dup_a or dup_b or only_a.height or only_b.height:
        print(only_a.sort(keys).head(10))
        print(only_b.sort(keys).head(10))
        print("DIFFER")
        return 1
    print("MATCH")
    return 0


def run_fixtures(fx_dir, cmd):
    cases = sorted(d for d in os.listdir(fx_dir) if os.path.isdir(os.path.join(fx_dir, d)))
    failed = []
    for case in cases:
        data = os.path.join(fx_dir, case, "input")
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as out:
            env = dict(os.environ, DATA_DIR=data, OUTPUT_DIR=out)
            p = subprocess.run(cmd + [data, out], env=env, capture_output=True, text=True)
            if p.returncode != 0:
                failed.append(case)
                print(f"FAIL {case}: exit {p.returncode}\n{p.stderr[-2000:]}")
                continue
            exp = rows(load_expected(os.path.join(fx_dir, case, "expected", f"{TABLE}.csv"), SCHEMA))
            try:
                act = rows(load_actual(os.path.join(out, TABLE), SCHEMA))
            except (ValueError, OSError, pl.exceptions.PolarsError) as error:
                failed.append(case)
                print(f"FAIL {case}: {error}")
                continue
            files = [f for f in os.listdir(os.path.join(out, TABLE)) if f.endswith(".parquet")]
            if exp != act or files != ["part-00000.parquet"]:
                failed.append(case)
                print(f"FAIL {case}\n  expected {exp}\n  actual   {act}\n  files {files}")
            else:
                print(f"ok   {case}")
    print(f"\n{len(cases) - len(failed)}/{len(cases)} fixtures passed ({TABLE} only)")
    return 1 if failed else 0


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--compare":
        return compare(sys.argv[2], sys.argv[3])
    if "--" in sys.argv:
        sep = sys.argv.index("--")
        return run_fixtures(sys.argv[1], sys.argv[sep + 1:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
