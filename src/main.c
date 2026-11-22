/*
 * main.c - C coordinator of the pipeline.
 *
 * One process, a fixed pthread pool (NCPU workers), explicit phase barriers:
 *   P0 dictionaries -> P1 parse -> P2 dedup -> P3 collect -> P4 dense ids
 *   -> P5 group -> C1-C3 clicks -> P6 sessionize + attribute -> P7 Parquet
 *   (built in RAM from the rows on all workers, each row group pwritten at
 *   its final offset). Nothing is printed unless something fails.
 * All event semantics live in the assembly engine (src/evx_*.s); this file
 * only owns memory, files, scheduling and barriers.
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
        madvise(p, (size_t)st.st_size, MADV_WILLNEED);   /* advice values are not flags */
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

/* Compact output arena: workers take private chunks from one shared array
   with a single atomic add and append into them, so output lands densely at
   the start of the array. Memory is first-touched (and zeroed by the kernel)
   only where it is written, instead of across a sparse, worst-case-sized
   region indexed by input position. */
typedef struct {
    char *base;
    size_t esz;
    uint64_t cap, chunk;          /* elements */
    _Atomic uint64_t next;
    struct { uint64_t cur, end; char pad[48]; } local[CPU_BUDGET];
} Arena;

static int arena_init(Arena *a, size_t esz, uint64_t used_max, uint64_t need_max,
                      uint64_t chunk, unsigned nw)
{
    if (chunk < need_max * 8)
        chunk = need_max * 8;
    a->esz = esz;
    a->chunk = chunk;
    /* each chunk switch abandons < need_max (<= 1/8 of a chunk) */
    a->cap = used_max + used_max / 7 + (uint64_t)(nw + 2) * chunk;
    a->base = big_alloc(a->cap * esz);
    atomic_init(&a->next, 0);
    memset(a->local, 0, sizeof a->local);
    return a->base ? 0 : -1;
}

static void arena_reset(Arena *a)
{
    atomic_store(&a->next, 0);
    memset(a->local, 0, sizeof a->local);
}

/* room for `need` elements in worker wid's chunk; commit with arena_commit */
static void *arena_take(Arena *a, unsigned wid, uint64_t need)
{
    if (a->local[wid].end - a->local[wid].cur < need) {
        uint64_t take = need > a->chunk ? need : a->chunk;
        uint64_t s = atomic_fetch_add_explicit(&a->next, take, memory_order_relaxed);
        if (s + take > a->cap) {
            fprintf(stderr, "arena overflow\n");
            exit(1);
        }
        a->local[wid].cur = s;
        a->local[wid].end = s + take;
    }
    return a->base + a->local[wid].cur * a->esz;
}

static void arena_commit(Arena *a, unsigned wid, uint64_t used) { a->local[wid].cur += used; }

static void arena_free(Arena *a) { big_free(a->base, a->cap * a->esz); }

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

/* Start fn on every worker; the coordinator may do serial work, then
   pool_wait. pool_run = start + wait: one phase barrier. */
static void pool_start(JobFn fn, void *arg)
{
    pool.fn = fn;
    pool.arg = arg;
    pthread_barrier_wait(&pool.start);
}

static void pool_wait(void) { pthread_barrier_wait(&pool.done); }

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
    Cand *out;
    uint64_t count;
} ParseTask;

typedef struct {
    Cand *c;
    uint64_t n;
} DedupTask;

typedef struct {
    const Cand *begin, *end;          /* candidate range (<= 65536) */
    Win *out;
    uint64_t count;
    uint64_t occupied;
} CollectTask;

typedef struct {
    uint64_t pbegin, pend;
    Row *out;
    uint64_t count;
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
    Arena *arena;        /* compact per-task output (collect, sessionize) */
} Queue;

/* P0b + P1 in one phase: every worker first takes link-file tasks (parse +
   intern), then event parse tasks. The worker that completes the last link
   task runs the (integer-only) union-find + smallest-name pass, then joins
   the event parse. Links are only needed from P3 on, so nothing waits for
   them; still exactly the pool's threads. */
