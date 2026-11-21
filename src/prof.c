/*
 * prof.c - per-phase, per-thread hardware counters (EVX_PROFILE=1).
 *
 * Each worker opens its own perf_event group (user-space only, so it works at
 * perf_event_paranoid = 2): core cycles, instructions, cache misses, branch
 * misses. Every job brackets its work with prof_begin / prof_end; the report
 * prints, per phase: items, summed core cycles over all threads, cycles per
 * item, instructions per cycle, misses per item, and the busiest thread.
 * Without EVX_PROFILE (or without perf permission) every call is a no-op.
 */
#define _GNU_SOURCE

#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "prof.h"

#define NEV 4
#define MAXW 64

static int enabled;
static unsigned nworkers;
static const char *phase_name[PH_COUNT] = {
    "links_parse", "event_parse", "link_unionfind", "dedup", "collect", "group_events",
    "click_parse", "group_clicks", "sessionize", "attribute", "partition",
};

typedef struct {
    uint64_t v[NEV];
    uint64_t items;
    uint64_t calls;
} Acc;

static Acc acc[MAXW][PH_COUNT];
static __thread int grp_fd = -1;
static __thread int grp_ok;
static __thread uint64_t start_v[NEV];

static long perf_open(struct perf_event_attr *a, int group)
{
    return syscall(SYS_perf_event_open, a, 0, -1, group, 0);
}

void prof_init(unsigned nw)
{
    const char *e = getenv("EVX_PROFILE");
    enabled = e && *e && *e != '0';
    nworkers = nw < MAXW ? nw : MAXW;
}

static void open_group(void)
{
    static const uint64_t cfg[NEV] = {
        PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS,
        PERF_COUNT_HW_CACHE_MISSES, PERF_COUNT_HW_BRANCH_MISSES,
    };
    grp_ok = 0;
    for (int i = 0; i < NEV; i++) {
        struct perf_event_attr a;
        memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = PERF_TYPE_HARDWARE;
        a.config = cfg[i];
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        a.read_format = PERF_FORMAT_GROUP;
        long fd = perf_open(&a, i == 0 ? -1 : grp_fd);
        if (fd < 0)
            return;
        if (i == 0)
            grp_fd = (int)fd;
    }
    ioctl(grp_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    grp_ok = 1;
}

static int read_group(uint64_t out[NEV])
{
    uint64_t buf[1 + NEV];
    if (read(grp_fd, buf, sizeof buf) != (ssize_t)sizeof buf)
        return 0;
    for (int i = 0; i < NEV; i++)
        out[i] = buf[1 + i];
    return 1;
}

void prof_begin(unsigned wid, int phase)
{
    (void)wid;
    (void)phase;
    if (!enabled)
        return;
    if (grp_fd < 0)
        open_group();
    if (grp_ok)
        read_group(start_v);
}

void prof_end(unsigned wid, int phase, uint64_t items, uint64_t calls)
{
    if (!enabled || !grp_ok || wid >= MAXW)
        return;
    uint64_t v[NEV];
    if (!read_group(v))
        return;
    for (int i = 0; i < NEV; i++)
        acc[wid][phase].v[i] += v[i] - start_v[i];
    acc[wid][phase].items += items;
    acc[wid][phase].calls += calls;
}

void prof_report(const double *wall_ms)
{
    if (!enabled)
        return;
    fprintf(stderr,
            "\nevx profile (user-space core counters, summed over %u threads)\n"
            "%-15s %9s %12s %11s %13s %6s %10s %10s %11s %8s\n",
            nworkers, "phase", "wall_ms", "items", "cyc/item", "Mcycles", "IPC",
            "miss/item", "brmiss/it", "max_thr_Mc", "imbal");
    for (int p = 0; p < PH_COUNT; p++) {
        uint64_t t[NEV] = {0}, items = 0, mx = 0;
        for (unsigned w = 0; w < nworkers; w++) {
            for (int i = 0; i < NEV; i++)
                t[i] += acc[w][p].v[i];
            items += acc[w][p].items;
            if (acc[w][p].v[0] > mx)
                mx = acc[w][p].v[0];
        }
        if (!t[0])
            continue;
        double avg = (double)t[0] / nworkers;
        fprintf(stderr, "%-15s %9.1f %12llu %11.1f %13.1f %6.2f %10.3f %10.3f %11.1f %8.2f\n",
                phase_name[p], wall_ms ? wall_ms[p] : 0.0, (unsigned long long)items,
                items ? (double)t[0] / (double)items : 0.0, (double)t[0] / 1e6,
                (double)t[1] / (double)t[0], items ? (double)t[2] / (double)items : 0.0,
                items ? (double)t[3] / (double)items : 0.0, (double)mx / 1e6,
                avg > 0 ? (double)mx / avg : 0.0);
    }
}
