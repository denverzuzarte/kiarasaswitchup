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
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"

#define MAX_WORKERS 64
#define CPU_BUDGET 8      /* never run more than 8 worker threads */

/* ------------------------------------------------------------- utilities */

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
        /* enumerated advice values, not flags: one call each */
        madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL);
        madvise(p, (size_t)st.st_size, MADV_WILLNEED);
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
    madvise(p, bytes, MADV_NOHUGEPAGE);
    return p;
}

/* random-access tables: huge pages cut TLB misses */
void *big_alloc_hp(size_t bytes)
{
    void *p = big_alloc(bytes);
    if (p)
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

/* Start fn on every worker; the coordinator may work until pool_wait. */
static void pool_start(JobFn fn, void *arg)
{
    pool.fn = fn;
    pool.arg = arg;
    pthread_barrier_wait(&pool.start);
}

static void pool_wait(void) { pthread_barrier_wait(&pool.done); }

/* Run fn on every worker and wait: one phase barrier. */
static void pool_run(JobFn fn, void *arg)
{
    pool_start(fn, arg);
    pool_wait();
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
    uint64_t count;
} ParseTask;

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
} LinkJob;

static void job_links(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    LinkBuild *lb = (LinkBuild *)q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        links_parse_task(lb, i);
    }
}

/* Chunk arena slabs (called from the asm when a worker needs a 4 KB chunk):
   a worker cuts chunks from its own 2 MB slab, so the shared counter is
   touched once per 512 chunks, and the slab is pre-faulted in one call. */
char *evx_chunk_alloc(Slab *s)
{
    if (s->cur >= s->end) {
        uint64_t i = __atomic_fetch_add(s->next, 1, __ATOMIC_RELAXED);
        if (i >= s->nslabs) {
            fprintf(stderr, "chunk arena exhausted\n");
            _exit(1);
        }
        s->cur = s->arena + i * SLAB_BYTES;
        s->end = s->cur + SLAB_BYTES;
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif
        if (s->populate && madvise(s->cur, SLAB_BYTES, MADV_POPULATE_WRITE) != 0)
            s->populate = 0;            /* EINVAL on old kernels: fault lazily */
    }
    char *c = s->cur;
    s->cur += CHUNK_BYTES;
    return c;
}

/* Partitioned Rec store shared by parse and dedup. */
typedef struct {
    const ParseCtx *pctx;
    char *arena;
    PartHead *heads;        /* [nw][P] */
    Slab *slabs;            /* [nw] */
    uint32_t P, pbits;
    unsigned nw;
    _Atomic int overflow;   /* raw-ID table too small: rerun P1 */
} PartStore;

static void job_parse(void *arg, unsigned wid)
{
    Queue *q = arg;
    ParseTask *t = q->tasks;
    PartStore *ps = (PartStore *)q->ctx;
    LinkJob *lj = q->ctx2;
    if (wid == 0 && lj) {
        lj->rc = links_finish(lj->lb, lj->dict);
    }
    PartHead *heads = ps->heads + (size_t)wid * ps->P;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        if (atomic_load_explicit(&ps->overflow, memory_order_relaxed))
            break;                      /* the phase will be rerun anyway */
        uint64_t c = evx_parse_events(ps->pctx, t[i].begin, t[i].end, heads, &ps->slabs[wid]);
        if (c == UINT64_MAX) {
            atomic_store(&ps->overflow, 1);
            break;
        }
        t[i].count = c;
    }
    /* close this worker's open chunks: their headers get the row count */
    for (uint32_t p = 0; p < ps->P; p++)
        if (heads[p].end)
            *(uint32_t *)(heads[p].end - CHUNK_BYTES) =
                (uint32_t)(((char *)heads[p].cur - (heads[p].end - CHUNK_BYTES) - 32) / sizeof(Rec));
}

/* P1.5: raw slots -> person slots, one slot range per task */
typedef struct {
    const ResolveCtx *ctx;
    uint64_t rsize, step;
    _Atomic int overflow;
    /* remap pass */
    const Key16 *raw;
    uint32_t *pslot;            /* in: person slot; out: dense person id */
    const uint32_t *dense;      /* person slot -> dense id */
} ResolveArgs;

