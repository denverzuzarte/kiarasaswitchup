/*
 * dict.c - metadata and identity preparation (C side, spec section 5).
 *   manifest.json  -> window bounds (parsed with the asm timestamp parser)
 *   products.csv   -> exact product_id -> category id lookup table
 *   links/ *.csv   -> DSU -> raw id -> canonical (byte-wise smallest) id table
 * Every CSV line goes through the same asm dialect splitter as the events.
 */
#define _GNU_SOURCE

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

int bytes_cmp(const char *a, size_t al, const char *b, size_t bl)
{
    size_t n = al < bl ? al : bl;
    int c = n ? memcmp(a, b, n) : 0;
    if (c)
        return c;
    return al < bl ? -1 : (al > bl ? 1 : 0);
}

static uint64_t table_size_for(size_t n)
{
    uint64_t s = 16;
    while (s < (uint64_t)n * 2)
        s <<= 1;
    return s;
}

/* Insert a key into a LookupEntry table; returns the entry (existing or new).
   Keys are the same Key16 encoding the asm engine probes with. */
static LookupEntry *table_put(LookupEntry *slots, uint64_t mask,
                              const char *key, uint32_t len, int *inserted)
{
    Key16 k = evx_key16(key, len);
    uint64_t i = evx_khash(k.k0, k.k1) & mask;
    for (;;) {
        LookupEntry *e = &slots[i];
        if (!e->key.k1) {
            e->key = k;
            *inserted = 1;
            return e;
        }
        if (evx_keq(e->key.k0, e->key.k1, k.k0, k.k1)) {
            *inserted = 0;
            return e;
        }
        i = (i + 1) & mask;
    }
}

/* Iterate the data lines (after the header) of a mapped CSV. */
typedef struct {
    const char *p, *end;
} LineIter;

static void line_iter_init(LineIter *it, const Mapped *m)
{
    it->p = m->data;
    it->end = m->data + m->size;
    if (!m->data) {
        it->p = it->end = NULL;
        return;
    }
    const char *nl = memchr(it->p, '\n', m->size);
    it->p = nl ? nl + 1 : it->end;   /* header-only file: no data lines */
}

static int line_next(LineIter *it, const char **line, size_t *len)
{
    if (it->p >= it->end)
        return 0;
    const char *nl = memchr(it->p, '\n', (size_t)(it->end - it->p));
    const char *e = nl ? nl : it->end;
    *line = it->p;
    *len = (size_t)(e - it->p);
    it->p = nl ? nl + 1 : it->end;
    return 1;
}

/* ---------------------------------------------------------------- manifest */

static int json_ts(const char *buf, size_t n, const char *key, int64_t *out)
{
    const char *k = memmem(buf, n, key, strlen(key));
    if (!k)
        return -1;
    const char *p = k + strlen(key);
    const char *end = buf + n;
    while (p < end && *p != ':')
        p++;
    while (p < end && *p != '"')
        p++;
    if (p >= end)
        return -1;
    const char *s = ++p;
    while (p < end && *p != '"')
        p++;
    if (p >= end)
        return -1;
    return evx_parse_ts(s, (size_t)(p - s), out) ? 0 : -1;
}

int load_manifest(const char *data_dir, Dict *d)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/manifest.json", data_dir);
    Mapped m;
    if (map_file(path, &m) != 0)
        return -1;
    int rc = 0;
    if (json_ts(m.data, m.size, "\"window_start\"", &d->window_start) != 0 ||
        json_ts(m.data, m.size, "\"window_end\"", &d->window_end) != 0) {
        fprintf(stderr, "manifest: cannot read window_start/window_end\n");
        rc = -1;
    } else if (d->window_end <= d->window_start) {
        fprintf(stderr, "manifest: empty window\n");
        rc = -1;
    }
    unmap_file(&m);
    return rc;
}

/* ---------------------------------------------------------------- products */

static int find_cat(const Dict *d, const char *p, uint32_t len)
{
    for (uint32_t i = 0; i < d->ncats; i++)
        if (d->cats[i].len == len && memcmp(d->cats[i].ptr, p, len) == 0)
            return (int)i;
    return -1;
}

static int slice_cmp(const void *a, const void *b)
{
    const Slice *x = a, *y = b;
    return bytes_cmp(x->ptr, x->len, y->ptr, y->len);
}

