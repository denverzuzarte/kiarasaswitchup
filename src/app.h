/* app.h - C-side shared declarations (orchestrator, dictionaries, Parquet). */
#ifndef APP_H
#define APP_H

#include <stddef.h>
#include <stdint.h>

#include "evx.h"

typedef struct {
    const char *data;   /* read-only mapping (NULL when size == 0) */
    size_t size;
} Mapped;

/* mmap a whole file read-only; returns 0 on success. */
int map_file(const char *path, Mapped *m);
void unmap_file(Mapped *m);
/* Anonymous, lazily committed (MAP_NORESERVE) zeroed memory. */
void *big_alloc(size_t bytes);
void big_free(void *p, size_t bytes);
uint64_t now_ns(void);

/* Sorted list of *.csv names in dir; returns count, -1 on error. */
int list_csv(const char *dir, char ***names_out);

typedef struct {
    int64_t window_start, window_end;

    /* products: exact product_id -> category id (byte-order rank) */
    LookupEntry *prod_slots;
    uint64_t prod_mask;
    size_t product_count;
    Mapped products_file;

    /* categories sorted byte-wise; unknown_cat indexes "unknown" */
    Slice cats[256];
    uint32_t ncats;
    uint32_t unknown_cat;

    /* links: raw id -> canonical (smallest) id of its DSU component */
    LookupEntry *link_slots;
    uint64_t link_mask;
    uint64_t *link_filter;      /* one bit per khash >> link_fshift: a linked id */
    uint64_t link_fshift;
    size_t link_ids, link_lines, link_dropped, link_components;
    Mapped *link_files;
    int nlink_files;

    /* campaigns: exact campaign_id -> dense id (val_len), file order;
       "organic" is id ncamp and never a key */
    LookupEntry *camp_slots;
    uint64_t camp_mask;
    uint32_t ncamp;
    Slice *camp_ids, *camp_channels;
    Mapped campaigns_file;
} Dict;

int load_manifest(const char *data_dir, Dict *d);
int load_products(const char *data_dir, Dict *d);
int load_links(const char *data_dir, Dict *d);
int load_campaigns(const char *data_dir, Dict *d);

/* Parallel links build (see dict.c): plan -> parse tasks on all workers ->
   finish (union-find + smallest names) on one worker. */
typedef struct {
    const char *begin, *end;
    uint64_t pair_off, npairs;
} LinkTask;
typedef struct LinkBuild LinkBuild;
int links_plan(const char *data_dir, unsigned workers, LinkBuild **out);
size_t links_ntasks(const LinkBuild *lb);
uint64_t links_task_pairs(const LinkBuild *lb, size_t task);
void links_parse_task(LinkBuild *lb, size_t task);
int links_finish(LinkBuild *lb, Dict *d);

/* byte-wise comparison: memcmp, then shorter first */
int bytes_cmp(const char *a, size_t al, const char *b, size_t bl);

/* Generic Parquet writer: one column descriptor per column. */
typedef enum { COL_STR, COL_STR_OPT, COL_DATE, COL_I64 } ColKind;
typedef struct {
    const char *name;
    ColKind kind;
    const Slice *str;        /* COL_STR / COL_STR_OPT (ptr == NULL -> null) */
    const int32_t *date;     /* COL_DATE: days since epoch */
    const int64_t *i64;      /* COL_I64 */
} ColSpec;

int write_parquet(const char *path, size_t nrows, int ncols, const ColSpec *cols);

#endif
