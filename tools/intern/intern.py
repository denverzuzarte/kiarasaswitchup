#!/usr/bin/env python3
"""intern.py - a local LLM (llama-server, OpenAI-compatible API) as an
optimisation intern for this repo.

Roles
  architect (Claude / a human): writes task files (tools/intern/tasks/*.json),
      reviews the inbox, merges what is good.
  intern (gpt-oss-20b on llama-server): proposes one small patch per attempt.
  harness (this file): applies the patch to a scratch copy, runs every
      correctness gate, benchmarks interleaved against the current best, keeps
      only verified wins, and writes a report + patch to the inbox.

The repo itself is never modified: all work happens under build/intern/.

Commands
  intern.py init                 base tree from the repo, base binary, golden outputs
  intern.py run TASK [TASK..]    work the given task files (or names)
  intern.py run --all            every task in tools/intern/tasks/ not yet done
  intern.py auto [--top N]       perf-profile the best binary, make tasks for hot functions
  intern.py status               ledger summary
  intern.py check                gates only, on the current best tree (sanity)
  intern.py daemon               work forever: architect tasks first, else its own
                                 perf-guided ideas; follows repo changes; never stops
                                 (PAUSE file in build/intern pauses it)

Only the Python standard library is used. compare.py needs a Python with
polars (config "py").
"""
import argparse
import difflib
import fcntl
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import random
import threading
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
STATE = os.path.join(REPO, "build", "intern")
BEST = os.path.join(STATE, "best")            # current best source tree
BASEBIN = os.path.join(STATE, "best.bin")     # its binary
GOLD = os.path.join(STATE, "golden")          # reference outputs from the base
INBOX = os.path.join(STATE, "inbox")          # reports + patches for review
LEDGER = os.path.join(STATE, "ledger.jsonl")
LOCK = os.path.join(STATE, "bench.lock")      # shared with any other benchmark
TASKS = os.path.join(HERE, "tasks")
INSTR = os.path.join(STATE, "instructions.md")    # architect's standing instructions
ACCEPTED = os.path.join(STATE, "accepted.json")   # ordered list of accepted patches
FUNCS = os.path.join(STATE, "funcs.json")         # per-function ideas / cooldowns
STATUS = os.path.join(STATE, "status.json")       # daemon heartbeat
PAUSE = os.path.join(STATE, "PAUSE")
COPY_ITEMS = ["src", "scripts", "fixtures", "Makefile", "CLAUDE.md"]

DEFAULT_CFG = {
    "llm_url": "http://127.0.0.1:8080/v1/chat/completions",
    "llm_model": "gpt-oss-20b",
    "reasoning_effort": "high",
    "temperature": 0.6,
    "max_tokens": 12000,
    "llm_timeout_s": 900,
    "py": sys.executable,             # must have polars (compare.py)
    "dev": "",                        # 1M dev set
    "dev_answer": "",                 # official answer folder (CSV)
    "fuzz": "",                       # adversarial set (golden made at init)
    "big": "",                        # 20M set (golden made at init); also the bench set
    "bench": "",                      # bench set (default: big)
    "ncpu": 8,
    "bench_rounds": 7,
    "accept_pct": 1.0,                # median must improve by at least this
    "accept_win_frac": 0.7,           # and win this share of paired rounds
    "attempts": 6,                    # per task
    "run_timeout_s": 60,
    "max_context_chars": 60000,
    "self_attempts": 3,               # attempts per self-generated idea
    "min_profile_pct": 1.0,           # ignore functions below this share
    "reprofile_every": 5,             # self-directed items between perf runs
    "workers": 4,                     # concurrent experiments (llama-server has 4 slots)
    "gate_slots": 2,                  # concurrent 20M-size gate runs (RAM bound)
    "confirm": True,                  # accepted candidates must win a second benchmark too
    "bench_docker": True,             # time inside the scorer's container (scripts/docker_run.sh flags)
    "docker_image": "evx_ours:latest",
    "docker_mem": "24g",
    "bench_window_s": 240,            # rest window period (benchmarks only run in windows)
    "bench_batch": 6,                 # ...or as soon as this many candidates wait
}


# ----------------------------------------------------------------- utilities

def cfg_load():
    path = os.path.join(STATE, "config.json")
    cfg = dict(DEFAULT_CFG)
    if os.path.exists(path):
        cfg.update(json.load(open(path)))
    if not cfg["bench"]:
        cfg["bench"] = cfg["big"]
    if os.environ.get("INTERN_LLM_URL"):          # e.g. a test server
        cfg["llm_url"] = os.environ["INTERN_LLM_URL"]
    return cfg


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def sh(cmd, cwd=None, env=None, timeout=None):
    """Run a command; returns (rc, combined output). rc 124 = timeout."""
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, shell=isinstance(cmd, str))
        return p.returncode, p.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as ex:
        out = (ex.stdout or b"").decode(errors="replace")
        return 124, out + "\n[TIMEOUT after %ss]" % timeout


def tail(text, n=40):
    lines = text.strip().splitlines()
    return "\n".join(lines[-n:])


def ledger(rec):
    rec["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with LEDGER_LOCK:
        with open(LEDGER, "a") as f:
            f.write(json.dumps(rec) + "\n")


def copy_tree(src_root, dst_root):
    if os.path.exists(dst_root):
        shutil.rmtree(dst_root)
    os.makedirs(dst_root)
    ign = shutil.ignore_patterns("*.o", "*.d", "__pycache__", "unit")
    for item in COPY_ITEMS:
        s = os.path.join(src_root, item)
        if os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst_root, item), ignore=ign)
        elif os.path.exists(s):
            shutil.copy2(s, os.path.join(dst_root, item))


def tree_diff(a_root, b_root, files):
    out = []
    for f in sorted(files):
        a = open(os.path.join(a_root, f)).read().splitlines(True) if os.path.exists(os.path.join(a_root, f)) else []
        b = open(os.path.join(b_root, f)).read().splitlines(True) if os.path.exists(os.path.join(b_root, f)) else []
        out.extend(difflib.unified_diff(a, b, "a/" + f, "b/" + f))
    return "".join(out)


def source_files(root):
    fs = []
    for d in ("src",):
        for n in sorted(os.listdir(os.path.join(root, d))):
            if n.endswith((".c", ".h", ".s")):
                fs.append(d + "/" + n)
    return fs


# --------------------------------------------------------------- build + gates