int load_products(const char *data_dir, Dict *d)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/products.csv", data_dir);
    if (map_file(path, &d->products_file) != 0)
        return -1;

    /* pass 1: count rows and collect distinct categories */
    static const char UNKNOWN[] = "unknown";
    LineIter it;
    const char *line;
    size_t len, rows = 0;
    Slice f[2];
    d->ncats = 0;
    d->cats[d->ncats++] = (Slice){UNKNOWN, 7, 0};
    line_iter_init(&it, &d->products_file);
    while (line_next(&it, &line, &len)) {
        if (!evx_split_line(line, len, 2, f) || f[0].len == 0)
            continue;
        rows++;
        if (find_cat(d, f[1].ptr, f[1].len) < 0) {
            if (d->ncats == 256) {
                fprintf(stderr, "products: more than 255 categories\n");
                return -1;
            }
            d->cats[d->ncats++] = f[1];
        }
    }
    /* category id = rank in byte order, so a tie on revenue is won by the
       smallest id in the asm aggregation */
    qsort(d->cats, d->ncats, sizeof(Slice), slice_cmp);
    d->unknown_cat = (uint32_t)find_cat(d, UNKNOWN, 7);

    d->product_count = rows;
    uint64_t size = table_size_for(rows);
    d->prod_slots = calloc(size, sizeof(LookupEntry));
    if (!d->prod_slots)
        return -1;
    d->prod_mask = size - 1;
    line_iter_init(&it, &d->products_file);
    while (line_next(&it, &line, &len)) {
        if (!evx_split_line(line, len, 2, f) || f[0].len == 0)
            continue;
        int ins;
        LookupEntry *e = table_put(d->prod_slots, d->prod_mask, f[0].ptr, f[0].len, &ins);
        if (ins)
            e->val.k0 = (uint64_t)find_cat(d, f[1].ptr, f[1].len);
    }
    return 0;
}

/* ------------------------------------------------------------------- links */

typedef struct {
    const char **ptr;
    uint32_t *len;
    uint32_t *parent;
    uint32_t *size;
    size_t n, cap;
    LookupEntry *index;     /* id -> ordinal (val_len) */
    uint64_t mask;
} Ids;

static uint32_t dsu_find(uint32_t *parent, uint32_t x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];   /* path halving */
        x = parent[x];
    }
    return x;
}

static int ids_grow_index(Ids *s)
{
    uint64_t size = table_size_for(s->cap);
    LookupEntry *idx = calloc(size, sizeof(LookupEntry));
    if (!idx)
        return -1;
    free(s->index);
    s->index = idx;
    s->mask = size - 1;
    for (size_t i = 0; i < s->n; i++) {
        int ins;
        LookupEntry *e = table_put(s->index, s->mask, s->ptr[i], s->len[i], &ins);
        e->val.k0 = i;
    }
    return 0;
}

static int ids_intern(Ids *s, const char *p, uint32_t len, uint32_t *out)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        if (cap > UINT32_MAX)
            return -1;
        const char **np = realloc(s->ptr, cap * sizeof *np);
        if (!np)
            return -1;
        s->ptr = np;
        uint32_t *nl = realloc(s->len, cap * sizeof *nl);
        if (!nl)
            return -1;
        s->len = nl;
        uint32_t *npar = realloc(s->parent, cap * sizeof *npar);
        if (!npar)
            return -1;
        s->parent = npar;
        uint32_t *nsz = realloc(s->size, cap * sizeof *nsz);
        if (!nsz)
            return -1;
        s->size = nsz;
        s->cap = cap;
        if (ids_grow_index(s) != 0)
            return -1;
    }
    int ins;
    LookupEntry *e = table_put(s->index, s->mask, p, len, &ins);
    if (ins) {
        uint32_t id = (uint32_t)s->n++;
        e->val.k0 = id;
        s->ptr[id] = p;
        s->len[id] = len;
        s->parent[id] = id;
        s->size[id] = 1;
    }
    *out = (uint32_t)e->val.k0;
    return 0;
}