typedef struct {
    Dict *dict;
    LinkBuild *lb;
    size_t ntasks;
    _Atomic size_t next, done;
    int rc;
} LinkJob;

static void job_parse(void *arg, unsigned wid)
{
    Queue *q = arg;
    ParseTask *t = q->tasks;
    LinkJob *lj = q->ctx2;
    if (lj->ntasks == 0) {
        if (wid == 0)
            lj->rc = links_finish(lj->lb, lj->dict);
    } else {
        size_t k;
        while ((k = atomic_fetch_add_explicit(&lj->next, 1, memory_order_relaxed)) < lj->ntasks) {
            links_parse_task(lj->lb, k);
            /* acq_rel RMW chain: the last one sees every task's writes */
            if (atomic_fetch_add_explicit(&lj->done, 1, memory_order_acq_rel) + 1 == lj->ntasks)
                lj->rc = links_finish(lj->lb, lj->dict);
        }
    }
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        t[i].out = arena_take(q->arena, wid, (uint64_t)(t[i].end - t[i].begin) / 8 + 1);
        t[i].count = evx_parse_events(q->ctx, t[i].begin, t[i].end, t[i].out);
        arena_commit(q->arena, wid, t[i].count);
    }
}

static void job_dedup(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    DedupTask *t = q->tasks;
    const uint64_t *size = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        evx_dedup_insert(q->ctx2, *size, t[i].c, t[i].n);
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
        t[i].out = arena_take(q->arena, wid, (uint64_t)(t[i].end - t[i].begin));
        t[i].count = evx_collect(q->ctx, t[i].begin, t[i].end, t[i].out, &t[i].occupied);
        if (t[i].count == UINT64_MAX)
            atomic_store(overflow, 1);
        else
            arena_commit(q->arena, wid, t[i].count);
    }
}

typedef struct {
    const uint32_t *remap;
    uint64_t *cursor;
    Ev *ev;
} ScatterArgs;

static void job_scatter(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    CollectTask *t = q->tasks;
    const ScatterArgs *a = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        evx_scatter(t[i].out, t[i].count, a->remap, a->cursor, a->ev);
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
        t[i].out = arena_take(q->arena, wid, nev);     /* rows <= events */
        t[i].count = evx_sessionize(ctx, t[i].pbegin, t[i].pend, t[i].out);
        arena_commit(q->arena, wid, t[i].count);
        evx_attribute(actx, t[i].pbegin, t[i].pend);
    }
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

/* P4: occupied person slots -> dense ids (fixed slot range per worker) */
typedef struct {
    uint32_t *counts;            /* in: events per slot; out: slot -> dense id */
    const PersonSlot *persons;
    uint64_t psize;
    unsigned nw;
    uint64_t np[CPU_BUDGET], ne[CPU_BUDGET], mx[CPU_BUDGET];   /* per range */
    uint64_t k0[CPU_BUDGET], e0[CPU_BUDGET];                   /* range prefixes */
    uint64_t *offsets, *cursor;
    Slice *pnames;
    Key16 *pkeys;
} DenseArgs;

static void range_of(uint64_t n, unsigned nw, unsigned w, uint64_t *b, uint64_t *e)
{
    *b = n * w / nw;
    *e = n * (w + 1) / nw;
}

static void job_dense_count(void *arg, unsigned wid)
{
    DenseArgs *a = arg;
    uint64_t b, e, np = 0, ne = 0, mx = 0;
    range_of(a->psize, a->nw, wid, &b, &e);
    for (uint64_t p = b; p < e; p++) {
        uint64_t c = a->counts[p];
        np += c != 0;
        ne += c;
        mx = c > mx ? c : mx;
    }
    a->np[wid] = np;
    a->ne[wid] = ne;
    a->mx[wid] = mx;
}

