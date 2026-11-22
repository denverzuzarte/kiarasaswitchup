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
 * Key16: every ID (event, user, product, campaign) as 16 bytes, compared as
 * two machine words instead of chasing a pointer into the CSV text.
 *   len <= 15: k0 = bytes 0..7, k1 = bytes 8..14 | len << 56   (exact, inline)
 *   len >= 16: k0 = pointer to the bytes, k1 = 0xFF << 56 | min(len, 2^24-1) << 32
 *              | evx_hash32; equality = equal k1 and evx_memeq of the bytes (exact)
 * k1 == 0 never occurs for a non-empty ID (it marks empty slots / invalid rows).
 * In memory an inline key holds the name bytes at offset 0 and the length in
 * byte 15, so output can point straight at it.
 */
typedef struct {
    uint64_t k0;
    uint64_t k1;
} Key16;
#define KEY_LONG(k1) (((k1) >> 56) == 0xFF)

/*
 * Read-only open-addressing table entry built by C, probed by asm.
 * Index = evx_khash(key) & mask, linear probing, k1 == 0 is empty.
 *   links:     key = raw user id, val = canonical (smallest) id as a Key16
 *   products:  val.k0 = category id;  campaigns: val.k0 = campaign id
 */
typedef struct {
    Key16 key;            /*  0 */
    Key16 val;            /* 16 */
} LookupEntry;            /* 32 */

/* Context for evx_parse_events (read-only, shared by all workers). */
typedef struct {
    int64_t window_start;         /*  0 */
    int64_t window_end;           /*  8 */
    const LookupEntry *link_slots;/* 16 unused by parse (links resolve in collect) */
    uint64_t link_mask;           /* 24 */
    const LookupEntry *prod_slots;/* 32 (NULL when there are no products) */
    uint64_t prod_mask;           /* 40 */
    uint32_t unknown_cat;         /* 48 */
    uint32_t pad;                 /* 52 */
} ParseCtx;                       /* 56 */

/*
 * One structurally valid event row with a non-empty event_id. 56 bytes.
 * Validation (incl. the late rule) is evaluated once at parse time; an invalid
 * row has user.k1 == 0 but still competes in dedup. Dedup also zeroes user.k1
 * of every losing copy, so after P2 user.k1 != 0 means "live and valid". The
 * person is resolved later, in evx_collect, which walks the Cands in order.
 */
typedef struct {
    Key16 eid;            /*  0 */
    int64_t ingest;       /* 16  epoch ms */
    Key16 user;           /* 24  user.k1 == 0: this copy fails validation */
    int64_t ts_off;       /* 40  ts - window_start (valid rows only) */
    uint32_t price_cat;   /* 48  price (24 bits, 0 unless purchase) | cat << 24 */
    uint32_t eid_h32;     /* 52  low 32 bits of evx_khash(eid) */
} Cand;

/* Person table slot: the canonical name as a Key16 (claimed with cmpxchg16b). */
typedef Key16 PersonSlot;

/* Context for evx_collect. */
typedef struct {
    const void *unused;           /*  0 */
    PersonSlot *persons;          /*  8 */
    uint64_t person_mask;         /* 16 */
    uint32_t *counts;             /* 24  events per person slot */
    const LookupEntry *link_slots;/* 32  NULL when there are no links */
    uint64_t link_mask;           /* 40 */
    uint64_t *claimed;            /* 48  shared count of claimed person slots */
    uint64_t claim_limit;         /* 56  > this many claims: table too small */
    const uint64_t *link_filter;  /* 64  bit (khash >> link_fshift) set: id may be linked */
    uint64_t link_fshift;         /* 72 */
} CollectCtx;                     /* 80 */

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
_Static_assert(sizeof(Key16) == 16, "Key16");
_Static_assert(sizeof(LookupEntry) == 32, "LookupEntry");
_Static_assert(offsetof(LookupEntry, val) == 16, "LookupEntry.val");
_Static_assert(sizeof(ParseCtx) == 56, "ParseCtx");
_Static_assert(offsetof(ParseCtx, unknown_cat) == 48, "ParseCtx.unknown_cat");
_Static_assert(sizeof(Cand) == 56, "Cand");
_Static_assert(offsetof(Cand, ingest) == 16, "Cand.ingest");
_Static_assert(offsetof(Cand, user) == 24, "Cand.user");
_Static_assert(offsetof(Cand, ts_off) == 40, "Cand.ts_off");
_Static_assert(offsetof(Cand, price_cat) == 48, "Cand.price_cat");
_Static_assert(offsetof(Cand, eid_h32) == 52, "Cand.eid_h32");
_Static_assert(sizeof(PersonSlot) == 16, "PersonSlot");
_Static_assert(sizeof(CollectCtx) == 80, "CollectCtx");
_Static_assert(offsetof(CollectCtx, link_filter) == 64, "CollectCtx.link_filter");
_Static_assert(offsetof(CollectCtx, claim_limit) == 56, "CollectCtx.claim_limit");
_Static_assert(sizeof(Win) == 16, "Win");
_Static_assert(sizeof(Ev) == 16, "Ev");
_Static_assert(offsetof(SessCtx, radix_passes) == 40, "SessCtx.radix_passes");
_Static_assert(offsetof(SessCtx, cat_sum) == 48, "SessCtx.cat_sum");
_Static_assert(offsetof(SessCtx, touched) == 2096, "SessCtx.touched");
_Static_assert(sizeof(Row) == 32, "Row");

