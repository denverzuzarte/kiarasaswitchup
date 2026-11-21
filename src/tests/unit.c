/* unit.c - tests of the asm primitives (make unit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../evx.h"

static int fails, total;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        total++;                                          \
        if (!(cond)) {                                    \
            fails++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* Put s at the very end of a page so any over-read would fault. */
static const char *at_page_end(const char *s)
{
    static char *page;
    static size_t used;
    size_t n = strlen(s);
    if (!page)
        page = aligned_alloc(4096, 4096 * 2);
    (void)used;
    char *dst = page + 4096 - n;
    memcpy(dst, s, n);
    return dst;
}

static void ts_ok(const char *s, long long want)
{
    int64_t v = 0x5555;
    const char *p = at_page_end(s);
    int ok = evx_parse_ts(p, strlen(s), &v);
    CHECK(ok == 1 && v == want, "ts '%s' -> ok=%d v=%lld want %lld", s, ok, (long long)v, want);
}

static void ts_bad(const char *s)
{
    int64_t v = 0;
    int ok = evx_parse_ts(s, strlen(s), &v);
    CHECK(ok == 0, "ts '%s' should be bad (got %lld)", s, (long long)v);
}

static void split(const char *line, unsigned nf, int want_ok, const char *const *want)
{
    Slice f[8];
    memset(f, 0, sizeof f);
    int ok = evx_split_line(line, strlen(line), nf, f);
    CHECK(ok == want_ok, "split '%s' nf=%u -> %d want %d", line, nf, ok, want_ok);
    if (ok && want)
        for (unsigned i = 0; i < nf; i++)
            CHECK(f[i].len == strlen(want[i]) && memcmp(f[i].ptr, want[i], f[i].len) == 0,
                  "split '%s' field %u = '%.*s' want '%s'", line, i, (int)f[i].len, f[i].ptr, want[i]);
}