static void job_resolve(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    ResolveArgs *a = (ResolveArgs *)q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        if (atomic_load_explicit(&a->overflow, memory_order_relaxed))
            break;
        uint64_t s0 = i * a->step, s1 = s0 + a->step < a->rsize ? s0 + a->step : a->rsize;
        if (evx_resolve_raw(a->ctx, s0, s1) == UINT64_MAX) {
            atomic_store(&a->overflow, 1);
            break;
        }
    }
}

static void job_rremap(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    ResolveArgs *a = (ResolveArgs *)q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        uint64_t s0 = i * a->step, s1 = s0 + a->step < a->rsize ? s0 + a->step : a->rsize;
        for (uint64_t s = s0; s < s1; s++)
            if (a->raw[s].k1)
                a->pslot[s] = a->dense[a->pslot[s]];
    }
}

/* P2: one dedup partition per task: all workers' chunk lists into one
   private table, winners emitted as Wins into person-range chunk lists */
typedef struct {
    const PartStore *ps;
    DedupCtx *dctx;             /* [nw] */
} DedupArgs;

static void job_dedup(void *arg, unsigned wid)
{
    Queue *q = arg;
    const DedupArgs *a = q->ctx;
    const PartStore *ps = a->ps;
    DedupCtx *d = &a->dctx[wid];
    size_t p;
    while ((p = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        uint64_t n = 0;
        for (unsigned w = 0; w < ps->nw; w++)
            for (uint32_t c = ps->heads[(size_t)w * ps->P + p].head; c != CHUNK_NONE;
                 c = *(uint32_t *)(ps->arena + (size_t)c * CHUNK_BYTES + 4))
                n += *(uint32_t *)(ps->arena + (size_t)c * CHUNK_BYTES);
        d->tsize = n + n / 2 + 16;
        memset(d->table, 0, (d->tsize + 64) * sizeof *d->table);
        evx_dedup_emit(d, (uint32_t)p);
    }
}
typedef struct {
    SessCtx *sctx;              /* [nw] */
    AttrCtx *actx;              /* [nw] */
    const char *warena;
    const PartHead *wheads;     /* [nw][R] */
    unsigned nw;
    uint32_t R;
    uint64_t *cursor;           /* per dense person: count, then next Ev position */
    uint64_t *offsets;          /* [persons + 1]; range starts preset by the caller */
    const uint64_t *ebase;      /* [R + 1] first event of each range */
    Ev **scratch;               /* [nw] >= the largest range */
} SessArgs;

/* P5+P6 per person range: place the range's Wins into this worker's scratch
   (its Ev slice, L2-resident; no global Ev array), then sessionize and
   attribute the range while it is hot */
static void job_session(void *arg, unsigned wid)
{
    Queue *q = arg;
    SessTask *t = q->tasks;
    const SessArgs *a = q->ctx;
    SessCtx *ctx = &a->sctx[wid];
    AttrCtx *actx = &a->actx[wid];
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        if (t[i].pbegin == t[i].pend)
            continue;
        uint64_t base = a->ebase[i];
        /* events + offsets[k] must land in the scratch: shift the base */
        Ev *dst = (Ev *)((uintptr_t)a->scratch[wid] - base * sizeof(Ev));
        for (unsigned w = 0; w < a->nw; w++)
            evx_count_range(a->warena, a->wheads[(size_t)w * a->R + i].head, a->cursor);
        /* this range's offsets (offsets[pbegin] was preset) */
        for (uint64_t k = t[i].pbegin, run = base; k < t[i].pend; k++) {
            uint64_t c = a->cursor[k];
            if (k != t[i].pbegin)
                a->offsets[k] = run;
            a->cursor[k] = run;
            run += c;
        }
        for (unsigned w = 0; w < a->nw; w++)
            evx_group_range(a->warena, a->wheads[(size_t)w * a->R + i].head,
                            a->cursor, dst);
        ctx->events = dst;
        actx->events = dst;
        t[i].count = evx_sessionize(ctx, t[i].pbegin, t[i].pend, t[i].out);
        t[i].credits = evx_attribute(actx, t[i].pbegin, t[i].pend);
    }
}