int load_links(const char *data_dir, Dict *d)
{
    char dir[4096], path[8192];
    snprintf(dir, sizeof dir, "%s/links", data_dir);
    char **names = NULL;
    int nf = list_csv(dir, &names);
    if (nf < 0)
        nf = 0;   /* no links directory: every id is its own person */
    d->link_files = calloc((size_t)(nf ? nf : 1), sizeof(Mapped));
    d->nlink_files = nf;

    Ids s = {0};
    for (int i = 0; i < nf; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        free(names[i]);
        if (map_file(path, &d->link_files[i]) != 0)
            return -1;
        LineIter it;
        const char *line;
        size_t len;
        Slice f[2];
        line_iter_init(&it, &d->link_files[i]);
        while (line_next(&it, &line, &len)) {
            d->link_lines++;
            if (!evx_split_line(line, len, 2, f) || f[0].len == 0 || f[1].len == 0) {
                d->link_dropped++;
                continue;
            }
            uint32_t a, b;
            if (ids_intern(&s, f[0].ptr, f[0].len, &a) != 0 ||
                ids_intern(&s, f[1].ptr, f[1].len, &b) != 0)
                return -1;
            a = dsu_find(s.parent, a);
            b = dsu_find(s.parent, b);
            if (a != b) {   /* union by size */
                if (s.size[a] < s.size[b]) {
                    uint32_t t = a; a = b; b = t;
                }
                s.parent[b] = a;
                s.size[a] += s.size[b];
            }
        }
    }
    free(names);

    d->link_ids = s.n;
    if (s.n == 0) {
        d->link_slots = NULL;
        d->link_mask = 0;
        free(s.index);
        return 0;
    }

    /* smallest name per component, tracked independently of the root */
    uint32_t *minid = malloc(s.n * sizeof *minid);
    if (!minid)
        return -1;
    for (size_t i = 0; i < s.n; i++)
        minid[i] = UINT32_MAX;
    for (size_t i = 0; i < s.n; i++) {
        uint32_t r = dsu_find(s.parent, (uint32_t)i);
        if (minid[r] == UINT32_MAX) {
            minid[r] = (uint32_t)i;
            d->link_components++;
        } else if (bytes_cmp(s.ptr[i], s.len[i], s.ptr[minid[r]], s.len[minid[r]]) < 0) {
            minid[r] = (uint32_t)i;
        }
    }

    uint64_t size = table_size_for(s.n);
    d->link_slots = calloc(size, sizeof(LookupEntry));
    if (!d->link_slots)
        return -1;
    d->link_mask = size - 1;
    for (size_t i = 0; i < s.n; i++) {
        uint32_t m = minid[dsu_find(s.parent, (uint32_t)i)];
        int ins;
        LookupEntry *e = table_put(d->link_slots, d->link_mask, s.ptr[i], s.len[i], &ins);
        e->val = evx_key16(s.ptr[m], s.len[m]);   /* canonical name as a Key16 */
    }
    free(minid);
    free(s.ptr);
    free(s.len);
    free(s.parent);
    free(s.size);
    free(s.index);
    return 0;
}

/* --------------------------------------------------------------- campaigns */

int load_campaigns(const char *data_dir, Dict *d)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/campaigns.csv", data_dir);
    if (map_file(path, &d->campaigns_file) != 0)
        return -1;
    LineIter it;
    const char *line;
    size_t len, rows = 0;
    Slice f[2];
    line_iter_init(&it, &d->campaigns_file);
    while (line_next(&it, &line, &len))
        rows++;
    if (rows >= UINT32_MAX - 1) {
        fprintf(stderr, "campaigns: too many rows\n");
        return -1;
    }
    d->camp_ids = calloc(rows + 1, sizeof(Slice));
    d->camp_channels = calloc(rows + 1, sizeof(Slice));
    uint64_t size = table_size_for(rows);
    d->camp_slots = calloc(size, sizeof(LookupEntry));
    if (!d->camp_ids || !d->camp_channels || !d->camp_slots)
        return -1;
    d->camp_mask = size - 1;
    d->ncamp = 0;
    line_iter_init(&it, &d->campaigns_file);
    while (line_next(&it, &line, &len)) {
        if (!evx_split_line(line, len, 2, f) || f[0].len == 0)
            continue;
        int ins;
        LookupEntry *e = table_put(d->camp_slots, d->camp_mask, f[0].ptr, f[0].len, &ins);
        if (!ins)
            continue;                    /* campaign_id is unique; keep first */
        e->val.k0 = d->ncamp;
        d->camp_ids[d->ncamp] = f[0];
        d->camp_channels[d->ncamp] = f[1];
        d->ncamp++;
    }
    static const char ORGANIC[] = "organic";
    d->camp_ids[d->ncamp] = (Slice){ORGANIC, 7, 0};
    d->camp_channels[d->ncamp] = (Slice){ORGANIC, 7, 0};
    return 0;
}

