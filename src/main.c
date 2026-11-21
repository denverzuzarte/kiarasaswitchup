/*
 * main.c - C coordinator of the events-only pipeline.
 *
 * One process, a fixed pthread pool (NCPU workers), explicit phase barriers:
 *   P0 dictionaries -> P1 parse -> P2 dedup -> P3 collect -> P4 prefix sums
 *   -> P5 group -> P6 sessionize -> P7 Parquet.
 * All event semantics live in the assembly engine (src/evx_*.s); this file
 * only owns memory, files, scheduling, barriers and reporting.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "prof.h"

#define MAX_WORKERS 64
#define CPU_BUDGET 8      /* never run more than 8 worker threads */

/* ------------------------------------------------------------- utilities */

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int map_file(const char *path, Mapped *m)
{
    m->data = NULL;
    m->size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror(path);
        close(fd);
        return -1;
    }
    if (st.st_size > 0) {
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            perror(path);
            close(fd);
            return -1;
        }
        madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL | MADV_WILLNEED);
        m->data = p;
        m->size = (size_t)st.st_size;
    }
    close(fd);
    return 0;
}

void unmap_file(Mapped *m)
{
    if (m->data)
        munmap((void *)m->data, m->size);
    m->data = NULL;
    m->size = 0;
}

void *big_alloc(size_t bytes)
{
    if (bytes == 0)
        bytes = 4096;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    madvise(p, bytes, MADV_HUGEPAGE);
    return p;
}

void big_free(void *p, size_t bytes)
{
    if (p)
        munmap(p, bytes ? bytes : 4096);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int list_csv(const char *dir, char ***names_out)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    size_t n = 0, cap = 0;
    char **names = NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".csv") != 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            char **nn = realloc(names, cap * sizeof *nn);
            if (!nn) {
                closedir(d);
                return -1;
            }
            names = nn;
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n)
        qsort(names, n, sizeof *names, name_cmp);
    *names_out = names;
    return (int)n;
}

static uint64_t pow2_at_least(uint64_t n)
{
    uint64_t s = 1024;
    while (s < n)
        s <<= 1;
    return s;
}

/* ----------------------------------------------------------- thread pool */

typedef void (*JobFn)(void *arg, unsigned wid);

static struct {
    pthread_t th[MAX_WORKERS];
    unsigned n;
    pthread_barrier_t start, done;
    JobFn fn;
    void *arg;
    int stop;
} pool;

static void *pool_worker(void *p)
{
    unsigned wid = (unsigned)(uintptr_t)p;
    for (;;) {
        pthread_barrier_wait(&pool.start);
        if (pool.stop)
            break;
        pool.fn(pool.arg, wid);
        pthread_barrier_wait(&pool.done);
    }
    return NULL;
}

static int pool_init(unsigned n)
{
    pool.n = n;
    if (pthread_barrier_init(&pool.start, NULL, n + 1) ||
        pthread_barrier_init(&pool.done, NULL, n + 1))
        return -1;
    for (unsigned i = 0; i < n; i++)
        if (pthread_create(&pool.th[i], NULL, pool_worker, (void *)(uintptr_t)i))
            return -1;
    return 0;
}

/* Run fn on every worker and wait: one phase barrier. */
static void pool_run(JobFn fn, void *arg)
{
    pool.fn = fn;
    pool.arg = arg;
    pthread_barrier_wait(&pool.start);
    pthread_barrier_wait(&pool.done);
}

static void pool_shutdown(void)
{
    pool.stop = 1;
    pthread_barrier_wait(&pool.start);
    for (unsigned i = 0; i < pool.n; i++)
        pthread_join(pool.th[i], NULL);
}

/* ---------------------------------------------------------------- phases */

typedef struct {
    const char *begin, *end;
    Cand *out;
    uint64_t count;
} ParseTask;

typedef struct {
    Cand *c;
    uint64_t n;
} DedupTask;

typedef struct {
    uint64_t slot_begin, slot_end;
    Win *out;
    uint64_t count;
    uint64_t occupied;
} CollectTask;

typedef struct {
    uint64_t pbegin, pend;
    Row *out;
    uint64_t count;
    uint64_t credits;   /* sessions credited to a non-organic campaign */
} SessTask;

typedef struct {
    const char *begin, *end;
    ClickCand *out;
    uint64_t count;
} ClickTask;

typedef struct {
    _Atomic size_t next;
    size_t ntasks;
    void *tasks;
    const void *ctx;
    void *ctx2;
} Queue;

/* Links are only needed from P3 on, so worker 0 runs the (integer-only)
   union-find + smallest-name pass while the other workers start parsing
   events; it joins the parse queue after.
   Still exactly the pool's threads: no extra thread, no oversubscription. */
typedef struct {
    Dict *dict;
    LinkBuild *lb;
    int rc;
    uint64_t ns;
} LinkJob;

static void job_links(void *arg, unsigned wid)
{
    Queue *q = arg;
    LinkBuild *lb = (LinkBuild *)q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        prof_begin(wid, PH_LINKS);
        links_parse_task(lb, i);
        prof_end(wid, PH_LINKS, links_task_pairs(lb, i), 1);
    }
}

