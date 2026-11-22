/*
 * parquet.c - in-memory Parquet builder (user_daily and campaign_daily).
 *
 * Replaces the DuckDB C API output path, which is unavailable on this build
 * host (see docs/SPEC_REVIEW.md). Format: "PAR1", one data page (v1) per
 * column per row group, PLAIN encoding, UNCOMPRESSED, Thrift compact
 * protocol FileMetaData footer, "PAR1".
 *
 * The file is built in RAM straight from the caller's rows: each row group is
 * encoded independently (pq_encode, any worker) into its own buffers through
 * the pq_put_* calls; pq_layout then fixes every offset, sizes the output
 * file with ftruncate and maps it shared; pq_copy (any worker) copies one
 * group's pages into the mapping; pq_close writes nothing more (the footer
 * is copied by pq_layout) and unmaps. No column arrays, no write() of data.
 *
 * Column kinds: COL_STR (BYTE_ARRAY, UTF8/STRING), COL_STR_OPT (same,
 * OPTIONAL with RLE definition levels), COL_DATE (INT32 DATE), COL_I64.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int oom;
} Buf;

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->oom || b->n + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) {
        b->oom = 1;
        return;
    }
    b->p = p;
    b->cap = cap;
}

static void put(Buf *b, const void *src, size_t n)
{
    buf_reserve(b, n);
    if (b->oom)
        return;
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static void put_u8(Buf *b, uint8_t v) { put(b, &v, 1); }

static void put_varint(Buf *b, uint64_t v)
{
    while (v >= 0x80) {
        put_u8(b, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    put_u8(b, (uint8_t)v);
}

static void put_zigzag(Buf *b, int64_t v)
{
    put_varint(b, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
}

/* ---- Thrift compact protocol ------------------------------------------- */

enum { T_STOP = 0, T_I32 = 5, T_I64 = 6, T_BINARY = 8, T_LIST = 9, T_STRUCT = 12 };

typedef struct {
    Buf *b;
    int last[16];
    int depth;
} Thrift;

static void th_field(Thrift *t, int id, int type)
{
    int delta = id - t->last[t->depth];
    if (delta > 0 && delta <= 15) {
        put_u8(t->b, (uint8_t)((delta << 4) | type));
    } else {
        put_u8(t->b, (uint8_t)type);
        put_zigzag(t->b, id);
    }
    t->last[t->depth] = id;
}

static void th_i32(Thrift *t, int id, int32_t v) { th_field(t, id, T_I32); put_zigzag(t->b, v); }
static void th_i64(Thrift *t, int id, int64_t v) { th_field(t, id, T_I64); put_zigzag(t->b, v); }

static void th_bin(Thrift *t, int id, const char *s, size_t n)
{
    th_field(t, id, T_BINARY);
    put_varint(t->b, n);
    put(t->b, s, n);
}

static void th_begin(Thrift *t, int id)   /* struct field */
{
    th_field(t, id, T_STRUCT);
    t->last[++t->depth] = 0;
}

static void th_begin_elem(Thrift *t)      /* struct as list element */
{
    t->last[++t->depth] = 0;
}

static void th_end(Thrift *t)
{
    put_u8(t->b, T_STOP);
    t->depth--;
}

static void th_list(Thrift *t, int id, int elem_type, size_t n)
{
    th_field(t, id, T_LIST);
    if (n < 15) {
        put_u8(t->b, (uint8_t)((n << 4) | elem_type));
    } else {
        put_u8(t->b, (uint8_t)(0xF0 | elem_type));
        put_varint(t->b, n);
    }
}

/* ---- schema ------------------------------------------------------------- */

enum { PT_INT32 = 1, PT_INT64 = 2, PT_BYTE_ARRAY = 6 };
enum { REP_REQUIRED = 0, REP_OPTIONAL = 1 };
enum { CT_UTF8 = 0, CT_DATE = 6, CT_NONE = -1 };
enum { LT_NONE = 0, LT_STRING = 1, LT_DATE = 6 };

static int col_type(ColKind k) { return k == COL_DATE ? PT_INT32 : k == COL_I64 ? PT_INT64 : PT_BYTE_ARRAY; }
static int col_rep(ColKind k) { return k == COL_STR_OPT ? REP_OPTIONAL : REP_REQUIRED; }
static int col_conv(ColKind k) { return k == COL_DATE ? CT_DATE : k == COL_I64 ? CT_NONE : CT_UTF8; }
static int col_logical(ColKind k) { return k == COL_DATE ? LT_DATE : k == COL_I64 ? LT_NONE : LT_STRING; }

/* One column chunk of one row group: page header + body, then its place. */
struct PqCol {
    Buf hdr, body;
    Buf lv;                  /* COL_STR_OPT: definition-level RLE runs */
    uint64_t run;            /* current def-level run length */
    int run_def;
    int64_t offset, total;
};

