#!/usr/bin/env python3
"""
Adversarial dataset generator for differential testing of the events engine.

  fuzz_events.py OUT_DIR SEED [EVENTS]

It targets the engine's implementation edges rather than the generator's
distribution: lines ending on 64-byte block boundaries, fields > 64 bytes,
files without a trailing newline, empty and header-only files, multi-MB
files (split into several byte-range tasks), duplicates across files and
tasks, whale persons (radix-sort path), windows longer than 2^32 ms and not
starting at midnight, equal timestamps, exact 30-minute gaps, CRLF, quotes,
every timestamp form and every dirt class of the rules.
"""
import os
import random
import sys
from datetime import datetime, timedelta, timezone

DAY = 86_400_000


def iso(ms, rnd):
    """Render epoch ms as F1, F2 or F3 (F3 when it has milliseconds)."""
    if ms % 1000 or rnd.random() < 0.3:
        s = str(ms)
        return "0" * rnd.choice([0, 0, 0, 1, 5, 30]) + s
    dt = datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
    if rnd.random() < 0.6:
        return dt.strftime("%Y-%m-%dT%H:%M:%SZ")
    off = rnd.randint(-23 * 60 - 59, 23 * 60 + 59)
    local = dt + timedelta(minutes=off)
    sign = "+" if off >= 0 else "-"
    a = abs(off)
    return local.strftime("%Y-%m-%dT%H:%M:%S") + f"{sign}{a // 60:02d}:{a % 60:02d}"


BAD_TS = ["", "2026-10-03 10:00:00", "2026-10-03T10:00:00z", "2026-10-03T10:00:00",
          "2026-10-03T10:00:00.000Z", "1.79e12", "-5", "2026-02-29T10:00:00Z",
          "2026-13-01T00:00:00Z", "2026-10-03T24:00:00Z", "2026-10-03T10:00:60Z",
          "2026-10-03T10:00:00+24:00", "2026-10-03T10:00:00+05:60", "2026-10-03T10:00:00+0200",
          "179102160000000000000", "86400000", "٢٥٠", "２026-10-03T10:00:00Z"]


def quote(v, rnd):
    return f'"{v}"' if rnd.random() < 0.2 else v