static void job_parse(void *arg, unsigned wid)
{
    Queue *q = arg;
    ParseTask *t = q->tasks;
    LinkJob *lj = q->ctx2;
    if (wid == 0 && lj) {
        uint64_t a = now_ns();
        prof_begin(wid, PH_UNIONFIND);
        lj->rc = links_finish(lj->lb, lj->dict);
        prof_end(wid, PH_UNIONFIND, lj->dict->link_ids, 1);
        lj->ns = now_ns() - a;
    }
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        prof_begin(wid, PH_PARSE);
        t[i].count = evx_parse_events(q->ctx, t[i].begin, t[i].end, t[i].out);
        prof_end(wid, PH_PARSE, t[i].count, 1);
    }
}

static void job_dedup(void *arg, unsigned wid)
{
    Queue *q = arg;
    DedupTask *t = q->tasks;
    const uint64_t *size = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        prof_begin(wid, PH_DEDUP);
        evx_dedup_insert(q->ctx2, *size, t[i].c, t[i].n);
        prof_end(wid, PH_DEDUP, t[i].n, 1);
    }
}

static void job_collect(void *arg, unsigned wid)
{
    Queue *q = arg;
    CollectTask *t = q->tasks;
    _Atomic int *overflow = q->ctx2;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        if (atomic_load_explicit(overflow, memory_order_relaxed))
            break;                      /* the phase will be rerun anyway */
        prof_begin(wid, PH_COLLECT);
        t[i].count = evx_collect(q->ctx, t[i].slot_begin, t[i].slot_end, t[i].out,
                                 &t[i].occupied);
        prof_end(wid, PH_COLLECT, t[i].occupied, 1);
        if (t[i].count == UINT64_MAX)
            atomic_store(overflow, 1);
    }
}

typedef struct {
    const uint32_t *remap;
    uint64_t *cursor;
    Ev *ev;
} ScatterArgs;

static void job_scatter(void *arg, unsigned wid)
{
    Queue *q = arg;
    CollectTask *t = q->tasks;
    const ScatterArgs *a = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        prof_begin(wid, PH_GROUP);
        evx_scatter(t[i].out, t[i].count, a->remap, a->cursor, a->ev);
        prof_end(wid, PH_GROUP, t[i].count, 1);
    }
}

/* P6: sessionize a block, then attribute it while its events are hot */
static void job_session(void *arg, unsigned wid)
{
    Queue *q = arg;
    SessTask *t = q->tasks;
    SessCtx *ctx = &((SessCtx *)q->ctx2)[wid];
    AttrCtx *actx = &((AttrCtx *)q->ctx)[wid];
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        uint64_t nev = ctx->offsets[t[i].pend] - ctx->offsets[t[i].pbegin];
        prof_begin(wid, PH_SESSION);
        t[i].count = evx_sessionize(ctx, t[i].pbegin, t[i].pend, t[i].out);
        prof_end(wid, PH_SESSION, nev, 1);
        prof_begin(wid, PH_ATTR);
        t[i].credits = evx_attribute(actx, t[i].pbegin, t[i].pend);
        prof_end(wid, PH_ATTR, nev + (actx->coffsets[t[i].pend] - actx->coffsets[t[i].pbegin]), 1);
    }
}

static void job_parse_clicks(void *arg, unsigned wid)
{
    Queue *q = arg;
    ClickTask *t = q->tasks;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        prof_begin(wid, PH_CPARSE);
        t[i].count = evx_parse_clicks(q->ctx, t[i].begin, t[i].end, t[i].out);
        prof_end(wid, PH_CPARSE, t[i].count, 1);
    }
}

typedef struct {
    uint64_t *cursor;
    ClickEv *dst;
    const ClickCand *base;
} ClickScatterArgs;

static void job_scatter_clicks(void *arg, unsigned wid)
{
    Queue *q = arg;
    ClickTask *t = q->tasks;
    const ClickScatterArgs *a = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        prof_begin(wid, PH_CGROUP);
        evx_scatter_clicks(t[i].out, t[i].count, a->cursor, a->dst, a->base);
        prof_end(wid, PH_CGROUP, t[i].count, 1);
    }
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* Map every *.csv in dir; returns file count (0 if the dir is missing). */
static int map_dir(const char *dir, Mapped **out, size_t *total)
{
    char path[8192];
    char **names = NULL;
    int n = list_csv(dir, &names);
    if (n < 0)
        n = 0;
    Mapped *files = calloc((size_t)(n ? n : 1), sizeof *files);
    *total = 0;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        free(names[i]);
        if (map_file(path, &files[i]) != 0)
            return -1;
        *total += files[i].size;
    }
    free(names);
    *out = files;
    return n;
}