/* P4: dense person ids from person-table occupancy, one range per task
   (every claimed slot has >= 1 valid event). Pass 1 counts, pass 2 fills. */
typedef struct {
    const PersonSlot *persons;
    uint32_t *remap;            /* slot -> dense id */
    uint32_t rsh;
    uint64_t *rfirst;           /* [R + 1] pass 1: persons per range; pass 2: prefix */
    Key16 *pkeys;
    Slice *pnames;
    int fill;
} CompactArgs;

static void job_compact(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    const CompactArgs *a = q->ctx;
    size_t r;
    while ((r = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        uint64_t p0 = (uint64_t)r << a->rsh, p1 = (uint64_t)(r + 1) << a->rsh;
        if (!a->fill) {
            uint64_t n = 0;
            for (uint64_t p = p0; p < p1; p++)
                n += a->persons[p].k1 != 0;
            a->rfirst[r] = n;
            continue;
        }
        uint64_t k = a->rfirst[r];
        for (uint64_t p = p0; p < p1; p++) {
            if (!a->persons[p].k1)
                continue;
            /* output name: an inline key holds its own bytes (length in byte
               15); a long key points into the input mapping */
            a->pkeys[k] = a->persons[p];
            if (KEY_LONG(a->pkeys[k].k1))
                a->pnames[k] = (Slice){(const char *)a->pkeys[k].k0,
                                       (uint32_t)((a->pkeys[k].k1 >> 32) & 0xFFFFFF), 0};
            else
                a->pnames[k] = (Slice){(const char *)&a->pkeys[k], (uint32_t)(a->pkeys[k].k1 >> 56), 0};
            a->remap[p] = (uint32_t)k++;
        }
    }
}

/* P7: every worker writes one user_daily part file from a contiguous block
   of person ranges (the rules accept any number of files). */
typedef struct {
    const SessTask *st;
    const size_t *tb;           /* [nw + 1] task block bounds */
    const Slice *pnames;
    const Slice *cats;
    const char *odir;
    int rc;
} WriteArgs;

static void job_write(void *arg, unsigned wid)
{
    WriteArgs *a = ((Queue *)arg)->tasks;
    size_t t0 = a->tb[wid], t1 = a->tb[wid + 1];
    uint64_t n = 0;
    for (size_t i = t0; i < t1; i++)
        n += a->st[i].count;
    if (!n && wid)
        return;                 /* worker 0 always writes (maybe empty) */
    /* one pass over the Rows into compact column arrays (fresh pages arrive
       cache-hot from the kernel's zeroing); strings as indexes into the
       name / category tables */
    size_t nn = n ? n : 1;
    char *mem = malloc(nn * 28);
    if (!mem) {
        a->rc = 1;
        return;
    }
    uint32_t *pid = (uint32_t *)mem, *top = pid + nn;
    int32_t *day = (int32_t *)(top + nn), *sess = day + nn, *evs = sess + nn;
    int64_t *rev = (int64_t *)(evs + nn);
    uint64_t r = 0;
    for (size_t i = t0; i < t1; i++)
        for (uint64_t k = 0; k < a->st[i].count; k++, r++) {
            const Row *row = &a->st[i].out[k];
            pid[r] = row->pid;
            top[r] = row->top_cat;              /* TOPCAT_NULL = null */
            day[r] = row->day;
            sess[r] = (int32_t)row->sessions;
            evs[r] = (int32_t)row->events;      /* < 2^32 events (checked) */
            rev[r] = (int64_t)row->revenue;
        }
    char opath[8192];
    snprintf(opath, sizeof opath, "%s/part-%05u.parquet", a->odir, wid);
    ColSpec ucols[6] = {
        {"user_id", COL_STR, a->pnames, NULL, NULL, 4, pid},
        {"day", COL_DATE, NULL, day, NULL, 0, NULL},
        {"sessions", COL_I32, NULL, sess, NULL, 0, NULL},
        {"events", COL_I32, NULL, evs, NULL, 0, NULL},
        {"revenue_cents", COL_I64, NULL, NULL, rev, 0, NULL},
        {"top_category", COL_STR_OPT, a->cats, NULL, NULL, 4, top},
    };
    if (write_parquet(opath, n, 6, ucols, NULL, 0) != 0)
        a->rc = 1;
    free(mem);
}

static void job_parse_clicks(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    ClickTask *t = q->tasks;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        t[i].count = evx_parse_clicks(q->ctx, t[i].begin, t[i].end, t[i].out);
    }
}