def build(root):
    sh("make clean", cwd=root)
    rc, out = sh("make -j8", cwd=root, timeout=300)
    warnings = len(re.findall(r"warning:", out))
    return rc, out, warnings


def run_prog(binary, data, out_dir, cfg, extra_env=None, ncpu=None):
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    env = {"DATA_DIR": data, "OUTPUT_DIR": out_dir, "NCPU": str(ncpu or cfg["ncpu"])}
    if extra_env:
        env.update(extra_env)
    return sh([binary], env=env, timeout=cfg["run_timeout_s"])


def compare(root, a, b, cfg):
    rc, out = sh([cfg["py"], os.path.join(root, "scripts", "compare.py"), a, b],
                 cwd=os.path.join(root, "scripts"), timeout=300)
    return rc == 0 and out.strip().endswith("MATCH"), out


def gates(root, cfg, base_warnings, quick=False):
    """Fail-fast correctness gates, cheapest first. Returns (ok, gate, log)."""
    rc, out, warns = build(root)
    if rc != 0:
        return False, "build", tail("\n".join(l for l in out.splitlines() if "error" in l or "Error" in l) or out, 30)
    if warns > base_warnings:
        return False, "warnings", tail("\n".join(l for l in out.splitlines() if "warning:" in l), 20)
    binary = os.path.join(root, "src", "main")
    rc, out = sh(["file", binary])
    if "statically linked" not in out:
        return False, "static", out
    rc, out = sh("make unit", cwd=root, timeout=120)
    if rc != 0:
        return False, "unit", tail(out, 30)
    rc, out = sh([cfg["py"], "scripts/run_fixtures.py", "fixtures", "--", "./src/main"], cwd=root, timeout=600)
    if rc != 0:
        return False, "fixtures", tail("\n".join(l for l in out.splitlines() if not l.startswith("ok ")), 30)
    scratch = os.path.join(root, "gate_out")
    runs = [("dev", cfg["dev"], cfg["dev_answer"], {}, None),
            ("dev EVX_PSIZE=1024", cfg["dev"], cfg["dev_answer"], {"EVX_PSIZE": "1024"}, None),
            ("dev NCPU=1", cfg["dev"], cfg["dev_answer"], {}, 1)]
    if cfg["fuzz"]:
        runs.append(("fuzz", cfg["fuzz"], os.path.join(GOLD, "fuzz"), {}, 3))
    if cfg["big"] and not quick:
        runs.append(("20M", cfg["big"], os.path.join(GOLD, "big"), {}, None))
    for name, data, ref, env, ncpu in runs:
        if not data:
            continue
        if GATE_SEM is not None:
            with GATE_SEM:
                rc, out = run_prog(binary, data, scratch, cfg, env, ncpu)
        else:
            rc, out = run_prog(binary, data, scratch, cfg, env, ncpu)
        if rc != 0:
            return False, name, "exit %d\n%s" % (rc, tail(out, 20))
        if out.strip():
            return False, name, "the binary must print nothing on success; it printed:\n" + tail(out, 10)
        ok, cmp = compare(root, scratch, ref, cfg)
        if not ok:
            return False, name, tail(cmp, 30)
    shutil.rmtree(scratch, ignore_errors=True)
    return True, "all", ""


# ------------------------------------------------------------------ benchmark

def idle_wait(min_idle=0.9, max_wait_s=120):
    def snap():
        v = list(map(int, open("/proc/stat").readline().split()[1:8]))
        return sum(v), v[3]
    t_end = time.time() + max_wait_s
    while time.time() < t_end:
        t1, i1 = snap()
        time.sleep(0.5)
        t2, i2 = snap()
        if t2 > t1 and (i2 - i1) / (t2 - t1) >= min_idle:
            return
    log("  (host never went idle; benchmarking anyway)")


def timed_run(cmd, env, timeout_s):
    """Exact wall time in ms. subprocess.run(timeout=...) polls with sleeps of
    up to 50 ms, which quantised every earlier timing to a 50 ms grid; here
    the wait blocks in waitpid and a timer thread kills a hung run."""
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.perf_counter()
    killer = threading.Timer(timeout_s, p.kill)
    killer.start()
    try:
        p.wait()                      # blocking waitpid: no polling
    finally:
        killer.cancel()
    return (time.perf_counter() - t0) * 1000, p.returncode


def time_once(binary, cfg):
    out = os.path.join(STATE, "bench_out")
    if os.path.exists(out):
        shutil.rmtree(out)
    if cfg.get("bench_docker"):
        # the scorer's container: same flags as scripts/docker_run.sh; the
        # candidate binary is mounted over the image's /main
        os.makedirs(out)
        os.chmod(out, 0o777)
        cmd = ["docker", "run", "--rm", "--network", "none", "--read-only",
               "--tmpfs", "/tmp:rw,size=64m,mode=1777", "--cap-drop", "ALL",
               "--security-opt", "no-new-privileges", "--pids-limit", "4096",
               "--cpus", str(cfg["ncpu"]), "--memory", cfg["docker_mem"], "--memory-swap", cfg["docker_mem"],
               "--user", "1000:1000", "-v", os.path.abspath(cfg["bench"]) + ":/data:ro",
               "-v", out + ":/output", "-v", os.path.abspath(binary) + ":/main:ro",
               "-e", "DATA_DIR=/data", "-e", "OUTPUT_DIR=/output", "-e", "NCPU=%d" % cfg["ncpu"],
               cfg["docker_image"], "/main"]
        t, rc = timed_run(cmd, None, cfg["run_timeout_s"] + 60)
    else:
        env = dict(os.environ, DATA_DIR=cfg["bench"], OUTPUT_DIR=out, NCPU=str(cfg["ncpu"]))
        t, rc = timed_run([binary], env, cfg["run_timeout_s"])
    if rc != 0:
        raise RuntimeError("benchmark run failed")
    return t


