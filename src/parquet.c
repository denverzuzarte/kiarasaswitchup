/*
 * parquet.c - minimal Parquet writer (user_daily and campaign_daily).
 *
 * Replaces the DuckDB C API output path, which is unavailable on this build
 * host (see docs/SPEC_REVIEW.md). Format: "PAR1", one data page (v1) per
 * column per row group, PLAIN encoding, UNCOMPRESSED, Thrift compact
 * protocol FileMetaData footer, "PAR1".
 *
 * Column kinds: COL_STR (BYTE_ARRAY, UTF8/STRING), COL_STR_OPT (same,
 * OPTIONAL with RLE definition levels), COL_DATE (INT32 DATE), COL_I64.
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
    int ext;            /* p is caller scratch: never realloc'd or freed */
} Buf;

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->oom || b->n + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *p = b->ext ? malloc(cap) : realloc(b->p, cap);
    if (!p) {
        b->oom = 1;
        return;
    }
    if (b->ext)
        memcpy(p, b->p, b->n);
    b->p = p;
    b->cap = cap;
    b->ext = 0;
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

#define ROWS_PER_GROUP ((size_t)1 << 20)

typedef struct {
    int64_t offset;      /* data page offset in file */
    int64_t total_size;  /* page header + page body */
    int64_t num_values;
} ChunkInfo;

static int col_type(ColKind k)
{
    return k == COL_DATE || k == COL_I32 ? PT_INT32 : k == COL_I64 ? PT_INT64 : PT_BYTE_ARRAY;
}
static int col_rep(ColKind k) { return k == COL_STR_OPT ? REP_OPTIONAL : REP_REQUIRED; }
static int col_conv(ColKind k)
{
    return k == COL_DATE ? CT_DATE : k == COL_I64 || k == COL_I32 ? CT_NONE : CT_UTF8;
}
static int col_logical(ColKind k)
{
    return k == COL_DATE ? LT_DATE : k == COL_I64 || k == COL_I32 ? LT_NONE : LT_STRING;
}

/* row i's string (NULL ptr = null), 32-bit and 64-bit values */
static inline Slice col_str(const ColSpec *c, size_t i)
{
    if (!c->idx)
        return c->str[i];
    uint32_t x = *(const uint32_t *)((const char *)c->idx + i * c->stride);
    return x == 0xFFFFFFFFu ? (Slice){NULL, 0, 0} : c->str[x];
}
static inline int32_t col_i32(const ColSpec *c, size_t i)
{
    return c->stride ? *(const int32_t *)((const char *)c->date + i * c->stride) : c->date[i];
}
static inline int64_t col_i64(const ColSpec *c, size_t i)
{
    return c->stride ? *(const int64_t *)((const char *)c->i64 + i * c->stride) : c->i64[i];
}

/* Encode one column's page body for rows [r0, r1). The exact size is
   reserved first, so values are stored with plain unchecked copies. */