typedef struct {
    uint64_t *cursor;
    ClickEv *dst;
    const ClickCand *base;
} ClickScatterArgs;

static void job_scatter_clicks(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    ClickTask *t = q->tasks;
    const ClickScatterArgs *a = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        evx_scatter_clicks(t[i].out, t[i].count, a->cursor, a->dst, a->base);
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

    const char *data_dir = getenv("DATA_DIR");
    if (!data_dir || !*data_dir)
        data_dir = "/data";
    const char *out_dir = getenv("OUTPUT_DIR");
    if (!out_dir || !*out_dir)
        out_dir = "/output";
    unsigned nw = worker_count();

    /* ---- P0: manifest, products, links ---------------------------------- */
    static Dict dict;
    if (load_manifest(data_dir, &dict))
        return 1;
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
        /* the coordinator loads products and campaigns while the workers
           parse the link files (both are needed only from P1 on) */
        pool_start(job_links, &lq);
        int rc = load_products(data_dir, &dict) || load_campaigns(data_dir, &dict);
        pool_wait();
        if (rc)
            return 1;
    }

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
    uint64_t est_rows = 0;
    for (int i = 0; i < nfiles; i++) {
        const char *b = files[i].data, *e = files[i].data + files[i].size;
        if (!b)
            continue;
        const char *nl = memchr(b, '\n', files[i].size);
        if (!nl)
            continue;                     /* header only */
        const char *p = nl + 1;
        /* newline density of the first 64 KB: estimates the rows (picks the
           number of dedup partitions; never correctness) */
        size_t sample = (size_t)(e - p) < ((size_t)1 << 16) ? (size_t)(e - p) : ((size_t)1 << 16);
        uint64_t nls = 1;
        for (const char *x = p, *xe = p + sample; x < xe && (x = memchr(x, '\n', (size_t)(xe - x)));
             x++)
            nls++;
        while (p < e) {
            const char *q = p + chunk < e ? p + chunk : e;
            if (q < e) {
                const char *n2 = memchr(q, '\n', (size_t)(e - q));
                q = n2 ? n2 + 1 : e;
            }
            ptasks[nptasks].begin = p;
            ptasks[nptasks].end = q;
            est_rows += (uint64_t)((double)(q - p) * (double)nls / (double)(sample ? sample : 1)) + 1;
            /* a row that yields a Cand is >= 8 bytes ("e,,,,,,\n") */
            cand_cap += (size_t)(q - p) / 8 + 1;
            nptasks++;
            p = q;
        }
    }
    /* Partitioned Rec store: P dedup partitions (from the row estimate; any
       count is correct), 4 KB chunks cut from per-worker 2 MB slabs of one
       arena. Worst case: every task's bytes / 8 rows, plus one partially
       filled open chunk per worker and partition, plus the fresh Win chunks
       of P2 (mostly recycled Rec chunks), plus a partial slab per worker. */
    uint32_t pbits = 4;   /* <= 1024 partitions of >= 64K rows: dedup tables stay in L2 */