def bench(base_bin, cand_bin, cfg):
    """Interleaved A/B (order alternates per round), idle-gated, under the
    shared bench lock. Returns dict with medians, win fraction, verdict."""
    with open(LOCK, "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        a, b = [], []
        time_once(base_bin, cfg)                # warm the page cache
        for r in range(cfg["bench_rounds"]):
            pair = [(base_bin, a), (cand_bin, b)]
            if r % 2:
                pair.reverse()
            for binary, acc in pair:
                idle_wait()
                acc.append(time_once(binary, cfg))
    ma, mb = statistics.median(a), statistics.median(b)
    wins = sum(1 for x, y in zip(a, b) if y < x) / len(a)
    gain = (ma - mb) / ma * 100
    ok = gain >= cfg["accept_pct"] and wins >= cfg["accept_win_frac"]
    return {"base_ms": [round(x) for x in a], "cand_ms": [round(x) for x in b],
            "base_med": round(ma, 1), "cand_med": round(mb, 1), "gain_pct": round(gain, 2),
            "win_frac": round(wins, 2), "accepted": ok}


# --------------------------------------------------------------- code context

def extract_region(text, spec):
    """spec: {"function": name} (C or asm) or {"start": regex, "end": regex}."""
    lines = text.splitlines(True)
    if "function" in spec:
        name = re.escape(spec["function"])
        # asm: from the comment block before `.globl name` to `.size name`
        for i, l in enumerate(lines):
            if re.match(r"\s*\.globl\s+%s\s*$" % name, l):
                s = i
                while s > 0 and (lines[s - 1].startswith("#") or lines[s - 1].strip() == ""):
                    s -= 1
                for j in range(i, len(lines)):
                    if re.match(r"\s*\.size\s+%s\b" % name, lines[j]):
                        return "".join(lines[s:j + 1])
        # C: a definition starting in column 0, to the closing brace in column 0
        for i, l in enumerate(lines):
            if re.match(r"^[A-Za-z_].*\b%s\s*\(" % name, l) and not l.rstrip().endswith(";"):
                s = i
                while s > 0 and lines[s - 1].lstrip().startswith(("/*", "*", "//")):
                    s -= 1
                for j in range(i, len(lines)):
                    if lines[j].startswith("}"):
                        return "".join(lines[s:j + 1])
        return None
    m = re.search(spec["start"], text, re.M)
    if not m:
        return None
    e = re.search(spec["end"], text[m.end():], re.M)
    end = m.end() + e.end() if e else len(text)
    return text[m.start():end]


SYSTEM = """You are a performance-engineering intern on a native x86-64 data pipeline: C plus GNU assembler in Intel syntax. It processes CSV event logs into Parquet. Your mentor assigns you small, well-scoped tasks. Your patch is tested automatically: build, unit tests, 36 fixtures, the official dev answer, adversarial fuzz data and a 20M-event golden output. Then it is benchmarked against the current version. Only patches that keep every output identical AND are measurably faster are kept.

Hard rules (a violation means rejection):
- Output must stay byte-for-byte identical in meaning. Never change semantics, rounding, tie-breaking or validation.
- src/evx.h is the ABI contract: struct offsets used by the .s files are pinned by _Static_assert. If you change a layout, change both sides.
- Assembly: Intel syntax (.intel_syntax noprefix). Use only zmm16-zmm31 / ymm16+ / xmm16+ (never zmm0-15). Follow the SysV ABI: preserve rbx, rbp, r12-r15 and keep rsp 16-byte aligned at calls. evx_memeq/evx_keq/evx_khash clobber only rax/r11 (memeq also k1, k2, zmm16, zmm17). Never read past a buffer end: tail loads must be masked loads.
- Target CPU: Intel Cascade Lake (AVX-512F/BW/VL/DQ, BMI2; no VBMI, so no vpermb). The binary must stay fully static.
- At most 8 worker threads. No new files, libraries or dependencies.
- The program must print nothing on success.
- Make the smallest change that achieves the goal. Do not reformat or rename unrelated code.

Answer format (strict):
1. At most 6 lines: what you change and why it is faster.
2. One or more edit blocks, exactly like this:
<<<<<<< SEARCH path/to/file
exact lines copied from the code shown (enough to be unique)
=======
replacement lines
>>>>>>> REPLACE
The SEARCH text must match the file exactly (spaces included) and appear exactly once. Edit only the files the task allows. Do not wrap edit blocks in ```diff or markdown fences. Do not prefix lines with extra diff spaces."""


def standing_instructions():
    if os.path.exists(INSTR):
        t = open(INSTR).read().strip()
        if t:
            return "## Mentor's standing instructions (always apply)\n" + t + "\n"
    return ""


def build_prompt(task, root, history, cfg):
    parts = ["# Task: %s\n\n%s\n" % (task["name"], task["goal"])]
    si = standing_instructions()
    if si:
        parts.append(si)
    if task.get("hints"):
        parts.append("## Mentor hints\n" + "\n".join("- " + h for h in task["hints"]) + "\n")
    parts.append("## Files you may edit: " + ", ".join(task["editable"]) + "\n")
    budget = cfg["max_context_chars"]
    for reg in task.get("context", []):
        path = os.path.join(root, reg["file"])
        text = open(path).read()
        body = extract_region(text, reg) if ("function" in reg or "start" in reg) else text
        if body is None:
            body = text
        if len(body) > budget:
            body = body[:budget] + "\n... (truncated)\n"
        budget -= len(body)
        label = reg.get("function") or reg.get("start") or "whole file"
        parts.append("## %s (%s)\n```\n%s```\n" % (reg["file"], label, body))
    if history:
        parts.append("## Your earlier attempts on this task (learn from them; do not repeat a failed idea)")
        for h in history[-4:]:
            parts.append("- attempt %d: %s\n  idea: %s\n  result: %s" % (
                h["attempt"], h["status"], h.get("summary", "")[:300], h.get("feedback", "")[:1500]))
    parts.append("\nGive your answer now in the strict format.")
    return "\n".join(parts)


def ask_llm(prompt, cfg, attempt):
    body = {
        "model": cfg["llm_model"],
        "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}],
        "temperature": cfg["temperature"],
        "max_tokens": cfg["max_tokens"],
        "seed": 1000 + attempt,
        "chat_template_kwargs": {"reasoning_effort": cfg["reasoning_effort"]},
    }
    req = urllib.request.Request(cfg["llm_url"], data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=cfg["llm_timeout_s"]) as r:
        resp = json.loads(r.read())
    return resp["choices"][0]["message"].get("content") or ""


def ask_llm_patient(prompt, cfg, attempt, system=None):
    """Like ask_llm, but waits for the server instead of failing."""
    wait = 10
    while True:
        try:
            if system is None:
                return ask_llm(prompt, cfg, attempt)
            return ask_llm_raw(system, prompt, cfg, attempt)
        except urllib.error.HTTPError as ex:
            if 400 <= ex.code < 500:          # our request is wrong: retrying cannot help
                raise
            heartbeat(state="waiting for llama-server", error=str(ex)[:200])
            log("  llama-server HTTP %d; retrying in %ds" % (ex.code, wait))
            time.sleep(wait)
            wait = min(wait * 2, 300)
        except Exception as ex:
            heartbeat(state="waiting for llama-server", error=str(ex)[:200])
            log("  llama-server unreachable (%s); retrying in %ds" % (str(ex)[:80], wait))
            time.sleep(wait)
            wait = min(wait * 2, 300)


def ask_llm_raw(system, prompt, cfg, attempt):
    body = {
        "model": cfg["llm_model"],
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": prompt}],
        "temperature": max(cfg["temperature"], 0.8),
        "max_tokens": cfg["max_tokens"],
        "seed": 5000 + attempt,
        "chat_template_kwargs": {"reasoning_effort": cfg["reasoning_effort"]},
    }
    req = urllib.request.Request(cfg["llm_url"], data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=cfg["llm_timeout_s"]) as r:
        resp = json.loads(r.read())
    return resp["choices"][0]["message"].get("content") or ""


def heartbeat(**kw):
    with HB_LOCK:
        st = jload(STATUS, {})
        st.update(kw, updated=time.strftime("%Y-%m-%d %H:%M:%S"), pid=os.getpid())
        json.dump(st, open(STATUS, "w"), indent=2)


def jload(path, default):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return default


BLOCK = re.compile(r"<<<<<<<[ \t]*(?:SEARCH[ \t]+)?(\S+)[ \t]*\n(.*?)\n=======[ \t]*\n(.*?)\n?>>>>>>> REPLACE", re.S)


def apply_blocks(answer, root, editable):
    """Returns (ok, message, files_changed)."""
    blocks = BLOCK.findall(answer)
    if not blocks:
        return False, "no edit blocks found in your answer (use the exact SEARCH/REPLACE format)", []
    changed = set()
    for path, old, new in blocks:
        path = path.strip().lstrip("./")
        old = re.sub(r"^```\w*\n", "", old)
        old = re.sub(r"\n```$", "", old)
        new = re.sub(r"^```\w*\n", "", new)
        new = re.sub(r"\n```$", "", new)
        if path not in editable:
            return False, "you edited %s, which this task does not allow" % path, []
        full = os.path.join(root, path)
        text = open(full).read()
        n = text.count(old)
        if n != 1 and all(not l or l.startswith(" ") for l in old.splitlines()):
            old_s = "\n".join(l[1:] if l.startswith(" ") else l for l in old.splitlines())
            if text.count(old_s) == 1:
                old = old_s
                if all(not l or l.startswith(" ") for l in new.splitlines()):
                    new = "\n".join(l[1:] if l.startswith(" ") else l for l in new.splitlines())
                n = 1
        if n != 1:
            # tolerate trailing-whitespace differences only
            pat = re.compile(r"[ \t]*\n".join(re.escape(l.rstrip()) for l in old.split("\n")))
            ms = list(pat.finditer(text))
            if len(ms) != 1:
                return False, "SEARCH text for %s matched %d times (must be exactly once):\n%s" % (
                    path, n if n else len(ms), old[:400]), []
            text = text[:ms[0].start()] + new + text[ms[0].end():]
        else:
            text = text.replace(old, new, 1)
        open(full, "w").write(text)
        changed.add(path)
    return True, "%d block(s) applied" % len(blocks), sorted(changed)


def summary_of(answer):
    head = answer.split("<<<<<<<")[0].strip()
    return " ".join(head.split())[:400]


# -------------------------------------------------------------------- commands

def cmd_init(args):
    cfg = cfg_load()
    os.makedirs(STATE, exist_ok=True)
    os.makedirs(INBOX, exist_ok=True)
    cfgp = os.path.join(STATE, "config.json")
    if not os.path.exists(cfgp):
        json.dump(DEFAULT_CFG, open(cfgp, "w"), indent=2)
        log("wrote %s - fill in the dataset paths and rerun init" % cfgp)
        return 1
    log("base tree from the repo working copy")
    copy_tree(REPO, BEST)
    rc, out, warns = build(BEST)
    if rc != 0:
        log("base does not build:\n" + tail(out))
        return 1
    shutil.copy2(os.path.join(BEST, "src", "main"), BASEBIN)
    json.dump({"warnings": warns, "repo_hash": repo_hash()}, open(os.path.join(STATE, "base.json"), "w"))
    os.makedirs(GOLD, exist_ok=True)
    for name in ("fuzz", "big"):
        if cfg[name]:
            log("golden output: " + name)
            rc, out = run_prog(BASEBIN, cfg[name], os.path.join(GOLD, name), cfg)
            if rc != 0:
                log("base binary failed on %s" % name)
                return 1
    log("sanity: gates on the base tree")
    ok, gate, msg = gates(BEST, cfg, warns)
    log("base gates: %s %s %s" % ("PASS" if ok else "FAIL", gate, msg))
    return 0 if ok else 1


def load_task(name):
    p = name if os.path.exists(name) else os.path.join(TASKS, name if name.endswith(".json") else name + ".json")
    t = json.load(open(p))
    t.setdefault("name", os.path.splitext(os.path.basename(p))[0])
    t.setdefault("attempts", None)
    return t


# ---------------------------------------------------------- concurrency
#
# Four experiments run at once, each in its own scratch tree. Model calls and
# correctness gates may overlap with each other. A benchmark or a profile is
# EXCLUSIVE: it waits for all activity to drain, and no new activity starts
# until it finishes, so timings are never disturbed by our own work.
# Lock order: exclusive -> BEST_LOCK. Nothing takes exclusive inside activity.

BEST_LOCK = threading.RLock()     # guards BEST, BASEBIN, BEST_GEN
BEST_GEN = [0]                    # bumps every time the best tree changes
HB_LOCK = threading.Lock()
LEDGER_LOCK = threading.Lock()
FUNC_LOCK = threading.Lock()      # guards the daemon's shared state dict
BENCH_LOCK = threading.Lock()
GATE_SEM = None                   # limits concurrent full-size gate runs (RAM)
_CV = threading.Condition()
_ACT = {"busy": 0, "bench_wait": 0}


class activity:
    """LLM calls and gate runs: any number at once, never during a benchmark."""

    def __enter__(self):
        with _CV:
            while _ACT["bench_wait"]:
                _CV.wait()
            _ACT["busy"] += 1
        return self

    def __exit__(self, *exc):
        with _CV:
            _ACT["busy"] -= 1
            _CV.notify_all()


class exclusive:
    """Benchmarks and profiles: drain all activity, block new activity."""

    def __enter__(self):
        with _CV:
            _ACT["bench_wait"] += 1
            while _ACT["busy"]:
                _CV.wait()
        BENCH_LOCK.acquire()
        return self

    def __exit__(self, *exc):
        BENCH_LOCK.release()
        with _CV:
            _ACT["bench_wait"] -= 1
            _CV.notify_all()


class RestWindows:
    """Benchmarks queue up; every bench_window_s (or once bench_batch are
    waiting) development pauses, all queued benchmarks run back to back on a
    quiet machine, then development resumes."""

    def __init__(self):
        self.q = []
        self.cv = threading.Condition()
        self.last = time.time()

    def run(self, fn):
        box, ev = {}, threading.Event()
        with self.cv:
            self.q.append((fn, box, ev))
            self.cv.notify_all()
        ev.wait()
        if "err" in box:
            raise box["err"]
        return box["res"]

    def loop(self):
        while True:
            cfg = cfg_load()
            with self.cv:
                while not (self.q and (time.time() - self.last >= cfg["bench_window_s"]
                                       or len(self.q) >= cfg["bench_batch"])):
                    self.cv.wait(timeout=5)
                batch, self.q = self.q, []
            log("rest window: %d benchmark(s); development paused" % len(batch))
            heartbeat(state="rest window", queued=len(batch))
            with exclusive():
                for fn, box, ev in batch:
                    try:
                        box["res"] = fn()
                    except Exception as ex:
                        box["err"] = ex
                    ev.set()
            self.last = time.time()
            log("rest window over; development resumes")


WINDOWS = None


def in_window(fn):
    if WINDOWS is None:               # `run` without the daemon: measure right away
        with exclusive():
            return fn()
    return WINDOWS.run(fn)


def current_gen():
    with BEST_LOCK:
        return BEST_GEN[0]


def snapshot_best(wd):
    """Copy the best tree into wd; returns the generation it belongs to."""
    with BEST_LOCK:
        copy_tree(BEST, wd)
        return BEST_GEN[0]


def texts_of(root, files):
    return {f: open(os.path.join(root, f)).read() for f in files if os.path.exists(os.path.join(root, f))}


def diff_against(orig, root, files):
    out = []
    for f in sorted(files):
        a = orig.get(f, "").splitlines(True)
        b = open(os.path.join(root, f)).read().splitlines(True)
        out.extend(difflib.unified_diff(a, b, "a/" + f, "b/" + f))
    return "".join(out)


def reapply(diff, wd):
    p = os.path.join(wd, ".cand.diff")
    open(p, "w").write(diff)
    rc, _ = sh(["patch", "-p1", "--forward", "--batch", "-i", p], cwd=wd)
    os.remove(p)
    return rc == 0


def record_stale(rec, msg):
    rec.update(status="stale", feedback="the intern's best changed while this was tested: " + msg)


def work_task(task, cfg, wid=0, patient=False):
    """One task: up to `attempts` candidates, each gated and benchmarked exclusively."""
    base_warn = json.load(open(os.path.join(STATE, "base.json")))["warnings"]
    attempts = task.get("attempts") or cfg["attempts"]
    history, best_gain = [], None
    wd = os.path.join(STATE, "work-%d" % wid)
    log("[w%d] task %s: %d attempts" % (wid, task["name"], attempts))
    for att in range(1, attempts + 1):
        gen = snapshot_best(wd)
        rec = {"task": task["name"], "attempt": att, "worker": wid}
        if task.get("patch"):             # architect patch: no model, same gates and windows
            diff = open(task["patch"]).read()
            rec["summary"] = "architect patch: " + task.get("goal", "")[:300]
            if not reapply(diff, wd):
                rec.update(status="bad_patch", feedback="the architect patch does not apply to the best")
                ledger(rec)
                break
            files = re.findall(r"^\+\+\+ b/(\S+)", diff, re.M)
        else:
            prompt = build_prompt(task, wd, history, cfg)
            t0 = time.time()
            heartbeat(state="asking the model", worker=wid, task=task["name"], attempt=att)
            try:
                with activity():
                    answer = ask_llm_patient(prompt, cfg, att) if patient else ask_llm(prompt, cfg, att)
            except Exception as ex:
                log("  [w%d] LLM error: %s" % (wid, ex))
                rec.update(status="llm_error", feedback=str(ex))
                ledger(rec)
                time.sleep(10)
                continue
            rec["llm_s"] = round(time.time() - t0, 1)
            rec["summary"] = summary_of(answer)
            orig = texts_of(wd, task["editable"])
            ok, msg, files = apply_blocks(answer, wd, task["editable"])
            if not ok:
                log("  [w%d] #%d patch rejected: %s" % (wid, att, msg.splitlines()[0]))
                rec.update(status="bad_patch", feedback=msg)
                history.append(rec)
                ledger(rec)
                continue
            diff = diff_against(orig, wd, files)
        heartbeat(state="gating", worker=wid, task=task["name"], attempt=att)
        with activity():
            ok, gate, glog = gates(wd, cfg, base_warn)
        if not ok:
            log("  [w%d] #%d FAILED gate %s" % (wid, att, gate))
            rec.update(status="gate_fail:" + gate, feedback="gate '%s' failed:\n%s" % (gate, glog))
            history.append(rec)
            ledger(rec)
            continue
        # the best may have moved while we worked: rebase the candidate onto it
        cand_dir, cand_gen = wd, gen
        verdict = None
        for _ in range(4):
            if cand_gen == current_gen():
                break
            rb = os.path.join(STATE, "work-%d-rb" % wid)
            cand_gen = snapshot_best(rb)
            cand_dir = rb
            if not reapply(diff, rb):
                verdict = ("stale", "the patch no longer applies to the new best")
                break
            with activity():
                ok, gate, glog = gates(rb, cfg, base_warn)
            if not ok:
                verdict = ("gate_fail:" + gate, "gate '%s' failed on the new best:\n%s" % (gate, glog))
                break
        else:
            verdict = ("stale", "the best kept changing")
        if verdict:
            if verdict[0] == "stale":
                record_stale(rec, verdict[1])
            else:
                rec.update(status=verdict[0], feedback=verdict[1])
            history.append(rec)
            ledger(rec)
            continue
        cand = os.path.join(STATE, "cand-%d.bin" % wid)
        shutil.copy2(os.path.join(cand_dir, "src", "main"), cand)
        heartbeat(state="queued for the next rest window", worker=wid, task=task["name"], attempt=att)

        def measure(task=task, att=att, rec=rec, diff=diff, cand=cand, cand_dir=cand_dir, cand_gen=cand_gen):
            """Runs inside a rest window (nothing else is running)."""
            if cand_gen != current_gen():
                return None
            b = bench(BASEBIN, cand, cfg)
            if b["accepted"] and cfg.get("confirm", True):
                b2 = bench(BASEBIN, cand, cfg)       # confirm: a second independent run
                b["confirm"] = {"gain_pct": b2["gain_pct"], "win_frac": b2["win_frac"],
                                "accepted": b2["accepted"]}
                b["accepted"] = b2["accepted"]
            if b["accepted"]:
                with BEST_LOCK:
                    n = len([f for f in os.listdir(INBOX) if f.endswith(".patch")]) + 1
                    stem = os.path.join(INBOX, "%03d-%s-a%d" % (n, task["name"], att))
                    open(stem + ".patch", "w").write(diff)
                    open(stem + ".md", "w").write(
                        "# %s, attempt %d: ACCEPTED (pending architect review)\n\n"
                        "Goal: %s\n\nIntern's rationale: %s\n\n"
                        "Benchmark (%s, %s, rest window, interleaved, ms): base median %.1f, candidate median %.1f, "
                        "gain %+.2f%%, paired wins %.0f%%; confirmation: %s\n\n"
                        "base %s\ncand %s\n\nAll gates passed.\n\n```diff\n%s```\n" % (
                            task["name"], att, task["goal"], rec["summary"], cfg["bench"],
                            "docker" if cfg.get("bench_docker") else "native",
                            b["base_med"], b["cand_med"], b["gain_pct"], b["win_frac"] * 100,
                            b.get("confirm"), b["base_ms"], b["cand_ms"], diff))
                    copy_tree(cand_dir, BEST)
                    shutil.copy2(cand, BASEBIN)
                    BEST_GEN[0] += 1
                    acc = jload(ACCEPTED, [])
                    acc.append({"patch": stem + ".patch", "task": task["name"], "gain_pct": b["gain_pct"],
                                "summary": rec["summary"]})
                    json.dump(acc, open(ACCEPTED, "w"), indent=2)
                    b["stem"] = stem
            return b

        b = in_window(measure)
        if b is None:
            record_stale(rec, "the best moved while this waited for the rest window")
            history.append(rec)
            ledger(rec)
            continue
        rec.update(bench=b)
        log("  [w%d] #%d base %.0f ms -> cand %.0f ms (%+.2f%%, wins %.0f%%) %s" % (
            wid, att, b["base_med"], b["cand_med"], b["gain_pct"], b["win_frac"] * 100,
            "ACCEPTED" if b["accepted"] else "not faster"))
        if b["accepted"]:
            stem = b.pop("stem")
            rec.update(status="accepted", patch=stem + ".patch")
            best_gain = b["gain_pct"] if best_gain is None else best_gain + b["gain_pct"]
            heartbeat(last_accept="%s %+.2f%%" % (task["name"], b["gain_pct"]))
            history.append(dict(rec, feedback="accepted: %+.2f%%; try to improve further" % b["gain_pct"]))
        else:
            rec.update(status="not_faster", feedback="correct but not faster: base %.0f ms, yours %.0f ms "
                       "(%+.2f%%), won %.0f%% of rounds; needs >= %.1f%% and >= %.0f%%, confirmed twice" % (
                           b["base_med"], b["cand_med"], b["gain_pct"], b["win_frac"] * 100,
                           cfg["accept_pct"], cfg["accept_win_frac"] * 100))
            history.append(rec)
        ledger(rec)
    if task.get("architect"):
        done = os.path.join(STATE, "done")
        os.makedirs(done, exist_ok=True)
        open(os.path.join(done, task["name"]), "w").write(json.dumps({"gain_pct": best_gain}))
    log("[w%d] task %s finished: %s" % (wid, task["name"],
                                        "accepted %+.2f%%" % best_gain if best_gain else "no win"))
    return best_gain, history


def cmd_run(args):
    cfg = cfg_load()
    if not os.path.exists(BASEBIN):
        log("run `intern.py init` first")
        return 1
    if args.all:
        names = sorted(f for f in os.listdir(TASKS) if f.endswith(".json"))
        names = [n for n in names if not os.path.exists(os.path.join(STATE, "done", n[:-5]))]
    else:
        names = args.tasks
    if args.attempts:
        cfg["attempts"] = args.attempts
    for _ in range(args.rounds):
        for n in names:
            t = load_task(n)
            t["architect"] = True
            work_task(t, cfg)
    return 0


# -------------------------------------------------------- daemon (workers)

def make_self_task(fn, pct, path, idea, cfg, number):
    ctx = [{"file": path, "function": fn}]
    if path.endswith(".s"):
        ctx.append({"file": "src/evx.h"})
    return {
        "name": "self-%s-%d" % (fn, number),
        "goal": "Make `%s` (%s, %.1f%% of all CPU cycles) faster by implementing this idea: %s\n"
                "If the idea turns out to be wrong or unsafe, implement the closest safe variant. "
                "Results must stay identical." % (fn, path, pct, idea),
        "editable": [path],
        "context": ctx,
        "hints": ["Changes worth well under 1% of total time are rejected by the benchmark."],
        "attempts": cfg["self_attempts"],
    }


def fresh_fn_state():
    return {"tried": [], "ideas": [], "fails": 0, "cool_until": 0, "wins": 0, "brewing": False}


def next_task(S, cfg):
    """(task, kind) for the next worker, or (None, None). kind: architect | self."""
    with FUNC_LOCK:
        open_t = sorted(f for f in os.listdir(TASKS) if f.endswith(".json")
                        and not os.path.exists(os.path.join(STATE, "done", f[:-5]))
                        and f not in S["claimed"])
        if open_t:
            S["claimed"].add(open_t[0])
            return load_task(open_t[0]), "architect"
        need_prof = (S["since"] >= cfg["reprofile_every"] or not S["hot"]) and not S["profiling"]
        if need_prof:
            S["profiling"] = True
    if need_prof:
        try:
            hot = hot_functions(cfg)
            log("profile: " + ", ".join("%s %.1f%%" % (f, p) for p, f, _ in hot[:8]))
            with FUNC_LOCK:
                S["hot"], S["since"] = hot, 0
        finally:
            with FUNC_LOCK:
                S["profiling"] = False
    with FUNC_LOCK:
        S["cycle"] += 1
        cyc = S["cycle"]
        cands = []
        for pct, fn, path in S["hot"]:
            st = S["funcs"].setdefault(fn, fresh_fn_state())
            if st["cool_until"] > cyc or st["brewing"]:
                continue
            cands.append((pct / (1 + st["fails"]), fn, path, pct))
        if not cands:
            return None, None
        r = random.uniform(0, sum(c[0] for c in cands))
        for w, fn, path, pct in cands:
            r -= w
            if r <= 0:
                break
        st = S["funcs"][fn]
        if st["ideas"]:
            idea = st["ideas"].pop(0)
            st["tried"].append(idea)
            return make_self_task(fn, pct, path, idea, cfg, len(st["tried"])), "self"
        st["brewing"] = True
        tried = list(st["tried"])
    try:
        with activity():
            ideas = brainstorm(fn, pct, path, tried, cfg, cyc)
    finally:
        with FUNC_LOCK:
            st["brewing"] = False
    with FUNC_LOCK:
        if not ideas:
            st["fails"] += 1
            st["cool_until"] = cyc + 2 ** min(st["fails"], 6)
            return None, None
        st["ideas"] = ideas[1:]
        idea = ideas[0]
        st["tried"].append(idea)
        return make_self_task(fn, pct, path, idea, cfg, len(st["tried"])), "self"


def after_task(S, task, kind, gain, cfg):
    with FUNC_LOCK:
        if kind == "architect":
            S["hot"], S["since"] = [], 10 ** 9          # the profile may have moved
        else:
            fn = task["_fn"]
            st = S["funcs"][fn]
            S["since"] += 1
            if gain:
                st["wins"] += 1
                st["fails"] = 0
                S["since"] = cfg["reprofile_every"]     # re-profile after a win
            else:
                st["fails"] += 1
                if not st["ideas"]:
                    st["cool_until"] = S["cycle"] + 2 ** min(st["fails"], 6)
        json.dump(S["funcs"], open(FUNCS, "w"), indent=2)


def ensure_base_current(cfg):
    """If the architect changed the repo, rebase the best onto it (exclusive)."""
    rh = repo_hash()
    if jload(os.path.join(STATE, "base.json"), {}).get("repo_hash") == rh:
        return
    with exclusive():
        if jload(os.path.join(STATE, "base.json"), {}).get("repo_hash") == rh:
            return
        heartbeat(state="rebasing onto the repo (exclusive)")
        with BEST_LOCK:
            if rebase(cfg):
                BEST_GEN[0] += 1


def daemon_worker(wid, S, cfg0):
    while True:
        try:
            cfg = cfg_load()
            if os.path.exists(PAUSE):
                heartbeat(worker=wid, state="paused")
                time.sleep(30)
                continue
            ensure_base_current(cfg)
            task, kind = next_task(S, cfg)
            if task is None:
                heartbeat(worker=wid, state="idle: every hot function is cooling down")
                time.sleep(20)
                continue
            if kind == "architect":
                task["architect"] = True
            else:
                task["_fn"] = task["name"][len("self-"):].rsplit("-", 1)[0]
            gain, _ = work_task(task, cfg, wid=wid, patient=True)
            after_task(S, task, kind, gain, cfg)
        except Exception as ex:   # never die: log, back off, continue
            import traceback
            log("[w%d] daemon error: %s" % (wid, ex))
            ledger({"task": "daemon", "attempt": 0, "worker": wid, "status": "error",
                    "feedback": traceback.format_exc()[-2000:]})
            heartbeat(worker=wid, state="error, retrying", error=str(ex)[:200])
            time.sleep(30)


def cmd_daemon(args):
    global GATE_SEM
    if not os.path.exists(BASEBIN):
        log("run `intern.py init` first")
        return 1
    cfg = cfg_load()
    GATE_SEM = threading.Semaphore(cfg.get("gate_slots", 2))
    S = {"funcs": jload(FUNCS, {}), "hot": [], "since": 10 ** 9, "cycle": 0,
         "claimed": set(), "profiling": False}
    n = cfg.get("workers", 4)
    global WINDOWS
    WINDOWS = RestWindows()
    threading.Thread(target=WINDOWS.loop, daemon=True).start()
    log("daemon started: pid %d, %d workers, rest window every %ds or %d queued. PAUSE file: %s" % (
        os.getpid(), n, cfg["bench_window_s"], cfg["bench_batch"], PAUSE))
    for wid in range(n):
        threading.Thread(target=daemon_worker, args=(wid, S, cfg), daemon=True).start()
        time.sleep(2)
    try:
        while True:
            time.sleep(5)
    except KeyboardInterrupt:
        log("daemon stopped")
        return 0


def cmd_auto(args):
    """Profile the best binary and write one generic task per hot function."""
    cfg = cfg_load()
    data = os.path.join(STATE, "perf.data")
    out = os.path.join(STATE, "perf_out")
    if os.path.exists(out):
        shutil.rmtree(out)
    env = dict(os.environ, DATA_DIR=cfg["bench"], OUTPUT_DIR=out, NCPU=str(cfg["ncpu"]))
    subprocess.run(["perf", "record", "-q", "-F", "4000", "-o", data, BASEBIN], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rc, rep = sh(["perf", "report", "-i", data, "--stdio", "--sort", "sym"])
    syms = re.findall(r"^\s+([\d.]+)%\s+\[\.\]\s+(\S+)", rep, re.M)
    texts = {f: open(os.path.join(BEST, f)).read() for f in source_files(BEST)}
    made = 0
    for pct, sym in syms:
        if made >= args.top:
            break
        home = [f for f, t in texts.items() if extract_region(t, {"function": sym})]
        if not home:
            continue
        task = {
            "name": "auto-" + sym,
            "goal": "Make `%s` faster. It takes %s%% of all CPU cycles at 20M events. Find one concrete "
                    "inefficiency (a dependency chain, a mispredicted branch, a redundant load or store, "
                    "a missed prefetch, a cheaper instruction sequence) and fix it without changing "
                    "any result." % (sym, pct),
            "editable": [home[0]],
            "context": [{"file": home[0], "function": sym}, {"file": "src/evx.h"}],
            "hints": ["Profile share: %s%%. Changes worth < 1%% of total time are rejected." % pct],
        }
        json.dump(task, open(os.path.join(TASKS, task["name"] + ".json"), "w"), indent=2)
        log("wrote task %s (%s%%, %s)" % (task["name"], pct, home[0]))
        made += 1
    return 0


# -------------------------------------------------------------------- daemon

def repo_hash():
    import hashlib
    h = hashlib.sha256()
    for f in source_files(REPO):
        h.update(f.encode())
        h.update(open(os.path.join(REPO, f), "rb").read())
    return h.hexdigest()[:16]


def rebase(cfg):
    """The architect changed the repo: new base = the repo tree, then the
    intern's accepted patches re-applied where they still apply and pass."""
    log("repo sources changed: rebasing the intern onto them")
    heartbeat(state="rebasing onto the repo")
    copy_tree(REPO, BEST)
    rc, out, warns = build(BEST)
    if rc != 0:
        log("  the repo tree does not build; keeping the old best until it does")
        return False
    json.dump({"warnings": warns, "repo_hash": repo_hash()}, open(os.path.join(STATE, "base.json"), "w"))
    shutil.copy2(os.path.join(BEST, "src", "main"), BASEBIN)
    for name in ("fuzz", "big"):           # the architect's tree defines the reference
        if cfg[name]:
            run_prog(BASEBIN, cfg[name], os.path.join(GOLD, name), cfg)
    kept = []
    for a in jload(ACCEPTED, []):
        work = os.path.join(STATE, "work")
        copy_tree(BEST, work)
        rc, out = sh(["patch", "-p1", "--forward", "--batch", "-i", a["patch"]], cwd=work)
        if rc != 0:
            log("  dropped %s (no longer applies)" % os.path.basename(a["patch"]))
            continue
        ok, gate, _ = gates(work, cfg, warns)
        if not ok:
            log("  dropped %s (fails gate %s on the new base)" % (os.path.basename(a["patch"]), gate))
            continue
        copy_tree(work, BEST)
        shutil.copy2(os.path.join(work, "src", "main"), BASEBIN)
        kept.append(a)
        log("  re-applied %s" % os.path.basename(a["patch"]))
    json.dump(kept, open(ACCEPTED, "w"), indent=2)
    return True


def hot_functions(cfg):
    """perf profile of the best binary -> [(pct, symbol, file)] for our code."""
    heartbeat(state="profiling")
    data = os.path.join(STATE, "perf.data")
    out = os.path.join(STATE, "perf_out")
    if os.path.exists(out):
        shutil.rmtree(out)
    with exclusive():
        env = dict(os.environ, DATA_DIR=cfg["bench"], OUTPUT_DIR=out, NCPU=str(cfg["ncpu"]))
        subprocess.run(["perf", "record", "-q", "-F", "4000", "-o", data, BASEBIN], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    rc, rep_ = sh(["perf", "report", "-i", data, "--stdio", "--sort", "sym"])
    texts = {f: open(os.path.join(BEST, f)).read() for f in source_files(BEST)}
    res = []
    for pct, sym in re.findall(r"^\s+([\d.]+)%\s+\[\.\]\s+(\S+)", rep_, re.M):
        if float(pct) < cfg["min_profile_pct"]:
            continue
        home = [f for f, t in texts.items() if extract_region(t, {"function": sym})]
        if home:
            res.append((float(pct), sym, home[0]))
    return res


BRAIN = """You are a senior performance engineer. You suggest concrete micro-optimisations for one function of a native x86-64 (C plus Intel-syntax GNU asm, Cascade Lake, AVX-512) data pipeline. Every suggestion must keep the output exactly identical. Each one must be implementable as a small local edit of this one function."""


def brainstorm(fn, pct, path, tried, cfg, seed):
    text = open(os.path.join(BEST, path)).read()
    body = extract_region(text, {"function": fn}) or ""
    prompt = ("Function `%s` (%s) takes %.1f%% of all CPU cycles.\n%s\n```\n%s```\n"
              "Already tried, do not repeat:\n%s\n\n"
              "List 5 different, concrete optimisation ideas for this function, one per line, "
              "numbered 1. to 5. Name the exact instructions or lines to change and why it is "
              "faster (fewer uops, fewer branch misses, fewer cache misses, a shorter dependency "
              "chain, better memory-level parallelism). No code.") % (
        fn, path, pct, standing_instructions(), body[:cfg["max_context_chars"]],
        "\n".join("- " + t for t in tried[-20:]) or "- (nothing yet)")
    ans = ask_llm_patient(prompt, cfg, seed, system=BRAIN)
    ideas = [m.strip() for m in re.findall(r"^\s*\d+[.)]\s+(.+)$", ans, re.M)]
    return [i for i in ideas if len(i) > 15][:5]


def cmd_status(args):
    if not os.path.exists(LEDGER):
        print("no attempts yet")
        return 0
    recs = [json.loads(l) for l in open(LEDGER)]
    by = {}
    for r in recs:
        by.setdefault(r["task"], []).append(r)
    for t, rs in by.items():
        acc = [r for r in rs if r["status"] == "accepted"]
        st = {}
        for r in rs:
            st[r["status"].split(":")[0]] = st.get(r["status"].split(":")[0], 0) + 1
        print("%-28s attempts %2d  %s%s" % (t, len(rs), " ".join("%s=%d" % kv for kv in sorted(st.items())),
                                            "  gains " + ", ".join("%+.2f%%" % r["bench"]["gain_pct"] for r in acc)
                                            if acc else ""))
    print("inbox:", INBOX)
    if os.path.exists(STATUS):
        print("daemon:", json.dumps(jload(STATUS, {})))
    return 0


def cmd_check(args):
    cfg = cfg_load()
    base_warn = json.load(open(os.path.join(STATE, "base.json")))["warnings"]
    work = os.path.join(STATE, "work")
    copy_tree(BEST, work)
    ok, gate, msg = gates(work, cfg, base_warn)
    print("PASS" if ok else "FAIL %s\n%s" % (gate, msg))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init")
    r = sub.add_parser("run")
    r.add_argument("tasks", nargs="*")
    r.add_argument("--all", action="store_true")
    r.add_argument("--rounds", type=int, default=1, help="repeat the task list (keep pushing)")
    r.add_argument("--attempts", type=int, default=0, help="override attempts per task")
    a = sub.add_parser("auto")
    a.add_argument("--top", type=int, default=3)
    sub.add_parser("status")
    sub.add_parser("check")
    sub.add_parser("daemon")
    args = ap.parse_args()
    return {"init": cmd_init, "run": cmd_run, "auto": cmd_auto, "status": cmd_status, "check": cmd_check,
            "daemon": cmd_daemon}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
