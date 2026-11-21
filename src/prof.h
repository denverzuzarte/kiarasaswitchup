/* prof.h - per-phase hardware counter accounting (EVX_PROFILE=1). */
#ifndef PROF_H
#define PROF_H

#include <stdint.h>

enum {
    PH_LINKS, PH_PARSE, PH_UNIONFIND, PH_DEDUP, PH_COLLECT, PH_GROUP,
    PH_CPARSE, PH_CGROUP, PH_SESSION, PH_ATTR, PH_PARTITION, PH_COUNT
};

void prof_init(unsigned nworkers);
void prof_begin(unsigned wid, int phase);
void prof_end(unsigned wid, int phase, uint64_t items, uint64_t calls);
void prof_report(const double *wall_ms);   /* wall_ms[PH_COUNT], may be NULL */

#endif
