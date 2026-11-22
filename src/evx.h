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
    Key16 *raw;                   /* 16  raw-ID table (cmpxchg16b slots; slot = rid) */
    uint64_t raw_mask;            /* 24 */
    const LookupEntry *prod_slots;/* 32 (NULL when there are no products) */
    uint64_t prod_mask;           /* 40 */
    uint32_t unknown_cat;         /* 48 */
    uint32_t pad;                 /* 52 */
    char *arena;                  /* 56  4 KB chunks: [0] u32 count, [4] u32 next, Recs at 32 */
    uint64_t *raw_claimed;        /* 64  shared count of claimed raw slots (lock xadd) */
    uint32_t pshift;              /* 72  partition = (uint32)eid hash >> pshift */
    uint32_t pad2;                /* 76 */
    int64_t ws_ms;                /* 80  floor(window_start / 1000) * 1000 */
    uint64_t raw_limit;           /* 88  > this many raw claims: table too small */
} ParseCtx;                       /* 96 */

#define CHUNK_BYTES 4096
#define CHUNK_RECS 127            /* 32 + 127 * 32 == 4096 */
#define CHUNK_NONE 0xFFFFFFFFu
#define RID_NONE 0xFFFFFFFFu
#define KI_BIAS ((int64_t)1 << 39) /* ingest seconds relative to the window, biased */
#define KI_BAD 0xFFFFFFFFFFull     /* 40-bit ingest field of a bad ingest_ts */

/* A worker's open chunk of one dedup partition / person range. 32 bytes. */
typedef struct {
    void *cur;                    /*  0  next record slot */
    char *end;                    /*  8  chunk end = chunk + 4096 (0: no chunk yet) */
    uint32_t head;                /* 16  first chunk of this worker's list */
    uint32_t count;               /* 20  Win lists: Wins in this list */
    uint64_t pad2;
} PartHead;

/*
 * Per-worker 2 MB slab of the shared chunk arena: chunks are cut from it
 * without atomics; a refill takes the next slab (one atomic add) and
 * pre-faults it with MADV_POPULATE_WRITE. evx_chunk_alloc is C, called from
 * the asm (System V: it may clobber every caller-saved register, zmm too).
 */
typedef struct {
    char *cur;                    /*  0 */
    char *end;                    /*  8 */
    char *arena;                  /* 16 */
    uint64_t *next;               /* 24  next free slab index (atomic add) */
    uint64_t nslabs;              /* 32  arena capacity in slabs */
    int populate;                 /* 40 */
    int pad;
} Slab;                           /* 48 */
#define SLAB_BYTES ((size_t)2 << 20)
char *evx_chunk_alloc(Slab *s);

/*
 * One structurally valid event row with a non-empty event_id. 32 bytes.
 * Validation (incl. the late rule) is evaluated once at parse time; an
 * invalid copy has rid == RID_NONE but still competes in dedup.
 *   ki    = (ingest_s - ws_s + 2^39) << 24 | h & 0xFFFFFF  (h = evx_khash of
 *           the event_id Key16; a bad ingest has the 40-bit field all ones),
 *           so ki order is ingest order among copies of one event_id
 *   eptr  = event_id bytes (48-bit pointer into the input; the value ends at
 *           its first ',' or '"') | (h >> 24 & 0xFFFF) << 48
 */
typedef struct Rec {
    uint64_t ki;          /*  0 */
    uint64_t eptr;        /*  8 */
    int64_t ts_off;       /* 16  ts - window_start (valid copies only) */
    uint32_t rid;         /* 24  raw-ID slot of user_id, RID_NONE: invalid copy */
    uint32_t price_cat;   /* 28  price (24 bits, 0 unless purchase) | cat << 24 */
} Rec;

/* Person table slot: the canonical name as a Key16 (claimed with cmpxchg16b). */
typedef Key16 PersonSlot;

/* Context for evx_resolve_raw (P1.5): raw id -> canonical person slot. */
typedef struct {
    const Key16 *raw;             /*  0 */
    uint32_t *pslot;              /*  8  out: raw slot -> person slot */
    PersonSlot *persons;          /* 16 */
    uint64_t person_mask;         /* 24 */
    const LookupEntry *link_slots;/* 32  NULL when there are no links */
    uint64_t link_mask;           /* 40 */
    uint64_t *claimed;            /* 48  shared count of claimed person slots */
    uint64_t claim_limit;         /* 56 */
} ResolveCtx;                     /* 64 */