struct PqFile {
    size_t ngroups;
    const size_t *group_rows;
    int ncols;
    const PqColDef *defs;
    struct PqCol *chunk;     /* [ngroups * ncols] */
    Buf meta;
    uint8_t *map;
    size_t size;
    int fd, failed;
};

PqFile *pq_begin(size_t ngroups, const size_t *group_rows, int ncols, const PqColDef *defs)
{
    PqFile *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->ngroups = ngroups;
    f->group_rows = group_rows;
    f->ncols = ncols;
    f->defs = defs;
    f->fd = -1;
    f->chunk = calloc(ngroups * (size_t)ncols + 1, sizeof *f->chunk);
    if (!f->chunk) {
        free(f);
        return NULL;
    }
    return f;
}

PqCol *pq_col(PqFile *f, size_t group, int col) { return &f->chunk[group * (size_t)f->ncols + col]; }

void pq_reserve(PqCol *c, size_t bytes) { buf_reserve(&c->body, bytes); }

void pq_put_str(PqCol *c, const char *p, uint32_t len)
{
    buf_reserve(&c->body, 4 + (size_t)len);
    if (c->body.oom)
        return;
    memcpy(c->body.p + c->body.n, &len, 4);
    memcpy(c->body.p + c->body.n + 4, p, len);
    c->body.n += 4 + (size_t)len;
}

void pq_put_i32(PqCol *c, int32_t v) { put(&c->body, &v, 4); }
void pq_put_i64(PqCol *c, int64_t v) { put(&c->body, &v, 8); }

static void lv_flush(PqCol *c)
{
    if (!c->run)
        return;
    put_varint(&c->lv, c->run << 1);        /* RLE run, bit width 1 */
    put_u8(&c->lv, (uint8_t)c->run_def);
    c->run = 0;
}

/* COL_STR_OPT: p == NULL is a null */
void pq_put_opt_str(PqCol *c, const char *p, uint32_t len)
{
    int def = p != NULL;
    if (c->run && c->run_def != def)
        lv_flush(c);
    c->run_def = def;
    c->run++;
    if (def)
        pq_put_str(c, p, len);
}

/* Finish one row group: page headers (bodies are complete). */
int pq_encode_done(PqFile *f, size_t group)
{
    size_t rows = f->group_rows[group];
    for (int col = 0; col < f->ncols; col++) {
        PqCol *c = pq_col(f, group, col);
        if (f->defs[col].kind == COL_STR_OPT) {
            /* page body = u32 level length, levels, values */
            lv_flush(c);
            Buf b = {0};
            uint32_t ln = (uint32_t)c->lv.n;
            buf_reserve(&b, 4 + c->lv.n + c->body.n);
            put(&b, &ln, 4);
            put(&b, c->lv.p, c->lv.n);
            put(&b, c->body.p, c->body.n);
            if (c->lv.oom)
                b.oom = 1;
            free(c->lv.p);
            free(c->body.p);
            c->lv = (Buf){0};
            c->body = b;
        }
        if (c->body.oom || c->body.n > INT32_MAX)
            return -1;
        Thrift t = {.b = &c->hdr};
        th_i32(&t, 1, 0);                       /* DATA_PAGE */
        th_i32(&t, 2, (int32_t)c->body.n);
        th_i32(&t, 3, (int32_t)c->body.n);
        th_begin(&t, 5);                        /* DataPageHeader */
        th_i32(&t, 1, (int32_t)rows);
        th_i32(&t, 2, 0);                       /* PLAIN */
        th_i32(&t, 3, 3);                       /* RLE def levels */
        th_i32(&t, 4, 3);                       /* RLE rep levels */
        th_end(&t);
        put_u8(&c->hdr, T_STOP);
        if (c->hdr.oom)
            return -1;
        c->total = (int64_t)(c->hdr.n + c->body.n);
    }
    return 0;
}