static void job_dense_fill(void *arg, unsigned wid)
{
    DenseArgs *a = arg;
    uint64_t b, e, k = a->k0[wid], run = a->e0[wid];
    range_of(a->psize, a->nw, wid, &b, &e);
    for (uint64_t p = b; p < e; p++) {
        uint64_t c = a->counts[p];
        if (!c)
            continue;
        a->offsets[k] = run;
        a->cursor[k] = run;
        /* output name: an inline key holds its own bytes (length in byte
           15); a long key points into the input mapping */
        Key16 key = a->persons[p];
        a->pkeys[k] = key;
        if (KEY_LONG(key.k1))
            a->pnames[k] = (Slice){(const char *)key.k0, (uint32_t)((key.k1 >> 32) & 0xFFFFFF), 0};
        else
            a->pnames[k] = (Slice){(const char *)&a->pkeys[k], (uint32_t)(key.k1 >> 56), 0};
        run += c;
        a->counts[p] = (uint32_t)k++;
    }
}

/* C2: click prefix sums over fixed person ranges */
typedef struct {
    const uint32_t *ccount;
    uint64_t n;
    unsigned nw;
    uint64_t sum[CPU_BUDGET], np[CPU_BUDGET], mx[CPU_BUDGET], base[CPU_BUDGET];
    uint64_t *coffsets, *ccursor;
} CPrefixArgs;

static void job_cprefix_count(void *arg, unsigned wid)
{
    CPrefixArgs *a = arg;
    uint64_t b, e, s = 0, np = 0, mx = 0;
    range_of(a->n, a->nw, wid, &b, &e);
    for (uint64_t k = b; k < e; k++) {
        uint64_t c = a->ccount[k];
        s += c;
        np += c != 0;
        mx = c > mx ? c : mx;
    }
    a->sum[wid] = s;
    a->np[wid] = np;
    a->mx[wid] = mx;
}

static void job_cprefix_fill(void *arg, unsigned wid)
{
    CPrefixArgs *a = arg;
    uint64_t b, e, run = a->base[wid];
    range_of(a->n, a->nw, wid, &b, &e);
    for (uint64_t k = b; k < e; k++) {
        a->coffsets[k] = run;
        a->ccursor[k] = run;
        run += a->ccount[k];
    }
}

/* P7: encode user_daily row groups from the session tasks' Row arrays */
typedef struct {
    PqFile *f;
    const SessTask *tasks;
    const size_t *gfirst, *grows;
    const Slice *pnames;
    const Slice *cats;
    _Atomic int failed;
} UdArgs;