/* Per-worker context for evx_dedup_emit (P2). */
typedef struct {
    uint64_t *table;              /*  0  private, zeroed by the caller: tsize + 64 */
    uint64_t tsize;               /*  8 */
    char *arena;                  /* 16 */
    const PartHead *heads;        /* 24  parse heads [nw][P] */
    uint32_t nw;                  /* 32 */
    uint32_t P;                   /* 36 */
    uint32_t pbits;               /* 40 */
    uint32_t dshift;              /* 44  person range = dense pid >> dshift */
    const uint32_t *remap;        /* 48  raw slot -> dense person id */
    PartHead *wheads;             /* 56  this worker's open Win chunk per range */
    char *free_chunks;            /* 64  consumed Rec chunks (linked via [chunk]) */
    Slab *slab;                   /* 72 */
} DedupCtx;                       /* 80 */
#define WCHUNK_WINS 255           /* 16 + 255 * 16 == 4096 */

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
_Static_assert(sizeof(ParseCtx) == 96, "ParseCtx");
_Static_assert(offsetof(ParseCtx, arena) == 56, "ParseCtx.arena");
_Static_assert(offsetof(ParseCtx, raw_claimed) == 64, "ParseCtx.raw_claimed");
_Static_assert(offsetof(ParseCtx, pshift) == 72, "ParseCtx.pshift");
_Static_assert(offsetof(ParseCtx, ws_ms) == 80, "ParseCtx.ws_ms");
_Static_assert(offsetof(ParseCtx, raw_limit) == 88, "ParseCtx.raw_limit");
_Static_assert(offsetof(ParseCtx, unknown_cat) == 48, "ParseCtx.unknown_cat");
_Static_assert(sizeof(PartHead) == 32, "PartHead");
_Static_assert(offsetof(PartHead, head) == 16, "PartHead.head");
_Static_assert(sizeof(Slab) == 48, "Slab");
_Static_assert(32 + CHUNK_RECS * 32 == CHUNK_BYTES, "chunk layout");
_Static_assert(sizeof(Rec) == 32, "Rec");
_Static_assert(offsetof(Rec, ts_off) == 16, "Rec.ts_off");
_Static_assert(offsetof(Rec, rid) == 24, "Rec.rid");
_Static_assert(offsetof(Rec, price_cat) == 28, "Rec.price_cat");
_Static_assert(sizeof(PersonSlot) == 16, "PersonSlot");
_Static_assert(sizeof(ResolveCtx) == 64, "ResolveCtx");
_Static_assert(offsetof(ResolveCtx, claim_limit) == 56, "ResolveCtx.claim_limit");
_Static_assert(sizeof(DedupCtx) == 80, "DedupCtx");
_Static_assert(offsetof(DedupCtx, dshift) == 44, "DedupCtx.dshift");
_Static_assert(offsetof(DedupCtx, slab) == 72, "DedupCtx.slab");
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
} ClickParseCtx;                    /* 80 */

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

_Static_assert(sizeof(ClickParseCtx) == 80, "ClickParseCtx");
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

/* Parse the event rows in [begin, end) (no header); every Rec goes into its
   dedup partition's open chunk in heads[] (this worker's PartHead[P]); valid
   copies intern their raw user_id (rid). Returns the Rec count, or
   UINT64_MAX when the raw-ID table passed raw_limit (rerun P1, larger). */
uint64_t evx_parse_events(const ParseCtx *ctx, const char *begin,
                          const char *end, PartHead *heads, Slab *slab);
const LookupEntry *evx_lookup(const LookupEntry *slots, uint64_t mask, uint64_t k0, uint64_t k1);
/* P1.5: canonical person of every claimed raw slot in [s0, s1), interned in
   the person table; pslot[s] = its person slot. Returns the claims made, or
   UINT64_MAX when the person table passed claim_limit (rerun P1.5). */
uint64_t evx_resolve_raw(const ResolveCtx *ctx, uint64_t s0, uint64_t s1);
/* P2: dedup of one partition (all workers' chunk lists) in the private table,
   then every valid winner is emitted as a Win into the worker's chunk list of
   its person range; the partition's chunks go to the worker's free list.
   Returns the Win count. */
uint64_t evx_dedup_emit(DedupCtx *ctx, uint32_t part);
/* 1 when the event_id values at a and b (each ending at ',' or '"') are equal */
int evx_eid_eq(const char *a, const char *b);
/* Events per dense person of one worker's Win list of one range (cnt[k]++). */
void evx_count_range(const char *warena, uint32_t first_chunk, uint64_t *cnt);
/* Places one worker's Win chunk list of one person range into Ev order. */
void evx_group_range(const char *warena, uint32_t first_chunk, uint64_t *cursor, Ev *dst);
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