def main():
    out, seed = sys.argv[1], int(sys.argv[2])
    n_events = int(sys.argv[3]) if len(sys.argv) > 3 else 200_000
    rnd = random.Random(seed)
    os.makedirs(os.path.join(out, "events"), exist_ok=True)
    os.makedirs(os.path.join(out, "links"), exist_ok=True)

    # window: random start (sometimes not at midnight), 14 days or > 2^32 ms
    start = datetime(2025, 1, 1, tzinfo=timezone.utc) + timedelta(days=rnd.randint(0, 1400))
    if seed % 3 == 1:
        start += timedelta(hours=rnd.randint(1, 23), minutes=rnd.randint(0, 59))
    days = 60 if seed % 2 else 14
    ws = int(start.timestamp() * 1000)
    we = ws + days * DAY
    with open(os.path.join(out, "manifest.json"), "w") as f:
        f.write('{"window_start": "%s", "window_end": "%s"}\n' % (
            start.strftime("%Y-%m-%dT%H:%M:%SZ"),
            (start + timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")))

    cats = ["shoes", "books", "Zeta", "alpha", "unknowm", "unknowx", "a b", "été"]
    prods = [f"p{i}" for i in range(60)] + ["P" * 70]
    with open(os.path.join(out, "products.csv"), "w") as f:
        f.write("product_id,category\n")
        for p in prods:
            f.write(f"{p},{rnd.choice(cats)}\n")

    # people with several ids; long ids exercise > 64-byte compares
    people = []
    for i in range(400):
        k = rnd.choice([1, 1, 1, 2, 2, 3, 5])
        ids = [f"u{i}_{j}" for j in range(k)]
        if i % 50 == 0:
            ids.append("L" * rnd.randint(60, 140) + str(i))
        if i % 37 == 0:
            ids.append(f"U{i}_0")
        if i % 41 == 0:
            ids.append(f" u{i}_0")
        people.append(ids)
    link_lines = []
    for ids in people:
        for j in range(1, len(ids)):
            a, b = ids[rnd.randrange(j)], ids[j]
            if rnd.random() < 0.5:
                a, b = b, a
            link_lines.append(f"{quote(a, rnd)},{quote(b, rnd)}")
            if rnd.random() < 0.08:
                link_lines.append(f"{b},{a}")
    link_lines += ["u1_0,", ",u2_0", "u3_0,u4_0,u5_0", 'u3_0,u6_0"', "", "u7_0", "u8_0,u8_0",
                   "ghost_a,u9_0", "AAA_only_link,u10_0"]
    rnd.shuffle(link_lines)
    third = len(link_lines) // 3
    for fi, chunk in enumerate([link_lines[:third], link_lines[third:2 * third], link_lines[2 * third:]]):
        with open(os.path.join(out, "links", f"part-{fi:05d}.csv"), "w", newline="") as f:
            f.write("user_id_a,user_id_b\n" + "\n".join(chunk) + ("\n" if fi != 1 else ""))
    with open(os.path.join(out, "links", "part-00003.csv"), "w") as f:
        f.write("user_id_a,user_id_b")  # header only, no newline

    # events
    rows = []
    eid = 0
    whale = people[7]
    for _ in range(n_events):
        eid += 1
        ids = whale if rnd.random() < 0.15 else rnd.choice(people)
        user = rnd.choice(ids)
        r = rnd.random()
        if r < 0.002:
            t = ws + rnd.choice([0, -1, days * DAY - 1, days * DAY])
        else:
            t = ws + rnd.randrange(days * DAY)
        # cluster: equal timestamps and exact 30:00.000 / 30:00.001 gaps
        if rows and rnd.random() < 0.1:
            t = rows[-1][2] + rnd.choice([0, 0, 1_800_000, 1_800_001, 1_799_999, 1])
        typ = rnd.choices(["view", "cart", "purchase"], [70, 20, 10])[0]
        price = str(rnd.randint(1, 50_000)) if typ == "purchase" else rnd.choice(["", "", "-5", "x"])
        prod = rnd.choice(prods)
        ing = t + rnd.choice([1000, 60_000, 3_600_000])
        rows.append([f"e{eid}", user, t, ing, typ, prod, price])

    lines = []
    for e in rows:
        eid_s, user, t, ing, typ, prod, price = e
        ts_s = iso(t, rnd)
        ing_s = datetime.fromtimestamp(ing // 1000, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        r = rnd.random()
        if r < 0.01:
            ts_s = rnd.choice(BAD_TS)
        elif r < 0.02:
            ing_s = datetime.fromtimestamp((t + DAY + rnd.choice([0, 1000, -1000, 5000])) // 1000,
                                           tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        elif r < 0.03:
            typ = rnd.choice(["Purchase", "cart ", "VIEW", "click", ""])
        elif r < 0.04 and typ == "purchase":
            price = rnd.choice(["0", "10000001", "19.99", "+5", "", "0250", "00000000000000000000123",
                                "10000000", "２５０", "1000\r"])
        elif r < 0.045:
            prod = rnd.choice(["", "p999", "P1", " p1"])
        elif r < 0.05:
            user = rnd.choice(["", "U1_0", " u1_0", "u1_0 "])
        fields = [eid_s, user, ts_s, ing_s, typ, prod, price]
        line = ",".join(quote(v, rnd) for v in fields)
        r = rnd.random()
        if r < 0.003:
            line = line + ",extra"
        elif r < 0.006:
            line = line.rsplit(",", 1)[0]
        elif r < 0.008:
            line = line.replace(",", ',"', 1)
        elif r < 0.009:
            line = ""
        elif r < 0.010:
            line = "," + line.split(",", 1)[1]          # empty event_id
        elif r < 0.012:
            line = line + "\r"
        elif r < 0.014:
            # pad so the '\n' lands at block offset 63 or 0 (line start relative)
            pad = (-(len(line.encode()) + rnd.choice([0, 1]))) % 64
            if pad and typ != "purchase":
                line = line + "x" * pad if fields[6] == "" else line
        lines.append(line)
        orig = len(lines) - 1
        # duplicates: later copies (sometimes other user / other file)
        if rnd.random() < 0.04:
            # distinct ingest per copy: the rules guarantee equal-ingest copies
            # are identical, so ties between different copies must not occur
            for delay in rnd.sample(range(1, 601), rnd.randint(1, 3)):
                d = list(fields)
                d[3] = datetime.fromtimestamp(ing // 1000 + delay,
                                              tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
                if rnd.random() < 0.1:
                    d[1] = rnd.choice(rnd.choice(people))
                if rnd.random() < 0.1:
                    d[2] = iso(t, rnd)
                lines.append(",".join(quote(v, rnd) for v in d))
        # an invalid earliest copy followed by a valid later one
        if rnd.random() < 0.005:
            d = list(fields)
            lines[orig] = ",".join([d[0], d[1], "bad-ts", d[3], d[4], d[5], d[6]])
            d[3] = datetime.fromtimestamp(ing // 1000 + 1000, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            lines.append(",".join(d))
    rnd.shuffle(lines)

    # file sizes: one big (multi-MB -> several tasks), several small,
    # one without trailing newline, one header-only, one completely empty
    header = "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents"
    cut = [0, int(len(lines) * 0.7), int(len(lines) * 0.85), int(len(lines) * 0.95), len(lines)]
    for fi in range(4):
        part = lines[cut[fi]:cut[fi + 1]]
        body = header + "\n" + "\n".join(part)
        if fi != 2:
            body += "\n"
        with open(os.path.join(out, "events", f"part-{fi:05d}.csv"), "w", newline="") as f:
            f.write(body)
    with open(os.path.join(out, "events", "part-00004.csv"), "w") as f:
        f.write(header + "\n")
    open(os.path.join(out, "events", "part-00005.csv"), "w").close()

    # ---- campaigns and clicks -------------------------------------------------
    os.makedirs(os.path.join(out, "clicks"), exist_ok=True)
    channels = ["search", "social", "email", "Display", "a,b"[:1]]
    camps = [f"c_{i}" for i in range(40)]
    with open(os.path.join(out, "campaigns.csv"), "w") as f:
        f.write("campaign_id,channel\n")
        for i, c in enumerate(camps):
            f.write(f"{quote(c, rnd)},{channels[i % len(channels)]}\n")
    W7 = 7 * DAY
    tie_ids = ["c10", "c9", "C1", "c1", " c", "c", "\u00e9x", "k", "k0"]
    starts = [r[2] for r in rows]            # event times (session-start proxies)
    clines = []
    kid = 0
    for _ in range(n_events // 4):
        kid += 1
        ids = rnd.choice(people)
        user = rnd.choice(ids)
        t0 = rnd.choice(starts)
        r = rnd.random()
        if r < 0.15:                          # attribution boundaries around an event
            t = t0 + rnd.choice([0, -1, 1, -W7, -W7 - 1, -W7 + 1])
        elif r < 0.17:                        # click-window edges
            t = rnd.choice([ws - W7, ws - W7 - 1, we - 1, we])
        else:
            t = t0 - rnd.randrange(0, 9 * DAY)
        cid = f"k{kid}"
        campaign = rnd.choice(camps[:6] if rnd.random() < 0.3 else camps)   # whale campaigns
        if rnd.random() < 0.03:
            campaign = rnd.choice(["c_999", "C_1", " c_1", "c_1 ", ""])
        if rnd.random() < 0.01:
            user = rnd.choice(["", "U1_0", "ghost_a", "AAA_only_link", "nobody_has_events"])
        ts_s = iso(t, rnd) if t >= 0 else str(t)
        if rnd.random() < 0.005:
            ts_s = rnd.choice(BAD_TS + [ts_s + "\r"])
        fields = [cid, user, campaign, ts_s]
        if rnd.random() < 0.004:
            fields[0] = ""
        clines.append(",".join(quote(v, rnd) for v in fields))
        if rnd.random() < 0.05:               # same-millisecond tie group, tricky ids
            for tid in rnd.sample(tie_ids, rnd.randint(1, 4)):
                # unique per group ("kid:" prefix) so a click_id never names two
                # different clicks; within the group the ids differ only after
                # the prefix: true-prefix, case, space and non-ASCII orderings
                clines.append(",".join([f"{kid}:{tid}", user, rnd.choice(camps), ts_s]))
        if rnd.random() < 0.02:               # duplicate, quoted differently
            clines.append(",".join(f'"{v}"' for v in fields))
        r = rnd.random()
        if r < 0.003:
            clines.append(clines[-1] + ",x")
        elif r < 0.005:
            clines.append(clines[-1].replace(",", ',"', 1))
        elif r < 0.006:
            clines.append("")
    rnd.shuffle(clines)
    cheader = "click_id,user_id,campaign_id,ts"
    cut = [0, int(len(clines) * 0.6), int(len(clines) * 0.9), len(clines)]
    for fi in range(3):
        body = cheader + "\n" + "\n".join(clines[cut[fi]:cut[fi + 1]])
        if fi != 1:
            body += "\n"
        with open(os.path.join(out, "clicks", f"part-{fi:05d}.csv"), "w", newline="") as f:
            f.write(body)
    with open(os.path.join(out, "clicks", "part-00003.csv"), "w") as f:
        f.write(cheader + "\n")


if __name__ == "__main__":
    main()