static void encode_column(Buf *page, const ColSpec *c, size_t r0, size_t r1)
{
    size_t n = r1 - r0, bytes = 0;
    switch (c->kind) {
    case COL_STR:
    case COL_STR_OPT:
        for (size_t i = r0; i < r1; i++) {
            Slice v = col_str(c, i);
            bytes += v.ptr ? 4 + (size_t)v.len : 0;
        }
        bytes += c->kind == COL_STR_OPT ? 4 + 11 * n : 0;   /* def levels: worst case */
        break;
    case COL_DATE:
    case COL_I32:
        bytes = 4 * n;
        break;
    case COL_I64:
        bytes = 8 * n;
        break;
    }
    buf_reserve(page, bytes);
    if (page->oom)
        return;
    uint8_t *o = page->p + page->n;
    switch (c->kind) {
    case COL_STR:
        for (size_t i = r0; i < r1; i++) {
            Slice v = col_str(c, i);
            memcpy(o, &v.len, 4);
            memcpy(o + 4, v.ptr, v.len);
            o += 4 + v.len;
        }
        break;
    case COL_DATE:
    case COL_I32:
        if (!c->stride) {
            memcpy(o, c->date + r0, 4 * n);
            o += 4 * n;
            break;
        }
        for (size_t i = r0; i < r1; i++, o += 4) {
            int32_t v = col_i32(c, i);
            memcpy(o, &v, 4);
        }
        break;
    case COL_I64:
        if (!c->stride) {
            memcpy(o, c->i64 + r0, 8 * n);
            o += 8 * n;
            break;
        }
        for (size_t i = r0; i < r1; i++, o += 8) {
            int64_t v = col_i64(c, i);
            memcpy(o, &v, 8);
        }
        break;
    case COL_STR_OPT: {
        /* definition levels: RLE/bit-packed hybrid, bit width 1, RLE runs
           only, prefixed with their byte length */
        uint8_t *lenp = o;
        o += 4;
        size_t i = r0;
        while (i < r1) {
            int def = col_str(c, i).ptr != NULL;
            size_t j = i + 1;
            while (j < r1 && (col_str(c, j).ptr != NULL) == def)
                j++;
            uint64_t v = (uint64_t)(j - i) << 1;
            while (v >= 0x80) {
                *o++ = (uint8_t)(v | 0x80);
                v >>= 7;
            }
            *o++ = (uint8_t)v;
            *o++ = (uint8_t)def;
            i = j;
        }
        uint32_t lvn = (uint32_t)(o - lenp - 4);
        memcpy(lenp, &lvn, 4);
        for (size_t k = r0; k < r1; k++) {
            Slice v = col_str(c, k);
            if (!v.ptr)
                continue;
            memcpy(o, &v.len, 4);
            memcpy(o + 4, v.ptr, v.len);
            o += 4 + v.len;
        }
        break;
    }
    }
    page->n = (size_t)(o - page->p);
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

int write_parquet(const char *path, size_t nrows, int ncols, const ColSpec *cols,
                  void *scratch, size_t scratch_cap)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    size_t ngroups = nrows ? (nrows + ROWS_PER_GROUP - 1) / ROWS_PER_GROUP : 1;
    ChunkInfo *info = calloc(ngroups * (size_t)ncols, sizeof *info);
    int64_t pos = 0;
    int rc = -1;
    Buf page = {0}, hdr = {0};
    if (scratch && scratch_cap) {
        page.p = scratch;
        page.cap = scratch_cap;
        page.ext = 1;
    }
    else
        buf_reserve(&page, (nrows < ROWS_PER_GROUP ? nrows : ROWS_PER_GROUP) * 24 + 4096);
    if (!info)
        goto out;
    if (write_all(fd, "PAR1", 4) != 0)
        goto out;
    pos = 4;

    for (size_t g = 0; g < ngroups; g++) {
        size_t r0 = g * ROWS_PER_GROUP;
        size_t r1 = r0 + ROWS_PER_GROUP < nrows ? r0 + ROWS_PER_GROUP : nrows;
        for (int col = 0; col < ncols; col++) {
            page.n = 0;
            hdr.n = 0;
            encode_column(&page, &cols[col], r0, r1);
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
            ChunkInfo *ci = &info[g * (size_t)ncols + col];
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
    th_list(&t, 2, T_STRUCT, (size_t)ncols + 1);    /* schema */
    th_begin_elem(&t);
    th_bin(&t, 4, "schema", 6);
    th_i32(&t, 5, ncols);                           /* num_children */
    th_end(&t);
    for (int col = 0; col < ncols; col++) {
        ColKind k = cols[col].kind;
        th_begin_elem(&t);
        th_i32(&t, 1, col_type(k));
        th_i32(&t, 3, col_rep(k));
        th_bin(&t, 4, cols[col].name, strlen(cols[col].name));
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
    th_list(&t, 4, T_STRUCT, ngroups);              /* row_groups */
    for (size_t g = 0; g < ngroups; g++) {
        size_t r0 = g * ROWS_PER_GROUP;
        size_t r1 = r0 + ROWS_PER_GROUP < nrows ? r0 + ROWS_PER_GROUP : nrows;
        int64_t total = 0;
        th_begin_elem(&t);
        th_list(&t, 1, T_STRUCT, (size_t)ncols);              /* columns */
        for (int col = 0; col < ncols; col++) {
            const ChunkInfo *ci = &info[g * (size_t)ncols + col];
            total += ci->total_size;
            th_begin_elem(&t);
            th_i64(&t, 2, ci->offset);              /* file_offset */
            th_begin(&t, 3);                        /* ColumnMetaData */
            th_i32(&t, 1, col_type(cols[col].kind));
            th_list(&t, 2, T_I32, 2);
            put_zigzag(&meta, 0);                   /* PLAIN */
            put_zigzag(&meta, 3);                   /* RLE */
            th_list(&t, 3, T_BINARY, 1);
            put_varint(&meta, strlen(cols[col].name));
            put(&meta, cols[col].name, strlen(cols[col].name));
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
    if (!page.ext)
        free(page.p);
    free(hdr.p);
    free(info);
    if (close(fd) != 0)
        rc = -1;
    if (rc != 0)
        fprintf(stderr, "parquet: failed writing %s\n", path);
    return rc;
}
