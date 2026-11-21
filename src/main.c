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
} SessTask;

typedef struct {
    _Atomic size_t next;
    size_t ntasks;
    void *tasks;
    const void *ctx;
    void *ctx2;
} Queue;

static void job_parse(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    ParseTask *t = q->tasks;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks)
        t[i].count = evx_parse_events(q->ctx, t[i].begin, t[i].end, t[i].out);
}

static void job_dedup(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    DedupTask *t = q->tasks;
    const uint64_t *mask = q->ctx;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks)
        evx_dedup_insert(q->ctx2, *mask, t[i].c, t[i].n);
}

static void job_collect(void *arg, unsigned wid)
{
    (void)wid;
    Queue *q = arg;
    CollectTask *t = q->tasks;
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks)
        t[i].count = evx_collect(q->ctx, t[i].slot_begin, t[i].slot_end, t[i].out,
                                 &t[i].occupied);
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
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks)
        evx_scatter(t[i].out, t[i].count, a->remap, a->cursor, a->ev);
}

static void job_session(void *arg, unsigned wid)
{
    Queue *q = arg;
    SessTask *t = q->tasks;
    SessCtx *ctx = &((SessCtx *)q->ctx2)[wid];
    size_t i;
    while ((i = atomic_fetch_add_explicit(&q->next, 1, memory_order_relaxed)) < q->ntasks)
        t[i].count = evx_sessionize(ctx, t[i].pbegin, t[i].pend, t[i].out);
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
        load_links(data_dir, &dict))
        return 1;
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

    if (pool_init(nw) != 0) {
        fprintf(stderr, "thread pool init failed\n");
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
    Queue q = {.ntasks = nptasks, .tasks = ptasks, .ctx = &pctx};
    atomic_init(&q.next, 0);
    uint64_t t_p1 = now_ns();
    pool_run(job_parse, &q);
    uint64_t t_parse = now_ns();
    uint64_t ncand = 0;
    for (size_t i = 0; i < nptasks; i++)
        ncand += ptasks[i].count;

    /* ---- P2: global dedup -------------------------------------------------- */
    uint64_t dsize = pow2_at_least(ncand * 2);
    uint64_t dmask = dsize - 1;
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
    q = (Queue){.ntasks = ndt, .tasks = dtasks, .ctx = &dmask, .ctx2 = dedup};
    atomic_init(&q.next, 0);
    pool_run(job_dedup, &q);
    uint64_t t_dedup = now_ns();

    /* ---- P3: collect valid winners, intern persons, count ----------------- */
    uint64_t psize = pow2_at_least(ncand * 2);
    const Cand **persons = big_alloc(psize * sizeof *persons);
    uint32_t *counts = big_alloc(psize * sizeof *counts);
    Win *wins = big_alloc(dsize * sizeof(Win));
    const uint64_t CSLOTS = 65536;
    size_t nct = (size_t)((dsize + CSLOTS - 1) / CSLOTS);
    CollectTask *ctasks = calloc(nct, sizeof *ctasks);
    if (!persons || !counts || !wins || !ctasks) {
        fprintf(stderr, "out of memory (collect)\n");
        return 1;
    }
    for (size_t i = 0; i < nct; i++) {
        ctasks[i].slot_begin = i * CSLOTS;
        ctasks[i].slot_end = (i + 1) * CSLOTS < dsize ? (i + 1) * CSLOTS : dsize;
        ctasks[i].out = wins + ctasks[i].slot_begin;
    }
    CollectCtx cctx = {.dedup = dedup, .persons = persons, .person_mask = psize - 1, .counts = counts};
    q = (Queue){.ntasks = nct, .tasks = ctasks, .ctx = &cctx};
    atomic_init(&q.next, 0);
    pool_run(job_collect, &q);
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
    if (!offsets || !cursor || !pnames) {
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
            pnames[k] = (Slice){persons[p]->pname_ptr, persons[p]->pname_len, 0};
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
    big_free((void *)persons, psize * sizeof *persons);
    big_free(cands, cand_bytes);
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
    big_free(counts, psize * sizeof *counts);
    big_free(cursor, (npersons ? npersons : 1) * sizeof(uint64_t));
    uint64_t t_scatter = now_ns();

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
    Row *rows = big_alloc((nvalid ? nvalid : 1) * sizeof(Row));
    /* blocks of ~16K events (whales get a block of their own) */
    size_t st_cap = npersons + 1;               /* every block has >= 1 person */
    SessTask *stasks = calloc(st_cap, sizeof *stasks);
    if (!rows || !stasks) {
        fprintf(stderr, "out of memory (rows)\n");
        return 1;
    }
    size_t nst = 0;
    for (uint64_t k = 0; k < npersons;) {
        uint64_t e = k + 1;
        while (e < npersons && e - k < 4096 && offsets[e + 1] - offsets[k] <= 16384)
            e++;
        stasks[nst].pbegin = k;
        stasks[nst].pend = e;
        stasks[nst].out = rows + offsets[k];   /* rows <= events */
        nst++;
        k = e;
    }
    q = (Queue){.ntasks = nst, .tasks = stasks, .ctx2 = sctx};
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
    UserDailyColumns cols = {nrows, uid, day, sess, evs, rev, top};
    if (write_user_daily_parquet(opath, &cols) != 0)
        return 1;
    uint64_t t_end = now_ns();

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    fprintf(stderr,
            "evx: workers=%u files=%d bytes=%zu tasks=%zu\n"
            "evx: products=%zu categories=%u link_ids=%zu link_lines=%zu dropped=%zu components=%zu\n"
            "evx: candidates=%llu unique_ids=%llu duplicates=%llu valid_events=%llu persons=%llu max_person=%llu\n"
            "evx: sessions=%llu rows=%llu peak_rss=%ld MiB\n"
            "evx: ms dict=%.1f map+plan=%.1f parse=%.1f dedup=%.1f collect=%.1f prefix=%.1f "
            "group=%.1f session=%.1f parquet=%.1f total=%.1f\n",
            nw, nfiles, total_bytes, nptasks, dict.product_count, dict.ncats, dict.link_ids,
            dict.link_lines, dict.link_dropped, dict.link_components,
            (unsigned long long)ncand, (unsigned long long)nunique,
            (unsigned long long)(ncand - nunique), (unsigned long long)nvalid,
            (unsigned long long)npersons, (unsigned long long)maxn,
            (unsigned long long)sum_sessions, (unsigned long long)nrows, ru.ru_maxrss / 1024,
            ms(t0, t_dict), ms(t_dict, t_p1), ms(t_p1, t_parse), ms(t_parse, t_dedup),
            ms(t_dedup, t_collect), ms(t_collect, t_prefix), ms(t_prefix, t_scatter),
            ms(t_scatter, t_session), ms(t_session, t_end), ms(t0, t_end));
    double cpu_s = (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                   (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    double wall_s = (double)(t_end - t0) / 1e9;
    fprintf(stderr, "evx: cpu user=%.2fs sys=%.2fs avg_cores=%.2f (worker threads=%u + 1 idle coordinator)\n",
            (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
            (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, cpu_s / wall_s, nw);
    return 0;
}
