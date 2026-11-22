#!/usr/bin/env python3
"""measure.py - reproducible measurement pass for the redesign hand-off.

usage: python3 docs/handoff/measure.py OUT_DIR LABEL=BINARY [LABEL=BINARY ...]
env:   D20=<20M dataset> D60=<60M dataset> RUNS20=7 RUNS60=3 DOCKER_IMAGE=evx_ours:latest

Per binary and dataset, interleaved round-robin (host noise hits all alike):
  - native wall ms (exact: blocking wait, no polling), user+sys CPU ms,
    minor/major page faults, peak RSS
  - docker wall ms with the scorer's flags (scripts/docker_run.sh), the
    container's cgroup cpu.stat (usage, nr_throttled, throttled_usec) and
    memory.peak, sampled while it runs
Writes OUT_DIR/runs.jsonl (one line per run) and OUT_DIR/summary.md.
"""
import json
import os
import resource
import shutil
import statistics
import subprocess
import sys
import threading
import time

OUT = os.path.abspath(sys.argv[1])
BINS = [a.split("=", 1) for a in sys.argv[2:]]
D20 = os.environ.get("D20", "/home/denver/Hackathon/soln/dev20m")
D60 = os.environ.get("D60", "/home/denver/Hackathon/soln/dev60m")
RUNS = {"20M": int(os.environ.get("RUNS20", 7)), "60M": int(os.environ.get("RUNS60", 3))}
IMAGE = os.environ.get("DOCKER_IMAGE", "evx_ours:latest")
os.makedirs(OUT, exist_ok=True)
SCR = os.path.join(OUT, "_scratch")


def native(binary, data):
    out = os.path.join(SCR, "out")
    shutil.rmtree(out, ignore_errors=True)
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    env = dict(os.environ, DATA_DIR=data, OUTPUT_DIR=out, NCPU="8")
    p = subprocess.Popen([binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    t = time.perf_counter()
    _, ru = os.wait4(p.pid, 0)[1:]
    ms = (time.perf_counter() - t) * 1000
    return {"wall_ms": round(ms, 1), "cpu_ms": round((ru.ru_utime + ru.ru_stime) * 1000),
            "user_ms": round(ru.ru_utime * 1000), "sys_ms": round(ru.ru_stime * 1000),
            "minflt": ru.ru_minflt, "majflt": ru.ru_majflt, "maxrss_mib": ru.ru_maxrss // 1024}


def docker(binary, data, mem="24g"):
    out = os.path.join(SCR, "dout")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    os.chmod(out, 0o777)
    cid_file = os.path.join(SCR, "cid")
    if os.path.exists(cid_file):
        os.remove(cid_file)
    cmd = ["docker", "run", "--rm", "--cidfile", cid_file, "--network", "none", "--read-only",
           "--tmpfs", "/tmp:rw,size=64m,mode=1777", "--cap-drop", "ALL",
           "--security-opt", "no-new-privileges", "--pids-limit", "4096",
           "--cpus", "8", "--memory", mem, "--memory-swap", mem, "--user", "1000:1000",
           "-v", data + ":/data:ro", "-v", out + ":/output", "-v", os.path.abspath(binary) + ":/main:ro",
           "-e", "DATA_DIR=/data", "-e", "OUTPUT_DIR=/output", "-e", "NCPU=8", IMAGE, "/main"]
    last = {}
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            try:
                cid = open(cid_file).read().strip()
                base = "/sys/fs/cgroup/system.slice/docker-%s.scope/" % cid
                st = dict(l.split() for l in open(base + "cpu.stat"))
                st["memory_peak"] = open(base + "memory.peak").read().strip()
                last.update(st)
            except (OSError, ValueError):
                pass
            time.sleep(0.02)

    th = threading.Thread(target=sample)
    th.start()
    t = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    err = p.communicate()[1]
    ms = (time.perf_counter() - t) * 1000
    stop.set()
    th.join()
    r = {"wall_ms": round(ms, 1), "rc": p.returncode}
    if p.returncode:
        r["stderr"] = err.decode(errors="replace")[-500:]
    for k in ("usage_usec", "user_usec", "system_usec", "nr_periods", "nr_throttled", "throttled_usec"):
        if k in last:
            r["cg_" + k] = int(last[k])
    if "memory_peak" in last:
        r["cg_memory_peak_mib"] = int(last["memory_peak"]) // 2 ** 20
    return r


def main():
    rows = []
    jl = open(os.path.join(OUT, "runs.jsonl"), "a")
    rows = [json.loads(l) for l in open(os.path.join(OUT, "runs.jsonl")) if '"rc": 125' not in l]
    for ds, data in (("20M", D20), ("60M", D60)):
        if not os.path.isdir(data):
            continue
        for mode in os.environ.get("MODES", "native,docker").split(","):
            for b in BINS:                         # warm-up, not recorded
                (native if mode == "native" else docker)(b[1], data)
            for r in range(RUNS[ds]):
                for label, binary in BINS:
                    rec = (native if mode == "native" else docker)(binary, data)
                    rec.update(dataset=ds, mode=mode, label=label, run=r,
                               time=time.strftime("%H:%M:%S"))
                    rows.append(rec)
                    jl.write(json.dumps(rec) + "\n")
                    jl.flush()
                    print(json.dumps(rec), flush=True)
    shutil.rmtree(SCR, ignore_errors=True)
    lines = ["| dataset | mode | binary | wall ms min / median / max | CPU ms median | peak RSS MiB | "
             "majflt median | cgroup throttled (nr / ms, median) |", "|---|---|---|---|---|---|---|---|"]
    for ds in ("20M", "60M"):
        for mode in ("native", "docker"):
            for label, _ in BINS:
                g = [r for r in rows if r["dataset"] == ds and r["mode"] == mode and r["label"] == label]
                if not g:
                    continue
                w = sorted(r["wall_ms"] for r in g)
                med = lambda k: statistics.median(r[k] for r in g if k in r) if any(k in r for r in g) else "-"
                rss = max((r.get("maxrss_mib") or r.get("cg_memory_peak_mib") or 0) for r in g)
                thr = "%s / %s" % (med("cg_nr_throttled"),
                                   round(med("cg_throttled_usec") / 1000) if mode == "docker" and
                                   any("cg_throttled_usec" in r for r in g) else "-") if mode == "docker" else "-"
                lines.append("| %s | %s | %s | %.0f / %.0f / %.0f | %s | %s | %s | %s |" % (
                    ds, mode, label, w[0], statistics.median(w), w[-1],
                    round(med("cpu_ms")) if mode == "native" else
                    (round(med("cg_usage_usec") / 1000) if any("cg_usage_usec" in r for r in g) else "-"),
                    rss, med("majflt") if mode == "native" else "-", thr))
    open(os.path.join(OUT, "summary.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
