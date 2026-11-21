/*
 * parquet.c - minimal Parquet writer for the user_daily table.
 *
 * Replaces the DuckDB C API output path, which is unavailable on this build
 * host (see docs/SPEC_REVIEW.md). Format: "PAR1", one data page (v1) per
 * column per row group, PLAIN encoding, UNCOMPRESSED, Thrift compact
 * protocol FileMetaData footer, "PAR1".
 *
 *   user_id       BYTE_ARRAY  REQUIRED  UTF8 / STRING
 *   day           INT32       REQUIRED  DATE
 *   sessions      INT64       REQUIRED
 *   events        INT64       REQUIRED
 *   revenue_cents INT64       REQUIRED
 *   top_category  BYTE_ARRAY  OPTIONAL  UTF8 / STRING (RLE definition levels)
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
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
static void put_le32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void put_le64(Buf *b, uint64_t v) { put(b, &v, 8); }

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

typedef struct {
    const char *name;
    int type, rep, conv, logical;
} ColDef;

static const ColDef COLS[6] = {
    {"user_id", PT_BYTE_ARRAY, REP_REQUIRED, CT_UTF8, LT_STRING},
    {"day", PT_INT32, REP_REQUIRED, CT_DATE, LT_DATE},
    {"sessions", PT_INT64, REP_REQUIRED, CT_NONE, LT_NONE},
    {"events", PT_INT64, REP_REQUIRED, CT_NONE, LT_NONE},
    {"revenue_cents", PT_INT64, REP_REQUIRED, CT_NONE, LT_NONE},
    {"top_category", PT_BYTE_ARRAY, REP_OPTIONAL, CT_UTF8, LT_STRING},
};

#define ROWS_PER_GROUP ((size_t)1 << 20)

typedef struct {
    int64_t offset;      /* data page offset in file */
    int64_t total_size;  /* page header + page body */
    int64_t num_values;
} ChunkInfo;