/* ------------------------------------------------------- parallel links
 *
 * L1 (all workers, byte-range tasks): split each line, drop malformed ones and
 *     empty sides, intern both ids into one shared open-addressing table of
 *     LookupEntry (the final link lookup table itself). A slot is claimed by
 *     CAS on key.k1 from 0 to LINK_BUSY, then k0 is written and k1 published
 *     with a release store; a reader that sees LINK_BUSY waits for it. A real
 *     k1 is never 1 (its top byte is a length or 0xFF). Each task appends its
 *     (slot a, slot b) pairs to its own buffer.
 * L2 (one worker): union-find over the integer pairs (union by size, path
 *     halving) on slot indices; no strings are touched.
 * L3 (same worker): smallest name per component (byte-wise), then every slot's
 *     val = its component's smallest key.
 */
#define LINK_BUSY 1ull

struct LinkBuild {
    Mapped *files;
    int nfiles;
    LinkTask *tasks;
    size_t ntasks;
    uint32_t (*pairs)[2];       /* dense ids */
    LookupEntry *slots;
    uint64_t mask;
    _Atomic uint64_t lines, dropped;
    _Atomic uint32_t next_id;   /* dense id counter */
    uint32_t *dslot;            /* dense id -> slot */
    Key16 *dkey;                /* dense id -> key (cache-friendly copy) */
};

/* Returns the id's dense number; the claimer assigns it (and records
   slot + key) before publishing k1, so a finder always sees it. */
static uint32_t link_intern(LinkBuild *lb, const char *p, uint32_t len)
{
    Key16 k = evx_key16(p, len);
    uint64_t i = evx_khash(k.k0, k.k1) & lb->mask;
    for (;;) {
        _Atomic uint64_t *k1p = (_Atomic uint64_t *)&lb->slots[i].key.k1;
        uint64_t cur = atomic_load_explicit(k1p, memory_order_acquire);
        if (cur == 0) {
            uint64_t zero = 0;
            if (atomic_compare_exchange_strong(k1p, &zero, LINK_BUSY)) {
                uint32_t id = atomic_fetch_add_explicit(&lb->next_id, 1, memory_order_relaxed);
                lb->slots[i].key.k0 = k.k0;
                lb->slots[i].val.k1 = id;            /* temporary: dense id */
                lb->dslot[id] = (uint32_t)i;
                lb->dkey[id] = k;
                atomic_store_explicit(k1p, k.k1, memory_order_release);
                return id;
            }
            cur = zero;
        }
        while (cur == LINK_BUSY)
            cur = atomic_load_explicit(k1p, memory_order_acquire);
        if (evx_keq(lb->slots[i].key.k0, cur, k.k0, k.k1))
            return (uint32_t)lb->slots[i].val.k1;
        i = (i + 1) & lb->mask;
    }
}

int links_plan(const char *data_dir, unsigned workers, LinkBuild **out)
{
    char dir[4096], path[8192];
    snprintf(dir, sizeof dir, "%s/links", data_dir);
    LinkBuild *lb = calloc(1, sizeof *lb);
    if (!lb)
        return -1;
    char **names = NULL;
    int nf = list_csv(dir, &names);
    if (nf < 0)
        nf = 0;
    lb->files = calloc((size_t)(nf ? nf : 1), sizeof(Mapped));
    lb->nfiles = nf;
    size_t bytes = 0, lines = 0;
    for (int i = 0; i < nf; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        free(names[i]);
        if (map_file(path, &lb->files[i]) != 0)
            return -1;
        bytes += lb->files[i].size;
        const char *p = lb->files[i].data, *e = p + lb->files[i].size;
        while (p && p < e) {
            const char *nl = memchr(p, '\n', (size_t)(e - p));
            lines++;
            p = nl ? nl + 1 : e;
        }
    }
    free(names);
    size_t chunk = bytes / ((size_t)workers * 8) + 1;
    if (chunk < ((size_t)1 << 18))
        chunk = (size_t)1 << 18;
    lb->tasks = calloc((size_t)nf + bytes / chunk + 16, sizeof(LinkTask));
    size_t pair_off = 0;
    for (int i = 0; i < nf; i++) {
        const char *b = lb->files[i].data, *e = b + lb->files[i].size;
        if (!b)
            continue;
        const char *nl = memchr(b, '\n', lb->files[i].size);
        if (!nl)
            continue;                        /* header only */
        for (const char *p = nl + 1; p < e;) {
            const char *q = p + chunk < e ? p + chunk : e;
            if (q < e) {
                const char *n2 = memchr(q, '\n', (size_t)(e - q));
                q = n2 ? n2 + 1 : e;
            }
            LinkTask *t = &lb->tasks[lb->ntasks++];
            t->begin = p;
            t->end = q;
            t->pair_off = pair_off;
            pair_off += (size_t)(q - p) / 4 + 1;    /* a kept line is >= 4 bytes */
            p = q;
        }
    }
    lb->pairs = big_alloc((pair_off ? pair_off : 1) * sizeof *lb->pairs);
    uint64_t size = table_size_for(lines * 2 + 1);   /* <= 2 ids per line, load <= 0.5 */
    lb->slots = big_alloc(size * sizeof(LookupEntry));
    lb->dslot = big_alloc((lines * 2 + 1) * sizeof *lb->dslot);
    lb->dkey = big_alloc((lines * 2 + 1) * sizeof *lb->dkey);
    if (!lb->pairs || !lb->slots || !lb->dslot || !lb->dkey)
        return -1;
    if (lines * 2 + 1 >= UINT32_MAX)
        return -1;
    lb->mask = size - 1;
    *out = lb;
    return 0;
}