/* ---------------------------------------------------------------- clicks */

/* Context for evx_parse_clicks (read-only, shared). */
typedef struct {
    int64_t cw_start;               /*  0  window_start - 7 days */
    int64_t cw_end;                 /*  8  window_end */
    const LookupEntry *link_slots;  /* 16  NULL when there are no links */
    uint64_t link_mask;             /* 24 */
    const LookupEntry *camp_slots;  /* 32  campaign_id -> dense id (val_len) */
    uint64_t camp_mask;             /* 40 */
    const PersonSlot *persons;      /* 48  events person table (name -> slot) */
    uint64_t person_mask;           /* 56 */
    const uint32_t *remap;          /* 64  person slot -> dense person id */
    uint32_t *ccount;               /* 72  clicks per dense person (lock inc) */
    const uint64_t *link_filter;    /* 80  see CollectCtx */
    uint64_t link_fshift;           /* 88 */
} ClickParseCtx;                    /* 96 */

/* A valid click of a person who has sessions. 32 bytes. */
typedef struct {
    uint64_t k;           /*  0  ts - cw_start (>= 0) */
    uint32_t pid;         /*  8  dense person id */
    uint32_t camp;        /* 12  dense campaign id */
    const char *cid;      /* 16  click_id (read only for equal-ts ties) */
    uint32_t cid_len;     /* 24 */
    uint32_t pad;         /* 28 */
} ClickCand;

/* Click grouped by person; Ev-shaped (8-byte sort key at 0, 16 bytes). */
typedef struct {
    uint64_t k;           /*  0 */
    uint32_t camp;        /*  8 */
    uint32_t cand_idx;    /* 12  index into the ClickCand store */
} ClickEv;

/* One session of the person being attributed. 24 bytes. */
typedef struct {
    int64_t start;        /*  0  ts_off of the first event */
    uint32_t day_rel;     /*  8  day - day0 */
    uint32_t pad;         /* 12 */
    uint64_t revenue;     /* 16 */
} SessRec;

/* campaign/day aggregate cell, per worker. 24 bytes. */
typedef struct {
    uint32_t sessions;    /*  0  credited sessions */
    uint32_t purchasing;  /*  4 */
    uint32_t assisted;    /*  8 */
    uint32_t pad;         /* 12 */
    uint64_t revenue;     /* 16 */
} AggCell;

/* Per-worker context for evx_attribute. */
typedef struct {
    const uint64_t *offsets;   /*   0  events per dense person (prefix) */
    const Ev *events;          /*   8  sorted by evx_sessionize */
    const uint64_t *coffsets;  /*  16  clicks per dense person (prefix) */
    ClickEv *clicks;           /*  24 */
    const ClickCand *ccands;   /*  32 */
    ClickEv *cscratch;         /*  40  >= max clicks of one person */
    SessRec *sess;             /*  48  >= max events of one person */
    uint32_t *cnt;             /*  56  [ncamp] zero between persons */
    uint32_t *enter;           /*  64  [ncamp] */
    uint32_t *day_first;       /*  72  [ndays + 2] */
    AggCell *agg;              /*  80  [(ncamp + 1) * ndays] */
    uint64_t base_ms;          /*  88  window_start - day0 * 86400000 */
    uint32_t ncamp;            /*  96  organic = ncamp */
    uint32_t ndays;            /* 100 */
    uint32_t cpasses;          /* 104  radix passes for click keys */
    uint32_t pad;              /* 108 */
    uint32_t hist[8 * 256];    /* 112  radix histograms */
} AttrCtx;