static unsigned worker_count(void)
{
    const char *env = getenv("NCPU");
    long n = env && *env ? strtol(env, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;
    if (n > CPU_BUDGET)   /* hard limit: the scorer gives 8 vCPUs */
        n = CPU_BUDGET;
    return (unsigned)n;
}

static double ms(uint64_t a, uint64_t b) { return (double)(b - a) / 1e6; }

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST)
        return 0;
    perror(path);
    return -1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    umask(0);
    uint64_t t0 = now_ns();

    const char *data_dir = getenv("DATA_DIR");
    if (!data_dir || !*data_dir)
        data_dir = "/data";
    const char *out_dir = getenv("OUTPUT_DIR");
    if (!out_dir || !*out_dir)
        out_dir = "/output";
    unsigned nw = worker_count();

    /* ---- P0: manifest, products, links ---------------------------------- */
    static Dict dict;
    if (load_manifest(data_dir, &dict) || load_products(data_dir, &dict) ||
        load_campaigns(data_dir, &dict))
        return 1;
    prof_init(nw);
    if (pool_init(nw) != 0) {
        fprintf(stderr, "thread pool init failed\n");
        return 1;
    }
    /* P0b: link files parsed and interned by all workers */
    LinkBuild *lbuild = NULL;
    if (links_plan(data_dir, nw, &lbuild) != 0)
        return 1;
    {
        Queue lq = {.ntasks = links_ntasks(lbuild), .ctx = lbuild};
        atomic_init(&lq.next, 0);
        pool_run(job_links, &lq);
    }
    uint64_t t_dict = now_ns();

    /* ---- event files -> newline-aligned byte-range tasks ----------------- */
    char evdir[4096], path[8192];
    snprintf(evdir, sizeof evdir, "%s/events", data_dir);
    char **names = NULL;
    int nfiles = list_csv(evdir, &names);
    if (nfiles < 0) {
        perror(evdir);
        return 1;
    }
    Mapped *files = calloc((size_t)(nfiles ? nfiles : 1), sizeof *files);
    size_t total_bytes = 0;
    for (int i = 0; i < nfiles; i++) {
        snprintf(path, sizeof path, "%s/%s", evdir, names[i]);
        free(names[i]);
        if (map_file(path, &files[i]) != 0)
            return 1;
        total_bytes += files[i].size;
    }
    free(names);

    size_t chunk = total_bytes / ((size_t)nw * 16) + 1;
    if (chunk < ((size_t)1 << 20))
        chunk = (size_t)1 << 20;
    size_t task_cap = (size_t)nfiles + total_bytes / chunk + 16;
    ParseTask *ptasks = calloc(task_cap, sizeof *ptasks);
    size_t nptasks = 0, cand_cap = 0;
    for (int i = 0; i < nfiles; i++) {
        const char *b = files[i].data, *e = files[i].data + files[i].size;
        if (!b)
            continue;
        const char *nl = memchr(b, '\n', files[i].size);
        if (!nl)
            continue;                     /* header only */
        const char *p = nl + 1;
        while (p < e) {
            const char *q = p + chunk < e ? p + chunk : e;
            if (q < e) {
                const char *n2 = memchr(q, '\n', (size_t)(e - q));
                q = n2 ? n2 + 1 : e;
            }
            ptasks[nptasks].begin = p;
            ptasks[nptasks].end = q;
            /* a row that yields a Cand is >= 8 bytes ("e,,,,,,\n") */
            cand_cap += (size_t)(q - p) / 8 + 1;
            nptasks++;
            p = q;
        }
    }
    size_t cand_bytes = cand_cap * sizeof(Cand);
    Cand *cands = big_alloc(cand_bytes);
    if (!cands) {
        fprintf(stderr, "out of memory (candidates)\n");
        return 1;
    }
    {
        size_t off = 0;
        for (size_t i = 0; i < nptasks; i++) {
            ptasks[i].out = cands + off;
            off += (size_t)(ptasks[i].end - ptasks[i].begin) / 8 + 1;
        }
    }

    /* ---- P1: parse --------------------------------------------------------- */
    ParseCtx pctx = {
        .window_start = dict.window_start,
        .window_end = dict.window_end,
        .link_slots = dict.link_slots,
        .link_mask = dict.link_mask,
        .prod_slots = dict.prod_slots,
        .prod_mask = dict.prod_mask,
        .unknown_cat = dict.unknown_cat,
    };
    LinkJob ljob = {.dict = &dict, .lb = lbuild};
    Queue q = {.ntasks = nptasks, .tasks = ptasks, .ctx = &pctx, .ctx2 = &ljob};
    atomic_init(&q.next, 0);
    uint64_t t_p1 = now_ns();
    pool_run(job_parse, &q);
    if (ljob.rc != 0)
        return 1;
    uint64_t t_parse = now_ns();
    uint64_t ncand = 0;
    for (size_t i = 0; i < nptasks; i++)
        ncand += ptasks[i].count;

    /* ---- P2: global dedup --------------------------------------------------
     * Exact-size table, 1.5 slots per candidate (load <= 0.67), indexed by a
     * multiply in asm, so it is not rounded up to a power of two. */
    uint64_t dsize = ncand + ncand / 2 + 1024;
    if (dsize >= ((uint64_t)1 << 32)) {
        fprintf(stderr, "too many event rows for the 32-bit dedup index\n");
        return 1;
    }
    uint64_t *dedup = big_alloc(dsize * sizeof(uint64_t));
    size_t dt_cap = nptasks + ncand / 65536 + 16;
    DedupTask *dtasks = calloc(dt_cap, sizeof *dtasks);
    size_t ndt = 0;
    for (size_t i = 0; i < nptasks; i++)
        for (uint64_t o = 0; o < ptasks[i].count; o += 65536) {
            uint64_t n = ptasks[i].count - o < 65536 ? ptasks[i].count - o : 65536;
            dtasks[ndt++] = (DedupTask){ptasks[i].out + o, n};
        }
    if (!dedup || !dtasks) {
        fprintf(stderr, "out of memory (dedup)\n");
        return 1;
    }
    q = (Queue){.ntasks = ndt, .tasks = dtasks, .ctx = &dsize, .ctx2 = dedup};
    atomic_init(&q.next, 0);
    pool_run(job_dedup, &q);
    uint64_t t_dedup = now_ns();

    /* ---- P3: collect valid winners, resolve + intern persons, count ---------
     * The person table starts at ~1/8 of the candidates (people are a few %
     * of events) and is enlarged and the phase rerun if more than half of it
     * gets claimed. At pow2 >= 2 * ncand it can no longer overflow. */
    uint64_t pmax = pow2_at_least(ncand * 2);
    uint64_t psize = pow2_at_least(ncand / 8);
    {
        const char *env = getenv("EVX_PSIZE");    /* test hook: force retries */
        if (env && *env)
            psize = pow2_at_least(strtoull(env, NULL, 10));
    }
    if (psize > pmax)
        psize = pmax;
    Win *wins = big_alloc(dsize * sizeof(Win));
    const uint64_t CSLOTS = 65536;
    size_t nct = (size_t)((dsize + CSLOTS - 1) / CSLOTS);
    CollectTask *ctasks = calloc(nct, sizeof *ctasks);
    if (!wins || !ctasks) {
        fprintf(stderr, "out of memory (collect)\n");
        return 1;
    }
    PersonSlot *persons = NULL;
    uint32_t *counts = NULL;
    unsigned person_retries = 0;
    for (;;) {
        persons = big_alloc(psize * sizeof *persons);
        counts = big_alloc(psize * sizeof *counts);
        if (!persons || !counts) {
            fprintf(stderr, "out of memory (persons)\n");
            return 1;
        }
        for (size_t i = 0; i < nct; i++) {
            ctasks[i].slot_begin = i * CSLOTS;
            ctasks[i].slot_end = (i + 1) * CSLOTS < dsize ? (i + 1) * CSLOTS : dsize;
            ctasks[i].out = wins + ctasks[i].slot_begin;
            ctasks[i].count = 0;
            ctasks[i].occupied = 0;
        }
        uint64_t claimed = 0;
        _Atomic int overflow = 0;
        CollectCtx cctx = {
            .dedup = dedup, .persons = persons, .person_mask = psize - 1, .counts = counts,
            .link_slots = dict.link_slots, .link_mask = dict.link_mask,
            .claimed = &claimed, .claim_limit = psize / 2,
        };
        q = (Queue){.ntasks = nct, .tasks = ctasks, .ctx = &cctx, .ctx2 = (void *)&overflow};
        atomic_init(&q.next, 0);
        pool_run(job_collect, &q);
        if (!atomic_load(&overflow))
            break;
        big_free(persons, psize * sizeof *persons);
        big_free(counts, psize * sizeof *counts);
        psize = psize * 8 < pmax ? psize * 8 : pmax;
        person_retries++;
    }
    uint64_t t_collect = now_ns();

    /* ---- P4: dense persons + prefix sums --------------------------------
     * Person slots are sparse (table sized for the worst case). Compact the
     * occupied ones into dense ids: counts[] is reused as the slot -> dense
     * remap, so offsets/cursor/names are sized by people, not by slots. */
    uint64_t nvalid = 0, maxn = 0, npersons = 0, nunique = 0;
    for (size_t i = 0; i < nct; i++)
        nunique += ctasks[i].occupied;
    for (uint64_t p = 0; p < psize; p++)
        npersons += counts[p] != 0;
    uint64_t *offsets = big_alloc((npersons + 1) * sizeof(uint64_t));
    uint64_t *cursor = big_alloc((npersons ? npersons : 1) * sizeof(uint64_t));
    Slice *pnames = malloc((npersons ? npersons : 1) * sizeof *pnames);
    Key16 *pkeys = malloc((npersons ? npersons : 1) * sizeof *pkeys);
    if (!offsets || !cursor || !pnames || !pkeys) {
        fprintf(stderr, "out of memory (offsets)\n");
        return 1;
    }
    {
        uint64_t k = 0;
        for (uint64_t p = 0; p < psize; p++) {
            uint64_t c = counts[p];
            if (!c)
                continue;
            offsets[k] = nvalid;
            cursor[k] = nvalid;
            /* output name: an inline key holds its own bytes (length in byte
               15); a long key points into the input mapping */
            pkeys[k] = persons[p];
            if (KEY_LONG(pkeys[k].k1))
                pnames[k] = (Slice){(const char *)pkeys[k].k0,
                                    (uint32_t)((pkeys[k].k1 >> 32) & 0xFFFFFF), 0};
            else
                pnames[k] = (Slice){(const char *)&pkeys[k], (uint32_t)(pkeys[k].k1 >> 56), 0};
            nvalid += c;
            if (c > maxn)
                maxn = c;
            counts[p] = (uint32_t)k++;
        }
        offsets[npersons] = nvalid;
    }
    /* names now live in pnames (pointing into the input mappings): the
       candidate store, dedup table and person table are no longer needed */
    big_free(dedup, dsize * sizeof(uint64_t));
    big_free(cands, cand_bytes);
    /* the person table and counts[] (now slot -> dense remap) stay alive
       until the click parse has resolved every click's person */
    uint64_t t_prefix = now_ns();

    /* ---- P5: group events by person ---------------------------------------- */
    Ev *ev = big_alloc((nvalid ? nvalid : 1) * sizeof(Ev));
    if (!ev) {
        fprintf(stderr, "out of memory (events)\n");
        return 1;
    }
    ScatterArgs sa = {.remap = counts, .cursor = cursor, .ev = ev};
    q = (Queue){.ntasks = nct, .tasks = ctasks, .ctx = &sa};
    atomic_init(&q.next, 0);
    pool_run(job_scatter, &q);
    big_free(wins, dsize * sizeof(Win));
    big_free(cursor, (npersons ? npersons : 1) * sizeof(uint64_t));
    uint64_t t_scatter = now_ns();

    /* ---- C1: parse clicks ----------------------------------------------------
     * Every failing row is dropped at parse (copies of a click are identical,
     * so no click dedup is needed); clicks of people without surviving events
     * are dropped too (they have no sessions to credit or assist). */
    const int64_t W7 = 604800000;
    char ckdir[4096];
    snprintf(ckdir, sizeof ckdir, "%s/clicks", data_dir);
    Mapped *cfiles = NULL;
    size_t click_bytes = 0;
    int ncfiles = map_dir(ckdir, &cfiles, &click_bytes);
    if (ncfiles < 0)
        return 1;
    size_t cchunk = click_bytes / ((size_t)nw * 16) + 1;
    if (cchunk < ((size_t)1 << 20))
        cchunk = (size_t)1 << 20;
    ClickTask *ktasks = calloc((size_t)ncfiles + click_bytes / cchunk + 16, sizeof *ktasks);
    size_t nkt = 0, kcap = 0;
    for (int i = 0; i < ncfiles; i++) {
        if (!cfiles[i].data)
            continue;
        const char *b = cfiles[i].data, *e = b + cfiles[i].size;
        const char *nl = memchr(b, '\n', cfiles[i].size);
        if (!nl)
            continue;
        for (const char *p = nl + 1; p < e;) {
            const char *q2 = p + cchunk < e ? p + cchunk : e;
            if (q2 < e) {
                const char *n2 = memchr(q2, '\n', (size_t)(e - q2));
                q2 = n2 ? n2 + 1 : e;
            }
            ktasks[nkt].begin = p;
            ktasks[nkt].end = q2;
            /* a click row that survives is >= 7 bytes ("c,u,k,t") */
            kcap += (size_t)(q2 - p) / 7 + 1;
            nkt++;
            p = q2;
        }
    }
    if (kcap >= ((uint64_t)1 << 32)) {
        fprintf(stderr, "too many click rows for the 32-bit click index\n");
        return 1;
    }
    ClickCand *kc = big_alloc((kcap ? kcap : 1) * sizeof(ClickCand));
    uint32_t *ccount = big_alloc((npersons ? npersons : 1) * sizeof(uint32_t));
    if (!kc || !ccount || !ktasks) {
        fprintf(stderr, "out of memory (clicks)\n");
        return 1;
    }
    {
        size_t off = 0;
        for (size_t i = 0; i < nkt; i++) {
            ktasks[i].out = kc + off;
            off += (size_t)(ktasks[i].end - ktasks[i].begin) / 7 + 1;
        }
    }
    ClickParseCtx kctx = {
        .cw_start = dict.window_start - W7,
        .cw_end = dict.window_end,
        .link_slots = dict.link_slots,
        .link_mask = dict.link_mask,
        .camp_slots = dict.camp_slots,
        .camp_mask = dict.camp_mask,
        .persons = persons,
        .person_mask = psize - 1,
        .remap = counts,
        .ccount = ccount,
    };
    q = (Queue){.ntasks = nkt, .tasks = ktasks, .ctx = &kctx};
    atomic_init(&q.next, 0);
    pool_run(job_parse_clicks, &q);
    big_free(persons, psize * sizeof *persons);
    big_free(counts, psize * sizeof *counts);
    uint64_t nclicks = 0;
    for (size_t i = 0; i < nkt; i++)
        nclicks += ktasks[i].count;
    uint64_t t_cparse = now_ns();

    /* ---- C2: click prefix sums ------------------------------------------------ */
    uint64_t *coffsets = big_alloc((npersons + 1) * sizeof(uint64_t));
    uint64_t *ccursor = big_alloc((npersons ? npersons : 1) * sizeof(uint64_t));
    if (!coffsets || !ccursor) {
        fprintf(stderr, "out of memory (click offsets)\n");
        return 1;
    }
    uint64_t maxc = 0, cpersons = 0;
    {
        uint64_t run = 0;
        for (uint64_t k = 0; k < npersons; k++) {
            coffsets[k] = run;
            ccursor[k] = run;
            run += ccount[k];
            if (ccount[k]) {
                cpersons++;
                if (ccount[k] > maxc)
                    maxc = ccount[k];
            }
        }
        coffsets[npersons] = run;
    }
    big_free(ccount, (npersons ? npersons : 1) * sizeof(uint32_t));

    /* ---- C3: group clicks by person ------------------------------------------- */
    ClickEv *kev = big_alloc((nclicks ? nclicks : 1) * sizeof(ClickEv));
    if (!kev) {
        fprintf(stderr, "out of memory (click events)\n");
        return 1;
    }
    ClickScatterArgs ksa = {.cursor = ccursor, .dst = kev, .base = kc};
    q = (Queue){.ntasks = nkt, .tasks = ktasks, .ctx = &ksa};
    atomic_init(&q.next, 0);
    pool_run(job_scatter_clicks, &q);
    big_free(ccursor, (npersons ? npersons : 1) * sizeof(uint64_t));
    uint64_t t_cgroup = now_ns();

    /* ---- P6: sessionize ------------------------------------------------------ */
    uint64_t span = (uint64_t)(dict.window_end - dict.window_start - 1);
    uint32_t passes = 1;
    while (passes < 8 && (span >> (8 * passes)) != 0)
        passes++;
    int64_t day0 = dict.window_start / 86400000;
    if (dict.window_start % 86400000 < 0)
        day0--;
    SessCtx *sctx = big_alloc(nw * sizeof(SessCtx));
    if (!sctx) {
        fprintf(stderr, "out of memory (session ctx)\n");
        return 1;
    }
    for (unsigned w = 0; w < nw; w++) {
        sctx[w].offsets = offsets;
        sctx[w].events = ev;
        sctx[w].scratch = big_alloc((maxn ? maxn : 1) * sizeof(Ev));
        sctx[w].day0 = day0;
        sctx[w].base_ms = (uint64_t)(dict.window_start - day0 * 86400000);
        sctx[w].radix_passes = passes;
        if (!sctx[w].scratch) {
            fprintf(stderr, "out of memory (scratch)\n");
            return 1;
        }
    }
    /* attribution state per worker */
    const int64_t DAYMS = 86400000;
    uint32_t ndays = (uint32_t)(floor_div(dict.window_end - 1, DAYMS) - day0 + 1);
    uint64_t cspan = (uint64_t)(dict.window_end - (dict.window_start - W7) - 1);
    uint32_t cpasses = 1;
    while (cpasses < 8 && (cspan >> (8 * cpasses)) != 0)
        cpasses++;
    if (nvalid >= ((uint64_t)1 << 32)) {
        fprintf(stderr, "too many events for 32-bit aggregate counters\n");
        return 1;
    }
    size_t ncells = ((size_t)dict.ncamp + 1) * ndays;
    AttrCtx *actx = big_alloc(nw * sizeof(AttrCtx));
    if (!actx) {
        fprintf(stderr, "out of memory (attribution ctx)\n");
        return 1;
    }
    for (unsigned w = 0; w < nw; w++) {
        AttrCtx *a = &actx[w];
        a->offsets = offsets;
        a->events = ev;
        a->coffsets = coffsets;
        a->clicks = kev;
        a->ccands = kc;
        a->cscratch = big_alloc((maxc ? maxc : 1) * sizeof(ClickEv));
        a->sess = big_alloc((maxn ? maxn : 1) * sizeof(SessRec));
        a->cnt = big_alloc(((size_t)dict.ncamp + 1) * sizeof(uint32_t));
        a->enter = big_alloc(((size_t)dict.ncamp + 1) * sizeof(uint32_t));
        a->day_first = big_alloc(((size_t)ndays + 2) * sizeof(uint32_t));
        a->agg = big_alloc(ncells * sizeof(AggCell));
        a->base_ms = (uint64_t)(dict.window_start - day0 * DAYMS);
        a->ncamp = dict.ncamp;
        a->ndays = ndays;
        a->cpasses = cpasses;
        if (!a->cscratch || !a->sess || !a->cnt || !a->enter || !a->day_first || !a->agg) {
            fprintf(stderr, "out of memory (attribution)\n");
            return 1;
        }
    }
    Row *rows = big_alloc((nvalid ? nvalid : 1) * sizeof(Row));
    /* blocks of ~16K events + clicks (whales get a block of their own) */
    size_t st_cap = npersons + 1;               /* every block has >= 1 person */
    SessTask *stasks = calloc(st_cap, sizeof *stasks);
    if (!rows || !stasks) {
        fprintf(stderr, "out of memory (rows)\n");
        return 1;
    }
    size_t nst = 0;
    for (uint64_t k = 0; k < npersons;) {
        uint64_t e = k + 1;
        while (e < npersons && e - k < 4096 &&
               (offsets[e + 1] - offsets[k]) + (coffsets[e + 1] - coffsets[k]) <= 16384)
            e++;
        stasks[nst].pbegin = k;
        stasks[nst].pend = e;
        stasks[nst].out = rows + offsets[k];   /* rows <= events */
        nst++;
        k = e;
    }
    q = (Queue){.ntasks = nst, .tasks = stasks, .ctx = actx, .ctx2 = sctx};
    atomic_init(&q.next, 0);
    pool_run(job_session, &q);
    uint64_t t_session = now_ns();
    pool_shutdown();

    /* ---- P7: Parquet ------------------------------------------------------------ */
    uint64_t nrows = 0;
    for (size_t i = 0; i < nst; i++)
        nrows += stasks[i].count;
    Slice *uid = malloc((nrows ? nrows : 1) * sizeof *uid);
    Slice *top = malloc((nrows ? nrows : 1) * sizeof *top);
    int32_t *day = malloc((nrows ? nrows : 1) * sizeof *day);
    int64_t *sess = malloc((nrows ? nrows : 1) * sizeof *sess);
    int64_t *evs = malloc((nrows ? nrows : 1) * sizeof *evs);
    int64_t *rev = malloc((nrows ? nrows : 1) * sizeof *rev);
    if (!uid || !top || !day || !sess || !evs || !rev) {
        fprintf(stderr, "out of memory (output columns)\n");
        return 1;
    }
    uint64_t r = 0, sum_sessions = 0;
    for (size_t i = 0; i < nst; i++)
        for (uint64_t k = 0; k < stasks[i].count; k++, r++) {
            const Row *row = &stasks[i].out[k];
            uid[r] = pnames[row->pid];
            day[r] = row->day;
            sess[r] = row->sessions;
            evs[r] = (int64_t)row->events;
            rev[r] = (int64_t)row->revenue;
            sum_sessions += row->sessions;
            top[r] = row->top_cat == TOPCAT_NULL ? (Slice){NULL, 0, 0} : dict.cats[row->top_cat];
        }
    char odir[4096], opath[8192];
    snprintf(odir, sizeof odir, "%s/user_daily", out_dir);
    if (ensure_dir(out_dir) || ensure_dir(odir))
        return 1;
    snprintf(opath, sizeof opath, "%s/part-00000.parquet", odir);
    ColSpec ucols[6] = {
        {"user_id", COL_STR, uid, NULL, NULL},
        {"day", COL_DATE, NULL, day, NULL},
        {"sessions", COL_I64, NULL, NULL, sess},
        {"events", COL_I64, NULL, NULL, evs},
        {"revenue_cents", COL_I64, NULL, NULL, rev},
        {"top_category", COL_STR_OPT, top, NULL, NULL},
    };
    if (write_parquet(opath, nrows, 6, ucols) != 0)
        return 1;

    /* ---- P7b: reduce per-worker campaign/day tables -> campaign_daily ------- */
    uint64_t sum_user_rev = 0;
    for (uint64_t i = 0; i < nrows; i++)
        sum_user_rev += (uint64_t)rev[i];
    uint64_t *tot_rev = calloc(ncells ? ncells : 1, sizeof *tot_rev);
    uint64_t *tot_s = calloc(ncells ? ncells : 1, sizeof *tot_s);
    uint64_t *tot_p = calloc(ncells ? ncells : 1, sizeof *tot_p);
    uint64_t *tot_a = calloc(ncells ? ncells : 1, sizeof *tot_a);
    if (!tot_rev || !tot_s || !tot_p || !tot_a) {
        fprintf(stderr, "out of memory (campaign reduce)\n");
        return 1;
    }
    for (unsigned w = 0; w < nw; w++)
        for (size_t c = 0; c < ncells; c++) {
            const AggCell *x = &actx[w].agg[c];
            tot_s[c] += x->sessions;
            tot_p[c] += x->purchasing;
            tot_a[c] += x->assisted;
            tot_rev[c] += x->revenue;
        }
    size_t ncrows = 0;
    for (size_t c = 0; c < ncells; c++)
        ncrows += (tot_s[c] || tot_a[c]);
    Slice *cid = malloc((ncrows ? ncrows : 1) * sizeof *cid);
    Slice *chn = malloc((ncrows ? ncrows : 1) * sizeof *chn);
    int32_t *cday = malloc((ncrows ? ncrows : 1) * sizeof *cday);
    int64_t *cs = malloc((ncrows ? ncrows : 1) * sizeof *cs);
    int64_t *cp = malloc((ncrows ? ncrows : 1) * sizeof *cp);
    int64_t *crev = malloc((ncrows ? ncrows : 1) * sizeof *crev);
    int64_t *cas = malloc((ncrows ? ncrows : 1) * sizeof *cas);
    if (!cid || !chn || !cday || !cs || !cp || !crev || !cas) {
        fprintf(stderr, "out of memory (campaign rows)\n");
        return 1;
    }
    uint64_t csum_s = 0, csum_rev = 0, organic_s = 0, assist_total = 0, bad_rows = 0;
    size_t cr = 0;
    for (uint32_t c = 0; c <= dict.ncamp; c++)
        for (uint32_t d = 0; d < ndays; d++) {
            size_t x = (size_t)c * ndays + d;
            if (!tot_s[x] && !tot_a[x])
                continue;
            cid[cr] = dict.camp_ids[c];
            chn[cr] = dict.camp_channels[c];
            cday[cr] = (int32_t)(day0 + d);
            cs[cr] = (int64_t)tot_s[x];
            cp[cr] = (int64_t)tot_p[x];
            crev[cr] = (int64_t)tot_rev[x];
            cas[cr] = (int64_t)tot_a[x];
            csum_s += tot_s[x];
            csum_rev += tot_rev[x];
            assist_total += tot_a[x];
            if (c == dict.ncamp) {
                organic_s += tot_s[x];
                bad_rows += tot_a[x] != 0;
            } else {
                bad_rows += tot_a[x] < tot_s[x];   /* credited => assisted */
            }
            cr++;
        }
    snprintf(odir, sizeof odir, "%s/campaign_daily", out_dir);
    if (ensure_dir(odir))
        return 1;
    snprintf(opath, sizeof opath, "%s/part-00000.parquet", odir);
    ColSpec ccols[7] = {
        {"campaign_id", COL_STR, cid, NULL, NULL},
        {"channel", COL_STR, chn, NULL, NULL},
        {"day", COL_DATE, NULL, cday, NULL},
        {"sessions", COL_I64, NULL, NULL, cs},
        {"purchasing_sessions", COL_I64, NULL, NULL, cp},
        {"revenue_cents", COL_I64, NULL, NULL, crev},
        {"assisted_sessions", COL_I64, NULL, NULL, cas},
    };
    if (write_parquet(opath, ncrows, 7, ccols) != 0)
        return 1;
    uint64_t credits = 0;
    for (size_t i = 0; i < nst; i++)
        credits += stasks[i].credits;
    int invariants_ok = csum_s == sum_sessions && csum_rev == sum_user_rev && bad_rows == 0 &&
                        credits + organic_s == sum_sessions;
    uint64_t t_end = now_ns();

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    fprintf(stderr,
            "evx: workers=%u files=%d bytes=%zu tasks=%zu\n"
            "evx: products=%zu categories=%u link_ids=%zu link_lines=%zu dropped=%zu components=%zu\n"
            "evx: candidates=%llu unique_ids=%llu duplicates=%llu valid_events=%llu persons=%llu max_person=%llu\n"
            "evx: sessions=%llu rows=%llu peak_rss=%ld MiB\n"
            "evx: ms dict=%.1f (link union-find %.1f, overlapped with parse) map+plan=%.1f parse=%.1f dedup=%.1f collect=%.1f prefix=%.1f "
            "group=%.1f session=%.1f parquet=%.1f total=%.1f\n",
            nw, nfiles, total_bytes, nptasks, dict.product_count, dict.ncats, dict.link_ids,
            dict.link_lines, dict.link_dropped, dict.link_components,
            (unsigned long long)ncand, (unsigned long long)nunique,
            (unsigned long long)(ncand - nunique), (unsigned long long)nvalid,
            (unsigned long long)npersons, (unsigned long long)maxn,
            (unsigned long long)sum_sessions, (unsigned long long)nrows, ru.ru_maxrss / 1024,
            ms(t0, t_dict), (double)ljob.ns / 1e6, ms(t_dict, t_p1), ms(t_p1, t_parse), ms(t_parse, t_dedup),
            ms(t_dedup, t_collect), ms(t_collect, t_prefix), ms(t_prefix, t_scatter),
            ms(t_cgroup, t_session), ms(t_session, t_end), ms(t0, t_end));
    double cpu_s = (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                   (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    double wall_s = (double)(t_end - t0) / 1e9;
    fprintf(stderr,
            "evx: clicks files=%d bytes=%zu tasks=%zu valid=%llu persons_with_clicks=%llu max_person_clicks=%llu\n"
            "evx: campaigns=%u days=%u credited=%llu organic=%llu assists=%llu campaign_rows=%zu\n"
            "evx: ms click_parse=%.1f click_group=%.1f (session+attribution in session=)\n"
            "evx: invariants %s (sessions %llu/%llu revenue %llu/%llu bad_rows %llu)\n",
            ncfiles, click_bytes, nkt, (unsigned long long)nclicks, (unsigned long long)cpersons,
            (unsigned long long)maxc, dict.ncamp, ndays, (unsigned long long)credits,
            (unsigned long long)organic_s, (unsigned long long)assist_total, ncrows,
            ms(t_scatter, t_cparse), ms(t_cparse, t_cgroup),
            invariants_ok ? "OK" : "VIOLATED", (unsigned long long)csum_s,
            (unsigned long long)sum_sessions, (unsigned long long)csum_rev,
            (unsigned long long)sum_user_rev, (unsigned long long)bad_rows);
    fprintf(stderr, "evx: tables dedup_slots=%llu person_slots=%llu person_retries=%u\n",
            (unsigned long long)dsize, (unsigned long long)psize, person_retries);
    {
        double wall[PH_COUNT] = {0};
        wall[PH_LINKS] = ms(t0, t_dict);
        wall[PH_PARSE] = ms(t_p1, t_parse);
        wall[PH_UNIONFIND] = (double)ljob.ns / 1e6;
        wall[PH_DEDUP] = ms(t_parse, t_dedup);
        wall[PH_COLLECT] = ms(t_dedup, t_collect);
        wall[PH_GROUP] = ms(t_prefix, t_scatter);
        wall[PH_CPARSE] = ms(t_scatter, t_cparse);
        wall[PH_CGROUP] = ms(t_cparse, t_cgroup);
        wall[PH_SESSION] = ms(t_cgroup, t_session);
        wall[PH_ATTR] = 0;
        prof_report(wall);
    }
    fprintf(stderr, "evx: page faults minor=%ld major=%ld\n", ru.ru_minflt, ru.ru_majflt);
    fprintf(stderr, "evx: cpu user=%.2fs sys=%.2fs avg_cores=%.2f (worker threads=%u + 1 idle coordinator)\n",
            (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
            (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, cpu_s / wall_s, nw);
    return 0;
}