static void job_ud_encode(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    UdArgs *a = (UdArgs *)q->ctx;
    size_t g;
    while ((g = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        PqCol *c[6];
        for (int k = 0; k < 6; k++)
            c[k] = pq_col(a->f, g, k);
        size_t n = a->grows[g];
        pq_reserve(c[0], n * 20);
        pq_reserve(c[1], n * 4);
        pq_reserve(c[2], n * 8);
        pq_reserve(c[3], n * 8);
        pq_reserve(c[4], n * 8);
        pq_reserve(c[5], n * 8);
        for (size_t t = a->gfirst[g]; t < a->gfirst[g + 1]; t++) {
            const Row *r = a->tasks[t].out, *e = r + a->tasks[t].count;
            for (; r < e; r++) {
                const Slice *u = &a->pnames[r->pid];
                pq_put_str(c[0], u->ptr, u->len);
                pq_put_i32(c[1], r->day);
                pq_put_i64(c[2], (int64_t)r->sessions);
                pq_put_i64(c[3], (int64_t)r->events);
                pq_put_i64(c[4], (int64_t)r->revenue);
                if (r->top_cat == TOPCAT_NULL)
                    pq_put_opt_str(c[5], NULL, 0);
                else
                    pq_put_opt_str(c[5], a->cats[r->top_cat].ptr, a->cats[r->top_cat].len);
            }
        }
        /* the layout is fixed (presized), so write it now: the pwrites
           (serialized per file by the kernel) overlap other groups' encoding */
        if (pq_encode_done(a->f, g) != 0)
            atomic_store(&a->failed, 1);
        else
            pq_copy(a->f, g);
    }
}

/* exact page-body sizes of every user_daily column chunk of the groups */
static void job_ud_size(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    UdArgs *a = (UdArgs *)q->ctx;
    size_t g;
    while ((g = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks) {
        size_t n = a->grows[g], uid = 0, top = 0, lv = 0, run = 0;
        int run_def = -1;
        for (size_t t = a->gfirst[g]; t < a->gfirst[g + 1]; t++) {
            const Row *r = a->tasks[t].out, *e = r + a->tasks[t].count;
            for (; r < e; r++) {
                uid += 4 + a->pnames[r->pid].len;
                int def = r->top_cat != TOPCAT_NULL;
                if (def)
                    top += 4 + a->cats[r->top_cat].len;
                if (run && def != run_def) {     /* RLE run: varint(len << 1), value byte */
                    lv += pq_varint_len((uint64_t)run << 1) + 1;
                    run = 0;
                }
                run_def = def;
                run++;
            }
        }
        if (run)
            lv += pq_varint_len((uint64_t)run << 1) + 1;
        pq_presize(a->f, g, 0, uid);
        pq_presize(a->f, g, 1, n * 4);
        pq_presize(a->f, g, 2, n * 8);
        pq_presize(a->f, g, 3, n * 8);
        pq_presize(a->f, g, 4, n * 8);
        pq_presize(a->f, g, 5, 4 + lv + top);
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
    if (load_manifest(data_dir, &dict) || load_products(data_dir, &dict) ||
        load_campaigns(data_dir, &dict))
        return 1;
    if (pool_init(nw) != 0) {
        fprintf(stderr, "thread pool init failed\n");
        return 1;
    }
    /* P0b: link files are parsed and interned by all workers at the start
       of the event parse phase (job_parse) */
    LinkBuild *lbuild = NULL;
    if (links_plan(data_dir, nw, &lbuild) != 0)
        return 1;

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
    /* candidates: dense, appended per task into worker chunks (arena) */
    uint64_t need_max = 1;
    for (size_t i = 0; i < nptasks; i++) {
        uint64_t n = (uint64_t)(ptasks[i].end - ptasks[i].begin) / 8 + 1;
        if (n > need_max)
            need_max = n;
    }
    static Arena carena;
    if (arena_init(&carena, sizeof(Cand), cand_cap, need_max, 0, nw) != 0) {
        fprintf(stderr, "out of memory (candidates)\n");
        return 1;
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
    static LinkJob ljob;
    ljob.dict = &dict;
    ljob.lb = lbuild;
    ljob.ntasks = links_ntasks(lbuild);
    Queue q = {.ntasks = nptasks, .tasks = ptasks, .ctx = &pctx, .ctx2 = &ljob, .arena = &carena};
    atomic_init(&q.next, 0);
    pool_run(job_parse, &q);
    if (ljob.rc != 0)
        return 1;
    uint64_t ncand = 0;
    for (size_t i = 0; i < nptasks; i++)
        ncand += ptasks[i].count;

    /* ---- P2: dedup: one global CAS table (1.5 slots per candidate,
     * multiply-indexed) --------------------------------------------------- */
    uint64_t dsize = ncand + ncand / 2 + 1024;
    if (dsize >= ((uint64_t)1 << 32)) {
        fprintf(stderr, "too many event rows for the 32-bit dedup index\n");
        return 1;
    }
    uint64_t *dedup = big_alloc(dsize * sizeof(uint64_t));
    DedupTask *dtasks = NULL;
    size_t ndt = 0;
    {
        size_t dt_cap = nptasks + ncand / 65536 + 16;
        dtasks = calloc(dt_cap, sizeof *dtasks);
        if (!dedup || !dtasks) {
            fprintf(stderr, "out of memory (dedup)\n");
            return 1;
        }
        for (size_t i = 0; i < nptasks; i++)
            for (uint64_t o = 0; o < ptasks[i].count; o += 65536) {
                uint64_t n = ptasks[i].count - o < 65536 ? ptasks[i].count - o : 65536;
                dtasks[ndt++] = (DedupTask){ptasks[i].out + o, n};
            }
        q = (Queue){.ntasks = ndt, .tasks = dtasks, .ctx = &dsize, .ctx2 = dedup};
        atomic_init(&q.next, 0);
        pool_start(job_dedup, &q);
    }
    /* coordinator, while the workers dedup: map and plan the click files */
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
    if (!kc || !ktasks) {
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
    pool_wait();
    /* the losers are marked dead in the candidates: the table is done */
    big_free(dedup, dsize * sizeof(uint64_t));

    /* ---- P3: collect valid winners, resolve + intern persons, count ---------
     * The person table starts at ~1/16 of the candidates (people are a few %
     * of events) and is enlarged and the phase rerun if more than half of it
     * gets claimed. At pow2 >= 2 * ncand it can no longer overflow. */
    uint64_t pmax = pow2_at_least(ncand * 2);
    uint64_t psize = pow2_at_least(ncand / 16);
    {
        const char *env = getenv("EVX_PSIZE");    /* test hook: force retries */
        if (env && *env)
            psize = pow2_at_least(strtoull(env, NULL, 10));
    }
    if (psize > pmax)
        psize = pmax;
    const uint64_t CSLOTS = 65536;
    static Arena warena;          /* collect output: dense Win runs per task */
    size_t nct = ndt;             /* the dedup ranges again: <= CSLOTS candidates */
    CollectTask *ctasks = calloc(nct, sizeof *ctasks);
    if (arena_init(&warena, sizeof(Win), ncand + 1, CSLOTS, 16 * CSLOTS, nw) != 0 || !ctasks) {
        fprintf(stderr, "out of memory (collect)\n");
        return 1;
    }
    PersonSlot *persons = NULL;
    uint32_t *counts = NULL;
    for (;;) {
        persons = big_alloc(psize * sizeof *persons);
        counts = big_alloc(psize * sizeof *counts);
        if (!persons || !counts) {
            fprintf(stderr, "out of memory (persons)\n");
            return 1;
        }
        arena_reset(&warena);
        for (size_t i = 0; i < nct; i++) {
            ctasks[i].begin = dtasks[i].c;
            ctasks[i].end = dtasks[i].c + dtasks[i].n;
            ctasks[i].out = NULL;
            ctasks[i].count = 0;
            ctasks[i].occupied = 0;
        }
        uint64_t claimed = 0;
        _Atomic int overflow = 0;
        CollectCtx cctx = {
            .persons = persons, .person_mask = psize - 1, .counts = counts,
            .link_slots = dict.link_slots, .link_mask = dict.link_mask,
            .link_filter = dict.link_filter, .link_fshift = dict.link_fshift,
            .claimed = &claimed, .claim_limit = psize / 2,
        };
        q = (Queue){.ntasks = nct, .tasks = ctasks, .ctx = &cctx, .ctx2 = (void *)&overflow,
                    .arena = &warena};
        atomic_init(&q.next, 0);
        pool_run(job_collect, &q);
        if (!atomic_load(&overflow))
            break;
        big_free(persons, psize * sizeof *persons);
        big_free(counts, psize * sizeof *counts);
        psize = psize * 8 < pmax ? psize * 8 : pmax;
    }

    /* ---- P4: dense persons + prefix sums --------------------------------
     * Person slots are sparse (table sized for the worst case). Compact the
     * occupied ones into dense ids: counts[] is reused as the slot -> dense
     * remap, so offsets/cursor/names are sized by people, not by slots. */
    uint64_t nvalid = 0, maxn = 0, npersons = 0;
    /* two parallel passes over fixed slot ranges (dense ids stay in slot
       order): count people + events per range, then fill from the range's
       prefix */
    DenseArgs da = {.counts = counts, .persons = persons, .psize = psize, .nw = nw};
    pool_run(job_dense_count, &da);
    for (unsigned w = 0; w < nw; w++) {
        da.k0[w] = npersons;
        da.e0[w] = nvalid;
        npersons += da.np[w];
        nvalid += da.ne[w];
        if (da.mx[w] > maxn)
            maxn = da.mx[w];
    }
    uint64_t *offsets = big_alloc((npersons + 1) * sizeof(uint64_t));
    uint64_t *cursor = big_alloc((npersons ? npersons : 1) * sizeof(uint64_t));
    Slice *pnames = malloc((npersons ? npersons : 1) * sizeof *pnames);
    Key16 *pkeys = malloc((npersons ? npersons : 1) * sizeof *pkeys);
    if (!offsets || !cursor || !pnames || !pkeys) {
        fprintf(stderr, "out of memory (offsets)\n");
        return 1;
    }
    da.offsets = offsets;
    da.cursor = cursor;
    da.pnames = pnames;
    da.pkeys = pkeys;
    pool_run(job_dense_fill, &da);
    offsets[npersons] = nvalid;
    /* names now live in pnames (pointing into the input mappings): the
       candidate store, dedup table and person table are no longer needed */
    /* the candidate arena stays mapped: its first pages are already
       faulted in, and the grouped events (P5) are written over them */
    /* the person table and counts[] (now slot -> dense remap) stay alive
       until the click parse has resolved every click's person */

    /* ---- P5: group events by person ---------------------------------------- */
    Ev *ev = (Ev *)carena.base;   /* nvalid * 16 <= ncand * 56: touched, dense */
    if (!ev) {
        fprintf(stderr, "out of memory (events)\n");
        return 1;
    }
    ScatterArgs sa = {.remap = counts, .cursor = cursor, .ev = ev};
    q = (Queue){.ntasks = nct, .tasks = ctasks, .ctx = &sa};
    atomic_init(&q.next, 0);
    pool_run(job_scatter, &q);
    arena_free(&warena);
    big_free(cursor, (npersons ? npersons : 1) * sizeof(uint64_t));

    /* ---- C1: parse clicks ----------------------------------------------------
     * Every failing row is dropped at parse (copies of a click are identical,
     * so no click dedup is needed); clicks of people without surviving events
     * are dropped too (they have no sessions to credit or assist). */
    uint32_t *ccount = big_alloc((npersons ? npersons : 1) * sizeof(uint32_t));
    if (!ccount) {
        fprintf(stderr, "out of memory (clicks)\n");
        return 1;
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
        .link_filter = dict.link_filter,
        .link_fshift = dict.link_fshift,
    };
    q = (Queue){.ntasks = nkt, .tasks = ktasks, .ctx = &kctx};
    atomic_init(&q.next, 0);
    pool_run(job_parse_clicks, &q);
    big_free(persons, psize * sizeof *persons);
    big_free(counts, psize * sizeof *counts);
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
        CPrefixArgs ca = {.ccount = ccount, .n = npersons, .nw = nw,
                          .coffsets = coffsets, .ccursor = ccursor};
        pool_run(job_cprefix_count, &ca);
        uint64_t run = 0;
        for (unsigned w = 0; w < nw; w++) {
            ca.base[w] = run;
            run += ca.sum[w];
            if (ca.mx[w] > maxc)
                maxc = ca.mx[w];
        }
        pool_run(job_cprefix_fill, &ca);
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
    static Arena rarena;          /* user_daily rows, dense per block */
    /* blocks of ~16K events + clicks (whales get a block of their own) */
    size_t st_cap = npersons + 1;               /* every block has >= 1 person */
    SessTask *stasks = calloc(st_cap, sizeof *stasks);
    if (arena_init(&rarena, sizeof(Row), nvalid + 1, (maxn > 16384 ? maxn : 16384) + 1,
                   (uint64_t)1 << 18, nw) != 0 || !stasks) {
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
        stasks[nst].out = NULL;                 /* taken from rarena */
        nst++;
        k = e;
    }
    q = (Queue){.ntasks = nst, .tasks = stasks, .ctx = actx, .ctx2 = sctx, .arena = &rarena};
    atomic_init(&q.next, 0);
    pool_run(job_session, &q);

    /* ---- P7: user_daily, built in RAM straight from the rows --------------
     * Row groups are runs of consecutive session tasks (~nrows / 4nw rows);
     * every group is encoded by one worker from its tasks' Row arrays, then
     * the workers pwrite the groups at their final offsets. */
    uint64_t nrows = 0;
    for (size_t i = 0; i < nst; i++)
        nrows += stasks[i].count;
    uint64_t gtarget = nrows / ((uint64_t)nw * 4) + 1;
    if (gtarget < 65536)
        gtarget = 65536;
    size_t *gfirst = malloc((nst + 2) * sizeof *gfirst);
    size_t *grows = malloc((nst + 1) * sizeof *grows);
    if (!gfirst || !grows) {
        fprintf(stderr, "out of memory (row groups)\n");
        return 1;
    }
    size_t ngroups = 0;
    for (size_t i = 0; i < nst;) {
        size_t j = i;
        uint64_t n = 0;
        while (j < nst && (n == 0 || n + stasks[j].count <= gtarget))
            n += stasks[j++].count;
        gfirst[ngroups] = i;
        grows[ngroups++] = n;
        i = j;
    }
    if (ngroups == 0) {                  /* no rows: one empty row group */
        gfirst[0] = 0;
        grows[0] = 0;
        ngroups = 1;
    }
    gfirst[ngroups] = nst;
    static const PqColDef udefs[6] = {
        {"user_id", COL_STR}, {"day", COL_DATE}, {"sessions", COL_I64},
        {"events", COL_I64}, {"revenue_cents", COL_I64}, {"top_category", COL_STR_OPT},
    };
    PqFile *uf = pq_begin(ngroups, grows, 6, udefs);
    char odir[4096], opath[8192];
    snprintf(odir, sizeof odir, "%s/user_daily", out_dir);
    if (!uf || ensure_dir(out_dir) || ensure_dir(odir))
        return 1;
    snprintf(opath, sizeof opath, "%s/part-00000.parquet", odir);
    UdArgs ua = {.f = uf, .tasks = stasks, .gfirst = gfirst, .grows = grows,
                 .pnames = pnames, .cats = dict.cats};
    q = (Queue){.ntasks = ngroups, .ctx = &ua};
    atomic_init(&q.next, 0);
    pool_run(job_ud_size, &q);
    if (pq_layout(uf, opath) != 0)
        return 1;
    q = (Queue){.ntasks = ngroups, .ctx = &ua};
    atomic_init(&q.next, 0);
    pool_start(job_ud_encode, &q);      /* encode + write; campaign_daily meanwhile */

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
    static const PqColDef cdefs[7] = {
        {"campaign_id", COL_STR}, {"channel", COL_STR}, {"day", COL_DATE},
        {"sessions", COL_I64}, {"purchasing_sessions", COL_I64},
        {"revenue_cents", COL_I64}, {"assisted_sessions", COL_I64},
    };
    PqFile *cf = pq_begin(1, &ncrows, 7, cdefs);
    if (!cf)
        return 1;
    PqCol *cc[7];
    for (int c = 0; c < 7; c++)
        cc[c] = pq_col(cf, 0, c);
    for (uint32_t c = 0; c <= dict.ncamp; c++)
        for (uint32_t d = 0; d < ndays; d++) {
            size_t x = (size_t)c * ndays + d;
            if (!tot_s[x] && !tot_a[x])
                continue;
            pq_put_str(cc[0], dict.camp_ids[c].ptr, dict.camp_ids[c].len);
            pq_put_str(cc[1], dict.camp_channels[c].ptr, dict.camp_channels[c].len);
            pq_put_i32(cc[2], (int32_t)(day0 + d));
            pq_put_i64(cc[3], (int64_t)tot_s[x]);
            pq_put_i64(cc[4], (int64_t)tot_p[x]);
            pq_put_i64(cc[5], (int64_t)tot_rev[x]);
            pq_put_i64(cc[6], (int64_t)tot_a[x]);
        }
    snprintf(odir, sizeof odir, "%s/campaign_daily", out_dir);
    if (ensure_dir(odir))
        return 1;
    snprintf(opath, sizeof opath, "%s/part-00000.parquet", odir);
    if (pq_encode_done(cf, 0) != 0 || pq_layout(cf, opath) != 0)
        return 1;
    pq_copy(cf, 0);
    if (pq_close(cf) != 0)
        return 1;
    pool_wait();
    pool_shutdown();
    if (atomic_load(&ua.failed) || pq_close(uf) != 0)
        return 1;
    return 0;
}