_Static_assert(sizeof(ClickParseCtx) == 96, "ClickParseCtx");
_Static_assert(offsetof(ClickParseCtx, link_filter) == 80, "ClickParseCtx.link_filter");
_Static_assert(sizeof(ClickCand) == 32, "ClickCand");
_Static_assert(offsetof(ClickCand, cid) == 16, "ClickCand.cid");
_Static_assert(sizeof(ClickEv) == 16, "ClickEv");
_Static_assert(sizeof(SessRec) == 24, "SessRec");
_Static_assert(sizeof(AggCell) == 24, "AggCell");
_Static_assert(offsetof(AggCell, revenue) == 16, "AggCell.revenue");
_Static_assert(offsetof(AttrCtx, base_ms) == 88, "AttrCtx.base_ms");
_Static_assert(offsetof(AttrCtx, cpasses) == 104, "AttrCtx.cpasses");
_Static_assert(offsetof(AttrCtx, hist) == 112, "AttrCtx.hist");

/* ---- assembly entry points (System V AMD64) ---- */
uint64_t evx_hash(const void *p, size_t len);
Key16 evx_key16(const void *p, size_t len);        /* returned in rax:rdx */
uint64_t evx_khash(uint64_t k0, uint64_t k1);       /* table index hash of a Key16 */
int evx_keq(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1);
int evx_memeq(const void *a, const void *b, size_t len);
/* 1 = valid line with exactly nfields fields (out[] filled), 0 = malformed */
int evx_split_line(const char *p, size_t len, uint32_t nfields, Slice *out);
/* 1 = valid F1/F2/F3 timestamp (*out = epoch ms), 0 = bad */
int evx_parse_ts(const char *p, size_t len, int64_t *out);

/* Parse the event rows in [begin, end) (no header). Returns Cand count. */
uint64_t evx_parse_events(const ParseCtx *ctx, const char *begin,
                          const char *end, Cand *out);
/* table has `size` slots (any size < 2^32; index = (eid_h32 * size) >> 32) */
const LookupEntry *evx_lookup(const LookupEntry *slots, uint64_t mask, uint64_t k0, uint64_t k1);
void evx_dedup_insert(uint64_t *table, uint64_t size, Cand *c, uint64_t n);
/* Returns the Win count, or UINT64_MAX when the person table passed
   claim_limit (the caller enlarges it and reruns the whole phase). */
uint64_t evx_collect(const CollectCtx *ctx, const Cand *begin, const Cand *end,
                     Win *out, uint64_t *live);
void evx_scatter(const Win *w, uint64_t n, const uint32_t *remap,
                 uint64_t *cursor, Ev *dst);
uint64_t evx_sessionize(SessCtx *ctx, uint64_t pbegin, uint64_t pend, Row *out);


/* byte-wise order: <0, 0, >0 (unsigned bytes, shorter prefix first) */
int evx_memcmp(const void *a, size_t alen, const void *b, size_t blen);
/* sort 16-byte records by their first u64; hist = 8*256 u32 scratch */
void evx_sort16(void *a, uint64_t n, void *scratch, uint32_t passes, uint32_t *hist);
/* Parse click rows in [begin, end); returns ClickCand count. */
uint64_t evx_parse_clicks(const ClickParseCtx *ctx, const char *begin,
                          const char *end, ClickCand *out);
void evx_scatter_clicks(const ClickCand *c, uint64_t n, uint64_t *cursor,
                        ClickEv *dst, const ClickCand *base);
/* Attribute dense persons [pbegin, pend); returns non-organic credits. */
uint64_t evx_attribute(AttrCtx *ctx, uint64_t pbegin, uint64_t pend);

#endif
