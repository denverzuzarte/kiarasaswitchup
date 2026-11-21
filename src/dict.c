/*
 * dict.c - metadata and identity preparation (C side, spec section 5).
 *   manifest.json  -> window bounds (parsed with the asm timestamp parser)
 *   products.csv   -> exact product_id -> category id lookup table
 *   links/ *.csv   -> DSU -> raw id -> canonical (byte-wise smallest) id table
 * Every CSV line goes through the same asm dialect splitter as the events.
 */
#define _GNU_SOURCE

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

/* Insert into a LookupEntry table; returns the entry (existing or new). */
static LookupEntry *table_put(LookupEntry *slots, uint64_t mask,
                              const char *key, uint32_t len, int *inserted)
{
    uint64_t h = evx_hash(key, len);
    uint64_t i = h & mask;
    uint32_t tag = (uint32_t)(h >> 32);
    for (;;) {
        LookupEntry *e = &slots[i];
        if (!e->key) {
            e->key = key;
            e->key_len = len;
            e->key_tag = tag;
            *inserted = 1;
            return e;
        }
        if (e->key_tag == tag && e->key_len == len && memcmp(e->key, key, len) == 0) {
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
            e->val_len = (uint32_t)find_cat(d, f[1].ptr, f[1].len);
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
        e->val_len = (uint32_t)i;
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
        e->val_len = id;
        s->ptr[id] = p;
        s->len[id] = len;
        s->parent[id] = id;
        s->size[id] = 1;
    }
    *out = e->val_len;
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
        e->val = s.ptr[m];
        e->val_len = s.len[m];
        e->val_tag = (uint32_t)(evx_hash(s.ptr[m], s.len[m]) >> 32);
    }
    free(minid);
    free(s.ptr);
    free(s.len);
    free(s.parent);
    free(s.size);
    free(s.index);
    return 0;
}