static void build_footer(PqFile *f)
{
    size_t nrows = 0;
    for (size_t g = 0; g < f->ngroups; g++)
        nrows += f->group_rows[g];
    Buf *meta = &f->meta;
    Thrift t = {.b = meta};
    th_i32(&t, 1, 1);                               /* version */
    th_list(&t, 2, T_STRUCT, (size_t)f->ncols + 1); /* schema */
    th_begin_elem(&t);
    th_bin(&t, 4, "schema", 6);
    th_i32(&t, 5, f->ncols);                        /* num_children */
    th_end(&t);
    for (int col = 0; col < f->ncols; col++) {
        ColKind k = f->defs[col].kind;
        th_begin_elem(&t);
        th_i32(&t, 1, col_type(k));
        th_i32(&t, 3, col_rep(k));
        th_bin(&t, 4, f->defs[col].name, strlen(f->defs[col].name));
        if (col_conv(k) != CT_NONE)
            th_i32(&t, 6, col_conv(k));
        if (col_logical(k) != LT_NONE) {
            th_begin(&t, 10);                       /* LogicalType union */
            th_begin(&t, col_logical(k));           /* StringType / DateType {} */
            th_end(&t);
            th_end(&t);
        }
        th_end(&t);
    }
    th_i64(&t, 3, (int64_t)nrows);
    th_list(&t, 4, T_STRUCT, f->ngroups);           /* row_groups */
    for (size_t g = 0; g < f->ngroups; g++) {
        int64_t total = 0;
        th_begin_elem(&t);
        th_list(&t, 1, T_STRUCT, (size_t)f->ncols); /* columns */
        for (int col = 0; col < f->ncols; col++) {
            const PqCol *c = pq_col(f, g, col);
            const char *name = f->defs[col].name;
            total += c->total;
            th_begin_elem(&t);
            th_i64(&t, 2, c->offset);               /* file_offset */
            th_begin(&t, 3);                        /* ColumnMetaData */
            th_i32(&t, 1, col_type(f->defs[col].kind));
            th_list(&t, 2, T_I32, 2);
            put_zigzag(meta, 0);                    /* PLAIN */
            put_zigzag(meta, 3);                    /* RLE */
            th_list(&t, 3, T_BINARY, 1);
            put_varint(meta, strlen(name));
            put(meta, name, strlen(name));
            th_i32(&t, 4, 0);                       /* UNCOMPRESSED */
            th_i64(&t, 5, (int64_t)f->group_rows[g]);
            th_i64(&t, 6, c->total);
            th_i64(&t, 7, c->total);
            th_i64(&t, 9, c->offset);               /* data_page_offset */
            th_end(&t);
            th_end(&t);
        }
        th_i64(&t, 2, total);
        th_i64(&t, 3, (int64_t)f->group_rows[g]);
        th_end(&t);
    }
    th_bin(&t, 6, "evx engine", 10);                /* created_by */
    put_u8(meta, T_STOP);
    uint32_t mlen = (uint32_t)meta->n;
    put(meta, &mlen, 4);
    put(meta, "PAR1", 4);
}

static int pwrite_all(int fd, const void *p, size_t n, off_t off)
{
    const uint8_t *s = p;
    while (n) {
        ssize_t w = pwrite(fd, s, n, off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        s += w;
        n -= (size_t)w;
        off += w;
    }
    return 0;
}

/* After every group is encoded: offsets, footer, sized + mapped file. */
int pq_layout(PqFile *f, const char *path)
{
    int64_t pos = 4;
    for (size_t g = 0; g < f->ngroups; g++)
        for (int col = 0; col < f->ncols; col++) {
            PqCol *c = pq_col(f, g, col);
            c->offset = pos;
            pos += c->total;
        }
    build_footer(f);
    if (f->meta.oom)
        return -1;
    f->size = (size_t)pos + f->meta.n;
    f->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (f->fd < 0) {
        perror(path);
        return -1;
    }
    if (ftruncate(f->fd, (off_t)f->size) != 0) {
        perror(path);
        return -1;
    }
    void *m = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    f->map = m == MAP_FAILED ? NULL : m;    /* NULL: pq_copy falls back to pwrite */
    if (f->map) {
        memcpy(f->map, "PAR1", 4);
        memcpy(f->map + pos, f->meta.p, f->meta.n);
        return 0;
    }
    if (pwrite_all(f->fd, "PAR1", 4, 0) || pwrite_all(f->fd, f->meta.p, f->meta.n, pos))
        return -1;
    return 0;
}

/* Copy one row group's pages to the file (any worker, after pq_layout). */
void pq_copy(PqFile *f, size_t group)
{
    for (int col = 0; col < f->ncols; col++) {
        PqCol *c = pq_col(f, group, col);
        if (f->map) {
            memcpy(f->map + c->offset, c->hdr.p, c->hdr.n);
            memcpy(f->map + c->offset + c->hdr.n, c->body.p, c->body.n);
        } else if (pwrite_all(f->fd, c->hdr.p, c->hdr.n, c->offset) ||
                   pwrite_all(f->fd, c->body.p, c->body.n, c->offset + (off_t)c->hdr.n)) {
            f->failed = 1;
        }
    }
}

int pq_close(PqFile *f)
{
    int rc = f->failed ? -1 : 0;
    if (f->map)
        munmap(f->map, f->size);
    if (f->fd >= 0 && close(f->fd) != 0)
        rc = -1;
    if (rc)
        fprintf(stderr, "parquet: write failed\n");
    return rc;
}
