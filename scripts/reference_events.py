#!/usr/bin/env python3
"""
Independent, deliberately naive Python implementation of the events-side rules
(The rules.md), used only for differential testing of the asm engine.

  reference_events.py DATA_DIR OUT_CSV
"""
import calendar
import os
import re
import sys

F1 = re.compile(rb"([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})Z")
F2 = re.compile(rb"([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})([+-])([0-9]{2}):([0-9]{2})")
DIGITS = re.compile(rb"[0-9]+")
DAY = 86_400_000


def split(line, n):
    if line == b"":
        return None
    out = []
    for f in line.split(b","):
        if len(f) >= 2 and f[:1] == b'"' and f[-1:] == b'"' and b'"' not in f[1:-1]:
            out.append(f[1:-1])
        elif b'"' in f:
            return None
        else:
            out.append(f)
    return out if len(out) == n else None


def civil_ms(y, mo, d, h, mi, s):
    if not (1 <= mo <= 12) or h > 23 or mi > 59 or s > 59:
        return None
    dim = [31, 29 if calendar.isleap(y) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][mo - 1]
    if not (1 <= d <= dim):
        return None
    # proleptic Gregorian days from 1970-01-01 (works for year 0 too)
    yy = y - (mo <= 2)
    era = yy // 400
    yoe = yy - era * 400
    mp = mo + 9 if mo <= 2 else mo - 3
    doy = (153 * mp + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    days = era * 146097 + doe - 719468
    return ((days * 86400) + h * 3600 + mi * 60 + s) * 1000


def parse_ts(v):
    m = F1.fullmatch(v)
    if m:
        return civil_ms(*map(int, m.groups()))
    m = F2.fullmatch(v)
    if m:
        g = m.groups()
        base = civil_ms(*map(int, g[:6]))
        oh, om = int(g[7]), int(g[8])
        if base is None or oh > 23 or om > 59:
            return None
        off = (oh * 60 + om) * 60_000
        return base - off if g[6] == b"+" else base + off
    if DIGITS.fullmatch(v):
        x = int(v)
        return x if x <= 2**63 - 1 else None
    return None


def data_lines(path):
    with open(path, "rb") as f:
        data = f.read()
    lines = data.split(b"\n")
    if data.endswith(b"\n"):
        lines.pop()
    return lines[1:]  # header


def csvs(d):
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".csv")) if os.path.isdir(d) else []


def main():
    data, out = sys.argv[1], sys.argv[2]
    man = open(os.path.join(data, "manifest.json"), "rb").read()
    ws = parse_ts(re.search(rb'"window_start"\s*:\s*"([^"]*)"', man).group(1))
    we = parse_ts(re.search(rb'"window_end"\s*:\s*"([^"]*)"', man).group(1))

    cat = {}
    for line in data_lines(os.path.join(data, "products.csv")):
        f = split(line, 2)
        if f and f[0] not in cat:
            cat[f[0]] = f[1]

    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            x = parent[x]
        return x

    for path in csvs(os.path.join(data, "links")):
        for line in data_lines(path):
            f = split(line, 2)
            if not f or not f[0] or not f[1]:
                continue
            a, b = find(f[0]), find(f[1])
            if a != b:
                parent[a] = b
    name = {}
    for x in list(parent):
        r = find(x)
        if r not in name or x < name[r]:
            name[r] = x

    def person(u):
        return name[find(u)] if u in parent else u

    best = {}
    for path in csvs(os.path.join(data, "events")):
        for line in data_lines(path):
            f = split(line, 7)
            if not f or not f[0]:
                continue
            ing = parse_ts(f[3])
            if ing is None:
                ing = 2**63 - 1
            if f[0] not in best or ing < best[f[0]][0]:
                best[f[0]] = (ing, f)

    per = {}
    for ing, f in best.values():
        eid, user, ts, _, typ, prod, price = f
        if not user or not prod or typ not in (b"view", b"cart", b"purchase"):
            continue
        t = parse_ts(ts)
        if t is None or not (ws <= t < we) or ing - t > DAY:
            continue
        cents = 0
        if typ == b"purchase":
            if not DIGITS.fullmatch(price) or not (1 <= int(price) <= 10_000_000):
                continue
            cents = int(price)
        per.setdefault(person(user), []).append((t, cents, cat.get(prod, b"unknown")))

    rows = []
    for p, evs in per.items():
        evs.sort(key=lambda e: e[0])
        days = {}
        prev = None
        cur = None
        for t, cents, c in evs:
            if prev is None or t - prev > 1_800_000:
                d = t // DAY
                cur = days.setdefault(d, {"s": 0, "e": 0, "r": 0, "c": {}})
                cur["s"] += 1
            prev = t
            cur["e"] += 1
            if cents:
                cur["r"] += cents
                cur["c"][c] = cur["c"].get(c, 0) + cents
        for d, a in days.items():
            top = ""
            if a["c"]:
                top = min(a["c"].items(), key=lambda kv: (-kv[1], kv[0]))[0]
                top = top.decode("utf-8", "surrogateescape")
            rows.append((p.decode("utf-8", "surrogateescape"), d, a["s"], a["e"], a["r"], top))

    import datetime
    epoch = datetime.date(1970, 1, 1)
    with open(out, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write("user_id,day,sessions,events,revenue_cents,top_category\n")
        for u, d, s, e, r, top in rows:
            def q(x):
                return '"' + x.replace('"', '""') + '"' if any(ch in x for ch in ',"\n\r') else x
            f.write(f"{q(u)},{epoch + datetime.timedelta(days=d)},{s},{e},{r},{q(top)}\n")


if __name__ == "__main__":
    main()