size_t links_ntasks(const LinkBuild *lb) { return lb->ntasks; }

void links_parse_task(LinkBuild *lb, size_t ti)
{
    LinkTask *t = &lb->tasks[ti];
    const char *p = t->begin;
    uint64_t n = 0, lines = 0, dropped = 0;
    Slice f[2];
    while (p < t->end) {
        const char *nl = memchr(p, '\n', (size_t)(t->end - p));
        const char *e = nl ? nl : t->end;
        lines++;
        if (!evx_split_line(p, (size_t)(e - p), 2, f) || f[0].len == 0 || f[1].len == 0) {
            dropped++;
        } else {
            lb->pairs[t->pair_off + n][0] = link_intern(lb, f[0].ptr, f[0].len);
            lb->pairs[t->pair_off + n][1] = link_intern(lb, f[1].ptr, f[1].len);
            n++;
        }
        p = nl ? nl + 1 : t->end;
    }
    t->npairs = n;
    atomic_fetch_add(&lb->lines, lines);
    atomic_fetch_add(&lb->dropped, dropped);
}

static Slice key_bytes(const Key16 *k)
{
    if (KEY_LONG(k->k1))
        return (Slice){(const char *)k->k0, (uint32_t)((k->k1 >> 32) & 0xFFFFFF), 0};
    return (Slice){(const char *)k, (uint32_t)(k->k1 >> 56), 0};
}

int links_finish(LinkBuild *lb, Dict *d)
{
    /* everything below works on dense ids: arrays of a few MB, cache-friendly */
    uint32_t ids = atomic_load(&lb->next_id);
    uint32_t *parent = malloc(((size_t)ids + 1) * sizeof *parent);
    uint32_t *aux = malloc(((size_t)ids + 1) * sizeof *aux);   /* size, then min id */
    if (!parent || !aux)
        return -1;
    for (uint32_t i = 0; i < ids; i++) {
        parent[i] = i;
        aux[i] = 1;
    }
    for (size_t ti = 0; ti < lb->ntasks; ti++) {
        const LinkTask *t = &lb->tasks[ti];
        for (uint64_t j = 0; j < t->npairs; j++) {
            uint32_t a = dsu_find(parent, lb->pairs[t->pair_off + j][0]);
            uint32_t b = dsu_find(parent, lb->pairs[t->pair_off + j][1]);
            if (a == b)
                continue;
            if (aux[a] < aux[b]) {
                uint32_t x = a; a = b; b = x;
            }
            parent[b] = a;
            aux[a] += aux[b];
        }
    }
    for (uint32_t i = 0; i < ids; i++)
        if (parent[i] == i)
            aux[i] = UINT32_MAX;
    size_t comps = 0;
    for (uint32_t i = 0; i < ids; i++) {
        uint32_t r = dsu_find(parent, i);
        if (aux[r] == UINT32_MAX) {
            aux[r] = i;
            comps++;
            continue;
        }
        Slice x = key_bytes(&lb->dkey[i]), m = key_bytes(&lb->dkey[aux[r]]);
        if (bytes_cmp(x.ptr, x.len, m.ptr, m.len) < 0)
            aux[r] = i;
    }
    for (uint32_t i = 0; i < ids; i++)
        lb->slots[lb->dslot[i]].val = lb->dkey[aux[dsu_find(parent, i)]];
    free(parent);
    free(aux);
    d->link_ids = ids;
    d->link_lines = atomic_load(&lb->lines);
    d->link_dropped = atomic_load(&lb->dropped);
    d->link_components = comps;
    d->link_slots = ids ? lb->slots : NULL;
    d->link_mask = ids ? lb->mask : 0;
    d->link_files = lb->files;
    d->nlink_files = lb->nfiles;
    return 0;
}