/* Encode one column's page body for rows [r0, r1). */
static void encode_column(Buf *page, int col, const UserDailyColumns *c, size_t r0, size_t r1)
{
    switch (col) {
    case 0:
        for (size_t i = r0; i < r1; i++) {
            put_le32(page, c->user_id[i].len);
            put(page, c->user_id[i].ptr, c->user_id[i].len);
        }
        break;
    case 1:
        for (size_t i = r0; i < r1; i++)
            put_le32(page, (uint32_t)c->day[i]);
        break;
    case 2:
    case 3:
    case 4: {
        const int64_t *v = col == 2 ? c->sessions : col == 3 ? c->events : c->revenue;
        for (size_t i = r0; i < r1; i++)
            put_le64(page, (uint64_t)v[i]);
        break;
    }
    case 5: {
        /* definition levels: RLE/bit-packed hybrid, bit width 1, RLE runs only,
           prefixed with their byte length */
        Buf lv = {0};
        size_t i = r0;
        while (i < r1) {
            int def = c->top_category[i].ptr != NULL;
            size_t j = i + 1;
            while (j < r1 && (c->top_category[j].ptr != NULL) == def)
                j++;
            put_varint(&lv, (uint64_t)(j - i) << 1);
            put_u8(&lv, (uint8_t)def);
            i = j;
        }
        put_le32(page, (uint32_t)lv.n);
        put(page, lv.p, lv.n);
        if (lv.oom)
            page->oom = 1;
        free(lv.p);
        for (size_t k = r0; k < r1; k++) {
            if (!c->top_category[k].ptr)
                continue;
            put_le32(page, c->top_category[k].len);
            put(page, c->top_category[k].ptr, c->top_category[k].len);
        }
        break;
    }
    }
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *s = p;
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

int write_user_daily_parquet(const char *path, const UserDailyColumns *c)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    size_t ngroups = c->nrows ? (c->nrows + ROWS_PER_GROUP - 1) / ROWS_PER_GROUP : 1;
    ChunkInfo *info = calloc(ngroups * 6, sizeof *info);
    int64_t pos = 0;
    int rc = -1;
    Buf page = {0}, hdr = {0};
    if (!info)
        goto out;
    if (write_all(fd, "PAR1", 4) != 0)
        goto out;
    pos = 4;

    for (size_t g = 0; g < ngroups; g++) {
        size_t r0 = g * ROWS_PER_GROUP;
        size_t r1 = r0 + ROWS_PER_GROUP < c->nrows ? r0 + ROWS_PER_GROUP : c->nrows;
        for (int col = 0; col < 6; col++) {
            page.n = 0;
            hdr.n = 0;
            encode_column(&page, col, c, r0, r1);
            if (page.oom || page.n > INT32_MAX)
                goto out;
            Thrift t = {.b = &hdr};
            th_i32(&t, 1, 0);                       /* DATA_PAGE */
            th_i32(&t, 2, (int32_t)page.n);
            th_i32(&t, 3, (int32_t)page.n);
            th_begin(&t, 5);                        /* DataPageHeader */
            th_i32(&t, 1, (int32_t)(r1 - r0));
            th_i32(&t, 2, 0);                       /* PLAIN */
            th_i32(&t, 3, 3);                       /* RLE def levels */
            th_i32(&t, 4, 3);                       /* RLE rep levels */
            th_end(&t);
            put_u8(&hdr, T_STOP);
            if (hdr.oom)
                goto out;
            ChunkInfo *ci = &info[g * 6 + col];
            ci->offset = pos;
            ci->total_size = (int64_t)(hdr.n + page.n);
            ci->num_values = (int64_t)(r1 - r0);
            if (write_all(fd, hdr.p, hdr.n) != 0 || write_all(fd, page.p, page.n) != 0)
                goto out;
            pos += ci->total_size;
        }
    }

    /* FileMetaData */
    Buf meta = {0};
    Thrift t = {.b = &meta};
    th_i32(&t, 1, 1);                               /* version */
    th_list(&t, 2, T_STRUCT, 7);                    /* schema */
    th_begin_elem(&t);
    th_bin(&t, 4, "schema", 6);
    th_i32(&t, 5, 6);                               /* num_children */
    th_end(&t);
    for (int col = 0; col < 6; col++) {
        const ColDef *d = &COLS[col];
        th_begin_elem(&t);
        th_i32(&t, 1, d->type);
        th_i32(&t, 3, d->rep);
        th_bin(&t, 4, d->name, strlen(d->name));
        if (d->conv != CT_NONE)
            th_i32(&t, 6, d->conv);
        if (d->logical != LT_NONE) {
            th_begin(&t, 10);                       /* LogicalType union */
            th_begin(&t, d->logical);               /* StringType / DateType {} */
            th_end(&t);
            th_end(&t);
        }
        th_end(&t);
    }
    th_i64(&t, 3, (int64_t)c->nrows);
    th_list(&t, 4, T_STRUCT, ngroups);              /* row_groups */
    for (size_t g = 0; g < ngroups; g++) {
        size_t r0 = g * ROWS_PER_GROUP;
        size_t r1 = r0 + ROWS_PER_GROUP < c->nrows ? r0 + ROWS_PER_GROUP : c->nrows;
        int64_t total = 0;
        th_begin_elem(&t);
        th_list(&t, 1, T_STRUCT, 6);                /* columns */
        for (int col = 0; col < 6; col++) {
            const ChunkInfo *ci = &info[g * 6 + col];
            total += ci->total_size;
            th_begin_elem(&t);
            th_i64(&t, 2, ci->offset);              /* file_offset */
            th_begin(&t, 3);                        /* ColumnMetaData */
            th_i32(&t, 1, COLS[col].type);
            th_list(&t, 2, T_I32, 2);
            put_zigzag(&meta, 0);                   /* PLAIN */
            put_zigzag(&meta, 3);                   /* RLE */
            th_list(&t, 3, T_BINARY, 1);
            put_varint(&meta, strlen(COLS[col].name));
            put(&meta, COLS[col].name, strlen(COLS[col].name));
            th_i32(&t, 4, 0);                       /* UNCOMPRESSED */
            th_i64(&t, 5, ci->num_values);
            th_i64(&t, 6, ci->total_size);
            th_i64(&t, 7, ci->total_size);
            th_i64(&t, 9, ci->offset);              /* data_page_offset */
            th_end(&t);
            th_end(&t);
        }
        th_i64(&t, 2, total);
        th_i64(&t, 3, (int64_t)(r1 - r0));
        th_end(&t);
    }
    th_bin(&t, 6, "evx events-only engine", 22);    /* created_by */
    put_u8(&meta, T_STOP);
    if (!meta.oom && write_all(fd, meta.p, meta.n) == 0) {
        uint32_t mlen = (uint32_t)meta.n;
        if (write_all(fd, &mlen, 4) == 0 && write_all(fd, "PAR1", 4) == 0)
            rc = 0;
    }
    free(meta.p);
out:
    free(page.p);
    free(hdr.p);
    free(info);
    if (close(fd) != 0)
        rc = -1;
    if (rc != 0)
        fprintf(stderr, "parquet: failed writing %s\n", path);
    return rc;
}