#ifndef EVX_PROWS
#define EVX_PROWS 65536
#endif
#ifndef EVX_PMAXB
#define EVX_PMAXB 10
#endif
    while (pbits < EVX_PMAXB && ((uint64_t)1 << pbits) * EVX_PROWS < est_rows)
        pbits++;
    uint32_t P = 1u << pbits;
    const uint32_t RBITS = 8, R = 1u << RBITS;   /* person ranges of the Win store */
    const uint64_t SLAB_CHUNKS = SLAB_BYTES / CHUNK_BYTES;
    uint64_t nchunk_need = cand_cap / CHUNK_RECS + nptasks + (uint64_t)P * nw +
                           cand_cap / WCHUNK_WINS + (uint64_t)R * nw + 16;
    uint64_t nslabs = (nchunk_need + SLAB_CHUNKS - 1) / SLAB_CHUNKS + 2 * (uint64_t)nw + 1;
    if (nslabs * SLAB_CHUNKS >= CHUNK_NONE) {
        fprintf(stderr, "too many event rows for the 32-bit chunk index\n");
        return 1;
    }
    size_t arena_bytes = nslabs * SLAB_BYTES;
    char *arena = big_alloc(arena_bytes);
    PartHead *heads = malloc((size_t)nw * P * sizeof *heads);
    Slab *slabs = calloc(nw, sizeof *slabs);
    uint64_t **dtable = calloc(nw, sizeof *dtable);
    if (!arena || !heads || !slabs || !dtable) {
        fprintf(stderr, "out of memory (candidates)\n");
        return 1;
    }
    uint64_t slab_next = 0;

    /* ---- P1 parse (+ raw-ID intern) ------------------------------------------
     * The raw-ID table starts at ~1/8 of the estimated rows (ids are a few %
     * of events). If more than half of it gets claimed, the parse stops and
     * is redone with a table 8x larger; at pow2 >= 2 * rows it cannot. */
    uint64_t rmax = pow2_at_least(cand_cap * 2);
    uint64_t rsize = pow2_at_least(est_rows / 8);
    {
        const char *env = getenv("EVX_RSIZE");    /* test hook: force P1 reruns */
        if (env && *env)
            rsize = pow2_at_least(strtoull(env, NULL, 10));
        if (rsize > rmax)
            rsize = rmax;
    }
    ParseCtx pctx = {
        .window_start = dict.window_start,
        .window_end = dict.window_end,
        .prod_slots = dict.prod_slots,
        .prod_mask = dict.prod_mask,
        .unknown_cat = dict.unknown_cat,
        .arena = arena,
        .pshift = 32 - pbits,
        .ws_ms = floor_div(dict.window_start, 1000) * 1000,
    };
    static PartStore ps;
    ps = (PartStore){.pctx = &pctx, .arena = arena, .heads = heads, .slabs = slabs, .P = P,
                     .pbits = pbits, .nw = nw};
    LinkJob ljob = {.dict = &dict, .lb = lbuild};
    Queue q;
    uint64_t ncand = 0, raw_claimed = 0;
    Key16 *raw = NULL;
    for (int first = 1;; first = 0) {
        raw = big_alloc_hp(rsize * sizeof *raw);
        if (!raw) {
            fprintf(stderr, "out of memory (raw ids)\n");
            return 1;
        }
        raw_claimed = 0;
        pctx.raw = raw;
        pctx.raw_mask = rsize - 1;
        pctx.raw_claimed = &raw_claimed;
        pctx.raw_limit = rsize / 2;
        slab_next = 0;
        for (unsigned w = 0; w < nw; w++)
            slabs[w] = (Slab){.arena = arena, .next = &slab_next, .nslabs = nslabs,
                              .populate = 1};
        for (size_t i = 0; i < (size_t)nw * P; i++)
            heads[i] = (PartHead){NULL, NULL, CHUNK_NONE, 0, 0};
        atomic_store(&ps.overflow, 0);
        q = (Queue){.ntasks = nptasks, .tasks = ptasks, .ctx = &ps, .ctx2 = first ? &ljob : NULL};
        atomic_init(&q.next, 0);
        pool_run(job_parse, &q);
        if (first && ljob.rc != 0)
            return 1;
        if (!atomic_load(&ps.overflow))
            break;
        big_free(raw, rsize * sizeof *raw);
        rsize = rsize * 8 < rmax ? rsize * 8 : rmax;
    }
    ncand = 0;
    for (size_t i = 0; i < nptasks; i++)
        ncand += ptasks[i].count;

    /* ---- P1.5: each distinct raw id -> canonical person -> dense id -----------
     * The person table is sized from the raw claims (persons <= raw ids), so
     * it cannot overflow unless EVX_PSIZE forces it smaller; then P1.5 alone
     * is rerun with a table 8x larger. */
    uint64_t pmax = pow2_at_least(raw_claimed * 2);
    uint64_t psize = pmax;
    {
        const char *env = getenv("EVX_PSIZE");    /* test hook: force P1.5 reruns */
        if (env && *env)
            psize = pow2_at_least(strtoull(env, NULL, 10));
        if (psize > pmax)
            psize = pmax;
    }
    uint32_t *pslot = big_alloc(rsize * sizeof *pslot);   /* raw slot -> person -> dense */
    if (!pslot) {
        fprintf(stderr, "out of memory (raw remap)\n");
        return 1;
    }
    PersonSlot *persons = NULL;
    uint32_t *counts = NULL;     /* person slot -> dense id */
    static ResolveArgs ra;
    ResolveCtx rctx;
    for (;;) {
        persons = big_alloc_hp(psize * sizeof *persons);
        if (!persons) {
            fprintf(stderr, "out of memory (persons)\n");
            return 1;
        }
        uint64_t pclaimed = 0;
        rctx = (ResolveCtx){.raw = raw, .pslot = pslot, .persons = persons,
                            .person_mask = psize - 1, .link_slots = dict.link_slots,
                            .link_mask = dict.link_mask, .claimed = &pclaimed,
                            .claim_limit = psize / 2};
        ra.ctx = &rctx;
        ra.rsize = rsize;
        ra.step = rsize / ((uint64_t)nw * 16) + 1;
        if (ra.step < 4096)
            ra.step = 4096;
        atomic_store(&ra.overflow, 0);
        q = (Queue){.ntasks = (size_t)((rsize + ra.step - 1) / ra.step), .ctx = &ra};
        atomic_init(&q.next, 0);
        pool_run(job_resolve, &q);
        if (!atomic_load(&ra.overflow))
            break;
        big_free(persons, psize * sizeof *persons);
        psize = psize * 8 < pmax ? psize * 8 : pmax;
    }
    uint32_t plog = 0;
    while (((uint64_t)1 << plog) < psize)
        plog++;
    counts = big_alloc(psize * sizeof *counts);

    /* ---- P4: dense persons ----------------------------------------------
     * Compact the occupied person slots into dense ids in slot order
     * (counts[] = slot -> dense id); names are sized by people. */
    uint64_t npersons = 0;
    uint64_t *rfirst = calloc((size_t)R + 1, sizeof *rfirst);   /* slot range -> first dense id */
    uint64_t *ebase = calloc((size_t)R + 1, sizeof *ebase);     /* person range -> first event */
    if (!rfirst || !ebase || !counts) {
        fprintf(stderr, "out of memory (ranges)\n");
        return 1;
    }
    const uint32_t rsh = plog - RBITS;          /* psize >= 1024 */
    CompactArgs ca = {.persons = persons, .remap = counts, .rsh = rsh, .rfirst = rfirst};
    q = (Queue){.ntasks = R, .ctx = &ca};
    atomic_init(&q.next, 0);
    pool_run(job_compact, &q);
    for (uint32_t r = 0; r < R; r++) {
        uint64_t n = rfirst[r];
        rfirst[r] = npersons;
        npersons += n;
    }
    rfirst[R] = npersons;
    Slice *pnames = malloc((npersons ? npersons : 1) * sizeof *pnames);
    Key16 *pkeys = malloc((npersons ? npersons : 1) * sizeof *pkeys);
    if (!pnames || !pkeys) {
        fprintf(stderr, "out of memory (names)\n");
        return 1;
    }
    ca.pkeys = pkeys;
    ca.pnames = pnames;
    ca.fill = 1;
    q = (Queue){.ntasks = R, .ctx = &ca};
    atomic_init(&q.next, 0);
    pool_run(job_compact, &q);
    /* raw slot -> dense person id, in place */
    ra.raw = raw;
    ra.pslot = pslot;
    ra.dense = counts;
    q = (Queue){.ntasks = (size_t)((rsize + ra.step - 1) / ra.step), .ctx = &ra};
    atomic_init(&q.next, 0);
    pool_run(job_rremap, &q);
    big_free(raw, rsize * sizeof *raw);

    /* ---- P2: dedup + emit per partition --------------------------------------
     * Person ranges are blocks of 2^dshift dense ids (<= R ranges). */
    uint32_t dshift = 0;
    while (npersons && ((npersons - 1) >> dshift) >= R)
        dshift++;
    PartHead *wheads = malloc((size_t)nw * R * sizeof *wheads);
    DedupCtx *dctx = calloc(nw, sizeof *dctx);
    if (!wheads || !dctx) {
        fprintf(stderr, "out of memory (wins)\n");
        return 1;
    }
    for (size_t i = 0; i < (size_t)nw * R; i++)
        wheads[i] = (PartHead){NULL, NULL, CHUNK_NONE, 0, 0};
    for (unsigned w = 0; w < nw; w++) {
        /* scratch per worker: lazily committed, sized for any partition */
        dtable[w] = big_alloc((ncand + ncand / 2 + 128) * sizeof(uint64_t));
        if (!dtable[w]) {
            fprintf(stderr, "out of memory (dedup)\n");
            return 1;
        }
        dctx[w] = (DedupCtx){.table = dtable[w], .arena = arena, .heads = heads, .nw = nw,
                             .P = P, .pbits = pbits, .dshift = dshift, .remap = pslot,
                             .wheads = wheads + (size_t)w * R, .slab = &slabs[w]};
    }
    DedupArgs da = {.ps = &ps, .dctx = dctx};
    q = (Queue){.ntasks = P, .ctx = &da};
    atomic_init(&q.next, 0);
    pool_run(job_dedup, &q);
    for (unsigned w = 0; w < nw; w++)
        big_free(dtable[w], (ncand + ncand / 2 + 128) * sizeof(uint64_t));
    big_free(pslot, rsize * sizeof *pslot);
    /* close the open Win chunks */
    for (size_t i = 0; i < (size_t)nw * R; i++)
        if (wheads[i].end)
            *(uint32_t *)(wheads[i].end - CHUNK_BYTES) =
                (uint32_t)(((char *)wheads[i].cur - (wheads[i].end - CHUNK_BYTES) - 16) / sizeof(Win));
    free(heads);

    /* person ranges: dense blocks [r << dshift, (r + 1) << dshift) */
    uint64_t nvalid = 0, maxn = 0;
    uint64_t *dfirst = calloc((size_t)R + 1, sizeof *dfirst);
    if (!dfirst) {
        fprintf(stderr, "out of memory (ranges)\n");
        return 1;
    }
    for (uint32_t r = 0; r <= R; r++) {
        uint64_t f = (uint64_t)r << dshift;
        dfirst[r] = f < npersons ? f : npersons;
    }
    for (uint32_t r = 0; r < R; r++) {
        uint64_t e = 0;
        for (unsigned w = 0; w < nw; w++)
            e += wheads[(size_t)w * R + r].count;
        ebase[r] = nvalid;
        nvalid += e;
        if (e > maxn)
            maxn = e;               /* largest range: bounds any one person */
    }
    ebase[R] = nvalid;
    uint64_t *offsets = big_alloc((npersons + 1) * sizeof(uint64_t));
    uint64_t *cursor = big_alloc((npersons ? npersons : 1) * sizeof(uint64_t));
    if (!offsets || !cursor) {
        fprintf(stderr, "out of memory (offsets)\n");
        return 1;
    }
    /* range starts; the rest of offsets[] is filled per range in P6 */
    for (uint32_t r = 0; r <= R; r++)
        offsets[dfirst[r]] = ebase[r];
    /* when trailing ranges are empty, dfirst repeats npersons: the last
       write above (r = R) leaves offsets[npersons] = nvalid */
    /* the person table and counts[] (now slot -> dense remap) stay alive
       until the click parse has resolved every click's person */


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
    big_free(persons, psize * sizeof *persons);   /* counts[] (remap) stays for P5 */
    uint64_t nclicks = 0;
    for (size_t i = 0; i < nkt; i++)
        nclicks += ktasks[i].count;

    /* ---- C2: click prefix sums ------------------------------------------------ */
    uint64_t *coffsets = big_alloc((npersons + 1) * sizeof(uint64_t));
    uint64_t *ccursor = big_alloc((npersons ? npersons : 1) * sizeof(uint64_t));
    if (!coffsets || !ccursor) {
        fprintf(stderr, "out of memory (click offsets)\n");
        return 1;
    }
    uint64_t maxc = 0;
    {
        uint64_t run = 0;
        for (uint64_t k = 0; k < npersons; k++) {
            coffsets[k] = run;
            ccursor[k] = run;
            run += ccount[k];
            if (ccount[k] > maxc)
                maxc = ccount[k];
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
        sctx[w].events = NULL;                  /* per range (job_session) */
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
        a->events = NULL;
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
    /* one task per person range (rows <= events, so rows + offsets fits) */
    size_t nst = R;
    SessTask *stasks = calloc(nst, sizeof *stasks);
    Ev **gscr = calloc(nw, sizeof *gscr);
    if (!rows || !stasks || !gscr) {
        fprintf(stderr, "out of memory (rows)\n");
        return 1;
    }
    uint64_t maxr = maxn ? maxn : 1;
    for (size_t r = 0; r < nst; r++) {
        stasks[r].pbegin = dfirst[r];
        stasks[r].pend = dfirst[r + 1];
        stasks[r].out = rows + ebase[r];
    }
    for (unsigned w = 0; w < nw; w++)
        if (!(gscr[w] = big_alloc(maxr * sizeof(Ev)))) {
            fprintf(stderr, "out of memory (range scratch)\n");
            return 1;
        }
    SessArgs sargs = {.sctx = sctx, .actx = actx, .warena = arena, .wheads = wheads, .nw = nw,
                      .R = R, .cursor = cursor, .offsets = offsets,
                      .ebase = ebase, .scratch = gscr};
    q = (Queue){.ntasks = nst, .tasks = stasks, .ctx = &sargs};
    atomic_init(&q.next, 0);
    pool_run(job_session, &q);

    /* ---- P7: Parquet: user_daily, one part per worker, in parallel ---------- */
    char odir[4096], opath[8192];
    snprintf(odir, sizeof odir, "%s/user_daily", out_dir);
    if (ensure_dir(out_dir) || ensure_dir(odir))
        return 1;
    uint64_t nrows = 0;
    for (size_t i = 0; i < nst; i++)
        nrows += stasks[i].count;
    size_t tb[MAX_WORKERS + 1];
    {
        size_t i = 0;
        uint64_t run = 0;
        tb[0] = 0;
        for (unsigned w = 0; w < nw; w++) {
            while (i < nst && run * nw < nrows * (w + 1))
                run += stasks[i++].count;
            tb[w + 1] = w + 1 == nw ? nst : i;
        }
    }
    static WriteArgs wa;
    wa = (WriteArgs){.st = stasks, .tb = tb, .pnames = pnames, .cats = dict.cats, .odir = odir};
    q = (Queue){.tasks = &wa};
    pool_run(job_write, &q);
    pool_shutdown();
    if (wa.rc)
        return 1;

    /* ---- P7b: reduce per-worker campaign/day tables -> campaign_daily ------- */
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
            cr++;
        }
    snprintf(odir, sizeof odir, "%s/campaign_daily", out_dir);
    if (ensure_dir(odir))
        return 1;
    snprintf(opath, sizeof opath, "%s/part-00000.parquet", odir);
    ColSpec ccols[7] = {
        {"campaign_id", COL_STR, cid, NULL, NULL, 0, NULL},
        {"channel", COL_STR, chn, NULL, NULL, 0, NULL},
        {"day", COL_DATE, NULL, cday, NULL, 0, NULL},
        {"sessions", COL_I64, NULL, NULL, cs, 0, NULL},
        {"purchasing_sessions", COL_I64, NULL, NULL, cp, 0, NULL},
        {"revenue_cents", COL_I64, NULL, NULL, crev, 0, NULL},
        {"assisted_sessions", COL_I64, NULL, NULL, cas, 0, NULL},
    };
    if (write_parquet(opath, ncrows, 7, ccols, NULL, 0) != 0)
        return 1;
    return 0;
}
