/*
 * evx.h - ABI contract between the C orchestrator and the x86-64 assembly
 * event engine. Every offset used by src/evx_*.s is pinned here with a
 * _Static_assert; change both sides together.
 */
#ifndef EVX_H
#define EVX_H

#include <stddef.h>
#include <stdint.h>

/* One CSV field after unquoting. 16 bytes. */
typedef struct {
    const char *ptr;
    uint32_t len;
    uint32_t pad;
} Slice;

/*
 * Read-only open-addressing table entry built by C, probed by asm.
 * Index = hash & mask, tag = hash >> 32, linear probing, key == NULL is empty.
 *   links:    key = raw user id, val = canonical (smallest) id, val_tag = hash(val) >> 32
 *   products: key = product id,  val_len = category id
 */
typedef struct {
    const char *key;      /*  0 */
    uint32_t key_len;     /*  8 */
    uint32_t key_tag;     /* 12 */
    const char *val;      /* 16 */
    uint32_t val_len;     /* 24 */
    uint32_t val_tag;     /* 28 */
} LookupEntry;            /* 32 */

/* Context for evx_parse_events (read-only, shared by all workers). */
typedef struct {
    int64_t window_start;         /*  0 */
    int64_t window_end;           /*  8 */
    const LookupEntry *link_slots;/* 16 (NULL when there are no links) */
    uint64_t link_mask;           /* 24 */
    const LookupEntry *prod_slots;/* 32 (NULL when there are no products) */
    uint64_t prod_mask;           /* 40 */
    uint32_t unknown_cat;         /* 48 */
    uint32_t pad;                 /* 52 */
} ParseCtx;                       /* 56 */

/* One structurally valid event row with a non-empty event_id. 64 bytes. */
#define CAND_VALID 1u
typedef struct {
    uint64_t eid_hash;    /*  0 */
    const char *eid_ptr;  /*  8 */
    uint32_t eid_len;     /* 16 */
    uint32_t flags;       /* 20  bit0 = passes validation + late rule */
    int64_t ingest;       /* 24  epoch ms */
    int64_t ts_off;       /* 32  ts - window_start (valid rows only) */
    const char *pname_ptr;/* 40  canonical person name (valid rows only) */
    uint32_t pname_len;   /* 48 */
    uint32_t pname_tag;   /* 52  evx_hash(name) >> 32 */
    uint32_t price_cat;   /* 56  price (24 bits, 0 unless purchase) | cat << 24 */
    uint32_t pad;         /* 60 */
} Cand;

/* Context for evx_collect. */
typedef struct {
    const uint64_t *dedup;   /*  0  slots: tag16 << 48 | Cand*  */
    const Cand **persons;    /*  8  person table: Cand* defining the name */
    uint64_t person_mask;    /* 16 */
    uint32_t *counts;        /* 24  events per person slot */
} CollectCtx;                /* 32 */

/* Valid dedup winner, before grouping. 16 bytes. */
typedef struct {
    int64_t ts_off;     /* 0 */
    uint32_t pid;       /* 8 */
    uint32_t price_cat; /* 12 */
} Win;

/* Event grouped by person. 16 bytes. */
typedef struct {
    int64_t ts_off;     /* 0 */
    uint32_t price_cat; /* 8 */
    uint32_t pad;       /* 12 */
} Ev;

/* Per-worker context for evx_sessionize. */
typedef struct {
    const uint64_t *offsets;  /*    0  persons + 1 prefix sums into events */
    Ev *events;               /*    8 */
    Ev *scratch;              /*   16  >= largest person */
    int64_t day0;             /*   24  floor(window_start / 86400000) */
    uint64_t base_ms;         /*   32  window_start - day0 * 86400000 */
    uint32_t radix_passes;    /*   40  bytes needed for ts_off */
    uint32_t pad;             /*   44 */
    uint64_t cat_sum[256];    /*   48 */
    uint64_t touched[4];      /* 2096 */
} SessCtx;                    /* 2128 */

/* One user/day output row. 32 bytes. */
#define TOPCAT_NULL 0xFFFFFFFFu
typedef struct {
    uint32_t pid;       /*  0 */
    int32_t day;        /*  4  days since epoch */
    uint32_t sessions;  /*  8 */
    uint32_t top_cat;   /* 12  TOPCAT_NULL if no purchase */
    uint64_t events;    /* 16 */
    uint64_t revenue;   /* 24 */
} Row;

_Static_assert(sizeof(Slice) == 16, "Slice");
_Static_assert(sizeof(LookupEntry) == 32, "LookupEntry");
_Static_assert(offsetof(LookupEntry, val) == 16, "LookupEntry.val");
_Static_assert(offsetof(LookupEntry, val_tag) == 28, "LookupEntry.val_tag");
_Static_assert(sizeof(ParseCtx) == 56, "ParseCtx");
_Static_assert(offsetof(ParseCtx, unknown_cat) == 48, "ParseCtx.unknown_cat");
_Static_assert(sizeof(Cand) == 64, "Cand");
_Static_assert(offsetof(Cand, ingest) == 24, "Cand.ingest");
_Static_assert(offsetof(Cand, pname_ptr) == 40, "Cand.pname_ptr");
_Static_assert(offsetof(Cand, price_cat) == 56, "Cand.price_cat");
_Static_assert(sizeof(CollectCtx) == 32, "CollectCtx");
_Static_assert(sizeof(Win) == 16, "Win");
_Static_assert(sizeof(Ev) == 16, "Ev");
_Static_assert(offsetof(SessCtx, radix_passes) == 40, "SessCtx.radix_passes");
_Static_assert(offsetof(SessCtx, cat_sum) == 48, "SessCtx.cat_sum");
_Static_assert(offsetof(SessCtx, touched) == 2096, "SessCtx.touched");
_Static_assert(sizeof(Row) == 32, "Row");

/* ---- assembly entry points (System V AMD64) ---- */
uint64_t evx_hash(const void *p, size_t len);
int evx_memeq(const void *a, const void *b, size_t len);
/* 1 = valid line with exactly nfields fields (out[] filled), 0 = malformed */
int evx_split_line(const char *p, size_t len, uint32_t nfields, Slice *out);
/* 1 = valid F1/F2/F3 timestamp (*out = epoch ms), 0 = bad */
int evx_parse_ts(const char *p, size_t len, int64_t *out);

/* Parse the event rows in [begin, end) (no header). Returns Cand count. */
uint64_t evx_parse_events(const ParseCtx *ctx, const char *begin,
                          const char *end, Cand *out);
void evx_dedup_insert(uint64_t *table, uint64_t mask, Cand *c, uint64_t n);
uint64_t evx_collect(const CollectCtx *ctx, uint64_t slot_begin,
                     uint64_t slot_end, Win *out, uint64_t *occupied);
void evx_scatter(const Win *w, uint64_t n, const uint32_t *remap,
                 uint64_t *cursor, Ev *dst);
uint64_t evx_sessionize(SessCtx *ctx, uint64_t pbegin, uint64_t pend, Row *out);

#endif