int main(void)
{
    /* ---- timestamps: F1 / F2 / F3 */
    ts_ok("2026-10-03T23:58:10Z", 1791071890000LL);
    ts_ok("2026-10-03T19:58:10-04:00", 1791071890000LL);
    ts_ok("1791071890000", 1791071890000LL);
    ts_ok("0001791071890000", 1791071890000LL);
    ts_ok("000000000000000000000000001791071890000", 1791071890000LL);
    ts_ok("2026-10-04T01:30:00+02:00", 1791071890000LL - (28 * 60 + 10) * 1000LL);
    ts_ok("2028-02-29T00:00:00Z", 1835395200000LL);
    ts_ok("2000-02-29T12:34:56Z", 951827696000LL);
    ts_ok("1970-01-01T00:00:00Z", 0);
    ts_ok("1969-12-31T23:59:59Z", -1000);
    ts_ok("9999-12-31T23:59:59Z", 253402300799000LL);
    ts_ok("2026-10-01T00:00:00Z", 1790812800000LL);
    ts_ok("2026-10-01T00:00:00+23:59", 1790812800000LL - (23 * 60 + 59) * 60000LL);
    ts_ok("2026-10-01T00:00:00-00:00", 1790812800000LL);
    ts_ok("86400000", 86400000);
    ts_ok("9223372036854775807", 9223372036854775807LL);
    ts_bad("9223372036854775808");
    ts_bad("179102160000000000000");
    ts_bad("");
    ts_bad("2026-10-03 10:00:00");
    ts_bad("2026-10-03T10:00:00z");
    ts_bad("2026-10-03t10:00:00Z");
    ts_bad("2026-10-03T10:00:00");
    ts_bad("2026-10-03T10:00:00.000Z");
    ts_bad("1791071890000.0");
    ts_bad("1.79e12");
    ts_bad("-5");
    ts_bad("2026-13-03T10:00:00Z");
    ts_bad("2026-00-03T10:00:00Z");
    ts_bad("2026-02-30T10:00:00Z");
    ts_bad("2026-02-29T10:00:00Z");
    ts_bad("2100-02-29T10:00:00Z");
    ts_bad("2026-04-31T10:00:00Z");
    ts_bad("2026-10-00T10:00:00Z");
    ts_bad("2026-10-03T24:00:00Z");
    ts_bad("2026-10-03T10:60:00Z");
    ts_bad("2026-10-03T10:00:60Z");
    ts_bad("2026-10-03T10:00:00+24:00");
    ts_bad("2026-10-03T10:00:00+05:60");
    ts_bad("2026-10-03T10:00:00+0200");
    ts_bad("2026-10-03T10:00:00+02:0a");
    ts_bad("\xef\xbc\x92" "026-10-03T10:00:00Z");      /* full-width 2 */
    ts_bad("\xd9\xa2\xd9\xa5\xd9\xa0");                /* Arabic-Indic digits */
    ts_bad("2026-10-03T10:00:00Z\r");
    ts_bad("2026/10/03T10:00:00Z");
    ts_bad("2026-10-03X10:00:00Z");

    /* E7: +02:00 converts to Oct 3 23:30 UTC */
    {
        int64_t a, b;
        evx_parse_ts("2026-10-04T01:30:00+02:00", 25, &a);
        evx_parse_ts("2026-10-03T23:30:00Z", 20, &b);
        CHECK(a == b, "E7 offset conversion %lld vs %lld", (long long)a, (long long)b);
    }

    /* ---- CSV dialect */
    const char *w7[] = {"e1", "u1", "ts", "it", "view", "p1", ""};
    split("e1,u1,ts,it,view,p1,", 7, 1, w7);
    split("\"e1\",\"u1\",ts,it,view,p1,\"\"", 7, 1, w7);
    split("e1,u1,ts,it,view,p1", 7, 0, NULL);
    split("e1,u1,ts,it,view,p1,,", 7, 0, NULL);
    split("", 7, 0, NULL);
    split("", 1, 0, NULL);
    const char *w1[] = {"\r"};
    split("\r", 1, 1, w1);
    const char *wq[] = {" u1", "u2 "};
    split("\" u1\",u2 ", 2, 1, wq);
    split("u1,u5\"", 2, 0, NULL);
    split("u\"1,u5", 2, 0, NULL);
    split("\"u1,u5\"", 2, 0, NULL);
    split("\",u5", 2, 0, NULL);
    split("\"\"\",u5", 2, 0, NULL);
    split("\"a\"b\",u5", 2, 0, NULL);
    const char *we[] = {"", ""};
    split("\"\",\"\"", 2, 1, we);
    split(",", 2, 1, we);
    {
        /* a field longer than one 64-byte block */
        char line[400];
        memset(line, 'x', 150);
        strcpy(line + 150, ",\"");
        memset(line + 152, 'y', 100);
        strcpy(line + 252, "\",z");
        Slice f[3];
        int ok = evx_split_line(line, strlen(line), 3, f);
        CHECK(ok && f[0].len == 150 && f[1].len == 100 && f[1].ptr[0] == 'y' && f[2].len == 1,
              "long split ok=%d", ok);
        line[200] = '"';
        CHECK(!evx_split_line(line, strlen(line), 3, f), "stray quote in long field");
    }

    /* ---- hash / memeq */
    CHECK(evx_hash("abc", 3) == evx_hash(at_page_end("abc"), 3), "hash position independent");
    CHECK(evx_hash("abc", 3) != evx_hash("abd", 3), "hash differs");
    CHECK(evx_hash("u1", 2) != evx_hash("u1 ", 3), "hash len");
    CHECK(evx_memeq("hello world", "hello world", 11) == 1, "memeq eq");
    CHECK(evx_memeq("hello world", "hello worle", 11) == 0, "memeq ne");
    CHECK(evx_memeq("a", "b", 0) == 1, "memeq empty");
    {
        char a[300], b[300];
        memset(a, 'q', 300);
        memset(b, 'q', 300);
        CHECK(evx_memeq(a, b, 300) == 1, "memeq long eq");
        b[299] = 'r';
        CHECK(evx_memeq(a, b, 300) == 0, "memeq long ne tail");
        b[299] = 'q';
        b[70] = 'r';
        CHECK(evx_memeq(a, b, 300) == 0, "memeq long ne mid");
        CHECK(evx_memeq(a, b, 64) == 1, "memeq exactly 64");
    }

    /* ---- Key16: inline keys exact, long keys by pointer + hash + bytes */
    {
        Key16 a = evx_key16("u1", 2), b = evx_key16(at_page_end("u1"), 2);
        CHECK(a.k0 == b.k0 && a.k1 == b.k1, "key16 position independent");
        CHECK((a.k1 >> 56) == 2 && memcmp(&a, "u1", 2) == 0, "key16 holds bytes + len");
        Key16 c = evx_key16("u1 ", 3), d = evx_key16("u1\0", 3);
        CHECK(!evx_keq(a.k0, a.k1, c.k0, c.k1), "key16 trailing space differs");
        CHECK(!evx_keq(c.k0, c.k1, d.k0, d.k1), "key16 NUL vs space differs");
        Key16 e = evx_key16("abcdefghijklmno", 15), f = evx_key16("abcdefghijklmnp", 15);
        CHECK(!KEY_LONG(e.k1) && !evx_keq(e.k0, e.k1, f.k0, f.k1), "key16 15 bytes inline, last byte counts");
        char l1[100], l2[100];
        memset(l1, 'L', 100);
        memset(l2, 'L', 100);
        Key16 g = evx_key16(l1, 100), h = evx_key16(l2, 100);
        CHECK(KEY_LONG(g.k1) && g.k0 != h.k0, "key16 long keeps pointer");
        CHECK(evx_keq(g.k0, g.k1, h.k0, h.k1), "key16 long equal by bytes");
        CHECK(evx_khash(g.k0, g.k1) == evx_khash(h.k0, h.k1), "khash long ignores pointer");
        l2[99] = 'M';
        h = evx_key16(l2, 100);
        CHECK(!evx_keq(g.k0, g.k1, h.k0, h.k1), "key16 long differs");
        Key16 i16 = evx_key16("abcdefghijklmnop", 16);
        CHECK(KEY_LONG(i16.k1), "key16 16 bytes is long");
        CHECK(evx_khash(a.k0, a.k1) != evx_khash(c.k0, c.k1), "khash differs");
    }

    /* ---- evx_memcmp: byte-wise order, shorter prefix first, unsigned bytes */
    {
#define SGN(x) ((x) > 0 ? 1 : (x) < 0 ? -1 : 0)
        struct { const char *a, *b; int want; } mc[] = {
            {"c10", "c9", -1}, {"C1", "c1", -1}, {" c", "c", -1}, {"c", "c1", -1},
            {"c1", "c", 1}, {"abc", "abc", 0}, {"", "", 0}, {"", "a", -1},
            {"\xc3\xa9", "z", 1}, {"\x7f", "\x80", -1}, {"k1", "k1 ", -1},
        };
        for (size_t i = 0; i < sizeof mc / sizeof mc[0]; i++) {
            int r = evx_memcmp(mc[i].a, strlen(mc[i].a), mc[i].b, strlen(mc[i].b));
            CHECK(SGN(r) == mc[i].want, "memcmp '%s' '%s' -> %d want %d", mc[i].a, mc[i].b, r, mc[i].want);
        }
        char a[200], b[200];
        memset(a, 'x', 200);
        memset(b, 'x', 200);
        CHECK(evx_memcmp(a, 200, b, 200) == 0, "memcmp long eq");
        b[130] = 'y';
        CHECK(evx_memcmp(a, 200, b, 200) < 0, "memcmp long lt");
        CHECK(evx_memcmp(a, 150, b, 120) > 0, "memcmp long prefix");
    }

    printf("%d/%d checks passed\n", total - fails, total);
    return fails != 0;
}
