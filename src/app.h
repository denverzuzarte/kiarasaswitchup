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
    size_t link_ids, link_lines, link_dropped, link_components;
    Mapped *link_files;
    int nlink_files;
} Dict;

int load_manifest(const char *data_dir, Dict *d);
int load_products(const char *data_dir, Dict *d);
int load_links(const char *data_dir, Dict *d);

/* byte-wise comparison: memcmp, then shorter first */
int bytes_cmp(const char *a, size_t al, const char *b, size_t bl);

/* Parquet output of user_daily. Columns are passed as flat arrays. */
typedef struct {
    size_t nrows;
    const Slice *user_id;
    const int32_t *day;
    const int64_t *sessions;
    const int64_t *events;
    const int64_t *revenue;
    const Slice *top_category;   /* ptr == NULL -> null */
} UserDailyColumns;

int write_user_daily_parquet(const char *path, const UserDailyColumns *c);

#endif
