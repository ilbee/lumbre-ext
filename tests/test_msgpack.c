/**
 * test_msgpack.c — CMocka unit tests for lumbre_msgpack
 *
 * Tests the hand-rolled msgpack packer against known byte sequences.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>
#include <stdlib.h>

#include "lumbre_msgpack.h"

/* --------------- Helpers --------------- */

#define BUF_SIZE 16384

typedef struct {
    uint8_t            raw[BUF_SIZE];
    lumbre_msgpack_buf buf;
} test_ctx_t;

static int setup(void **state)
{
    test_ctx_t *ctx = calloc(1, sizeof(test_ctx_t));
    ctx->buf.buf      = ctx->raw;
    ctx->buf.capacity = BUF_SIZE;
    ctx->buf.pos      = 0;
    *state = ctx;
    return 0;
}

static int teardown(void **state)
{
    free(*state);
    return 0;
}

/* Reset buffer for a sub-test within a test function */
static void reset(test_ctx_t *ctx)
{
    lumbre_msgpack_reset(&ctx->buf);
    memset(ctx->raw, 0, BUF_SIZE);
}

/* --------------- Test 1: pack_uint boundary values --------------- */

static void test_pack_uint_boundaries(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    /* 0 → positive fixint 0x00 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 0));
    assert_int_equal(1, b->pos);
    assert_int_equal(0x00, ctx->raw[0]);

    /* 127 → positive fixint 0x7f */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 127));
    assert_int_equal(1, b->pos);
    assert_int_equal(0x7f, ctx->raw[0]);

    /* 128 → uint8: 0xcc 0x80 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 128));
    assert_int_equal(2, b->pos);
    assert_int_equal(0xcc, ctx->raw[0]);
    assert_int_equal(0x80, ctx->raw[1]);

    /* 255 → uint8: 0xcc 0xff */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 255));
    assert_int_equal(2, b->pos);
    assert_int_equal(0xcc, ctx->raw[0]);
    assert_int_equal(0xff, ctx->raw[1]);

    /* 256 → uint16: 0xcd 0x01 0x00 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 256));
    assert_int_equal(3, b->pos);
    assert_int_equal(0xcd, ctx->raw[0]);
    assert_int_equal(0x01, ctx->raw[1]);
    assert_int_equal(0x00, ctx->raw[2]);

    /* 65535 → uint16: 0xcd 0xff 0xff */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 65535));
    assert_int_equal(3, b->pos);
    assert_int_equal(0xcd, ctx->raw[0]);
    assert_int_equal(0xff, ctx->raw[1]);
    assert_int_equal(0xff, ctx->raw[2]);

    /* 65536 → uint32: 0xce 0x00 0x01 0x00 0x00 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 65536));
    assert_int_equal(5, b->pos);
    assert_int_equal(0xce, ctx->raw[0]);
    assert_int_equal(0x00, ctx->raw[1]);
    assert_int_equal(0x01, ctx->raw[2]);
    assert_int_equal(0x00, ctx->raw[3]);
    assert_int_equal(0x00, ctx->raw[4]);

    /* 2^32 = 4294967296 → uint64: 0xcf + 8 bytes BE */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, (uint64_t)4294967296ULL));
    assert_int_equal(9, b->pos);
    assert_int_equal(0xcf, ctx->raw[0]);
    /* 4294967296 = 0x00 00 00 01 00 00 00 00 in big-endian */
    assert_int_equal(0x00, ctx->raw[1]);
    assert_int_equal(0x00, ctx->raw[2]);
    assert_int_equal(0x00, ctx->raw[3]);
    assert_int_equal(0x01, ctx->raw[4]);
    assert_int_equal(0x00, ctx->raw[5]);
    assert_int_equal(0x00, ctx->raw[6]);
    assert_int_equal(0x00, ctx->raw[7]);
    assert_int_equal(0x00, ctx->raw[8]);
}

/* --------------- Test 2: pack_str fixstr vs str16 --------------- */

static void test_pack_str_sizes(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    /* Empty string → 0xa0 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_str(b, "", 0));
    assert_int_equal(1, b->pos);
    assert_int_equal(0xa0, ctx->raw[0]);

    /* 5 bytes → fixstr 0xa5 + 5 bytes data */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_str(b, "hello", 5));
    assert_int_equal(6, b->pos);
    assert_int_equal(0xa5, ctx->raw[0]);
    assert_memory_equal("hello", &ctx->raw[1], 5);

    /* 31 bytes → fixstr 0xbf (max fixstr) */
    reset(ctx);
    char str31[32];
    memset(str31, 'X', 31);
    str31[31] = '\0';
    assert_int_equal(0, lumbre_msgpack_pack_str(b, str31, 31));
    assert_int_equal(32, b->pos);
    assert_int_equal(0xbf, ctx->raw[0]);
    assert_memory_equal(str31, &ctx->raw[1], 31);

    /* 32 bytes → str16: 0xda 0x00 0x20 + 32 bytes */
    reset(ctx);
    char str32[33];
    memset(str32, 'Y', 32);
    str32[32] = '\0';
    assert_int_equal(0, lumbre_msgpack_pack_str(b, str32, 32));
    assert_int_equal(35, b->pos);
    assert_int_equal(0xda, ctx->raw[0]);
    assert_int_equal(0x00, ctx->raw[1]);
    assert_int_equal(0x20, ctx->raw[2]);
    assert_memory_equal(str32, &ctx->raw[3], 32);

    /* 2048 bytes → str16 */
    reset(ctx);
    char *str2048 = malloc(2048);
    memset(str2048, 'Z', 2048);
    assert_int_equal(0, lumbre_msgpack_pack_str(b, str2048, 2048));
    assert_int_equal(3 + 2048, b->pos);
    assert_int_equal(0xda, ctx->raw[0]);
    /* 2048 = 0x0800 BE */
    assert_int_equal(0x08, ctx->raw[1]);
    assert_int_equal(0x00, ctx->raw[2]);
    assert_memory_equal(str2048, &ctx->raw[3], 2048);
    free(str2048);
}

/* --------------- Test 3: pack_bin (trace_id) --------------- */

static void test_pack_bin_trace_id(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    uint8_t trace_id[16] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
    };

    assert_int_equal(0, lumbre_msgpack_pack_bin(b, trace_id, 16));
    assert_int_equal(18, b->pos); /* 0xc4 + 0x10 + 16 bytes */
    assert_int_equal(0xc4, ctx->raw[0]);
    assert_int_equal(0x10, ctx->raw[1]);
    assert_memory_equal(trace_id, &ctx->raw[2], 16);
}

/* --------------- Test 4: pack_map fixmap --------------- */

static void test_pack_map_fixmap(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    /* 8 entries → 0x88 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 8));
    assert_int_equal(1, b->pos);
    assert_int_equal(0x88, ctx->raw[0]);

    /* 10 entries → 0x8a */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 10));
    assert_int_equal(1, b->pos);
    assert_int_equal(0x8a, ctx->raw[0]);

    /* 15 entries → 0x8f (max fixmap) */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 15));
    assert_int_equal(1, b->pos);
    assert_int_equal(0x8f, ctx->raw[0]);

    /* 16 entries → map16: 0xde 0x00 0x10 */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 16));
    assert_int_equal(3, b->pos);
    assert_int_equal(0xde, ctx->raw[0]);
    assert_int_equal(0x00, ctx->raw[1]);
    assert_int_equal(0x10, ctx->raw[2]);
}

/* --------------- Test 5: Roundtrip HTTP out span --------------- */

static void test_roundtrip_http_out_span(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    uint8_t trace_id[16];
    memset(trace_id, 0xAA, 16);

    /* Encode: map(9) with all HTTP out span fields */
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 9));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_TYPE, 1, 1));           /* t=1 (http_out) */
    assert_int_equal(0, lumbre_msgpack_pack_bin_kv(b, LUMBRE_MKEY_TRACE_ID, 3, trace_id, 16)); /* tid */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_SPAN_ID, 3, 12345));    /* sid */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_PARENT_ID, 3, 0));      /* pid */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_START_NS, 1, 1000000000ULL)); /* s */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_DURATION, 1, 5000000ULL));    /* d */
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_URL, 3, "https://api.example.com/v1/users", 32)); /* url */
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_METHOD, 3, "GET", 3));    /* mth */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_STATUS, 2, 200));        /* st */

    size_t total_size = b->pos;
    assert_true(total_size > 0);

    /* Verify by decoding byte by byte */
    size_t p = 0;

    /* map(9) */
    assert_int_equal(0x89, ctx->raw[p++]);

    /* Key "t" (fixstr len=1) */
    assert_int_equal(0xa1, ctx->raw[p++]);
    assert_int_equal('t', ctx->raw[p++]);
    /* Value 1 (positive fixint) */
    assert_int_equal(0x01, ctx->raw[p++]);

    /* Key "tid" (fixstr len=3) */
    assert_int_equal(0xa3, ctx->raw[p++]);
    assert_memory_equal("tid", &ctx->raw[p], 3); p += 3;
    /* Value bin8(16) */
    assert_int_equal(0xc4, ctx->raw[p++]);
    assert_int_equal(0x10, ctx->raw[p++]);
    assert_memory_equal(trace_id, &ctx->raw[p], 16); p += 16;

    /* Key "sid" */
    assert_int_equal(0xa3, ctx->raw[p++]);
    assert_memory_equal("sid", &ctx->raw[p], 3); p += 3;
    /* Value 12345 → uint16: 0xcd 0x30 0x39 */
    assert_int_equal(0xcd, ctx->raw[p++]);
    assert_int_equal(0x30, ctx->raw[p++]);
    assert_int_equal(0x39, ctx->raw[p++]);

    /* Key "pid" */
    assert_int_equal(0xa3, ctx->raw[p++]);
    assert_memory_equal("pid", &ctx->raw[p], 3); p += 3;
    /* Value 0 → fixint 0x00 */
    assert_int_equal(0x00, ctx->raw[p++]);

    /* Key "s" */
    assert_int_equal(0xa1, ctx->raw[p++]);
    assert_int_equal('s', ctx->raw[p++]);
    /* Value 1000000000 → uint32: 0xce + 4 bytes */
    assert_int_equal(0xce, ctx->raw[p++]);
    /* 1000000000 = 0x3B9ACA00 */
    assert_int_equal(0x3B, ctx->raw[p++]);
    assert_int_equal(0x9A, ctx->raw[p++]);
    assert_int_equal(0xCA, ctx->raw[p++]);
    assert_int_equal(0x00, ctx->raw[p++]);

    /* Key "d" */
    assert_int_equal(0xa1, ctx->raw[p++]);
    assert_int_equal('d', ctx->raw[p++]);
    /* Value 5000000 → uint32: 0xce + 4 bytes */
    assert_int_equal(0xce, ctx->raw[p++]);
    /* 5000000 = 0x004C4B40 */
    assert_int_equal(0x00, ctx->raw[p++]);
    assert_int_equal(0x4C, ctx->raw[p++]);
    assert_int_equal(0x4B, ctx->raw[p++]);
    assert_int_equal(0x40, ctx->raw[p++]);

    /* Key "url" */
    assert_int_equal(0xa3, ctx->raw[p++]);
    assert_memory_equal("url", &ctx->raw[p], 3); p += 3;
    /* Value str16(32): 0xda 0x00 0x20 */
    assert_int_equal(0xda, ctx->raw[p++]);
    assert_int_equal(0x00, ctx->raw[p++]);
    assert_int_equal(0x20, ctx->raw[p++]);
    assert_memory_equal("https://api.example.com/v1/users", &ctx->raw[p], 32); p += 32;

    /* Key "mth" */
    assert_int_equal(0xa3, ctx->raw[p++]);
    assert_memory_equal("mth", &ctx->raw[p], 3); p += 3;
    /* Value "GET" fixstr(3): 0xa3 */
    assert_int_equal(0xa3, ctx->raw[p++]);
    assert_memory_equal("GET", &ctx->raw[p], 3); p += 3;

    /* Key "st" */
    assert_int_equal(0xa2, ctx->raw[p++]);
    assert_memory_equal("st", &ctx->raw[p], 2); p += 2;
    /* Value 200 → uint8: 0xcc 0xc8 */
    assert_int_equal(0xcc, ctx->raw[p++]);
    assert_int_equal(0xc8, ctx->raw[p++]);

    /* All bytes consumed */
    assert_int_equal(total_size, p);
}

/* --------------- Test 6: Roundtrip DB span --------------- */

static void test_roundtrip_db_span(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    uint8_t trace_id[16];
    memset(trace_id, 0xBB, 16);

    /* Build a 500-char SQL query */
    char query[501];
    memset(query, 'S', 500);
    query[500] = '\0';

    assert_int_equal(0, lumbre_msgpack_pack_map(b, 9));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_TYPE, 1, 2));              /* t=2 (db) */
    assert_int_equal(0, lumbre_msgpack_pack_bin_kv(b, LUMBRE_MKEY_TRACE_ID, 3, trace_id, 16));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_SPAN_ID, 3, 99999));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_PARENT_ID, 3, 10));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_START_NS, 1, 2000000000ULL));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_DURATION, 1, 1500000ULL));
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_QUERY, 1, query, 500));       /* q */
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_DB_TYPE, 3, "mysql", 5));     /* dbt */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_ROWS, 1, 42));               /* r */

    size_t total_size = b->pos;
    assert_true(total_size > 0);

    /* Verify map header */
    assert_int_equal(0x89, ctx->raw[0]);

    /* Find the query string in the output and verify it is full 500 chars.
     * The query key "q" is fixstr(1) = 0xa1 'q', then str16(500) = 0xda 0x01 0xf4 + 500 bytes. */
    size_t p = 0;
    int found_query = 0;
    while (p < total_size - 1) {
        if (ctx->raw[p] == 0xa1 && ctx->raw[p + 1] == 'q') {
            p += 2;
            /* str16 header */
            assert_int_equal(0xda, ctx->raw[p++]);
            assert_int_equal(0x01, ctx->raw[p++]); /* 500 = 0x01F4 BE */
            assert_int_equal(0xf4, ctx->raw[p++]);
            /* Full query preserved */
            assert_memory_equal(query, &ctx->raw[p], 500);
            found_query = 1;
            break;
        }
        p++;
    }
    assert_true(found_query);
}

/* --------------- Test 7: Roundtrip func span --------------- */

static void test_roundtrip_func_span(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    uint8_t trace_id[16];
    memset(trace_id, 0xCC, 16);

    assert_int_equal(0, lumbre_msgpack_pack_map(b, 10));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_TYPE, 1, 5));              /* t=5 (func) */
    assert_int_equal(0, lumbre_msgpack_pack_bin_kv(b, LUMBRE_MKEY_TRACE_ID, 3, trace_id, 16));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_SPAN_ID, 3, 777));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_PARENT_ID, 3, 100));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_START_NS, 1, 3000000000ULL));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_DURATION, 1, 800000ULL));
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_FUNC_CLASS, 3, "App\\Service\\Foo", 15)); /* cls */
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_FUNC_NAME, 2, "bar", 3));                 /* fn */
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_FUNC_FILE, 1, "/var/www/app.php", 16));   /* f */
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_FUNC_LINE, 1, 42));                      /* l */

    size_t total_size = b->pos;

    /* Verify map header: map(10) = 0x8a */
    assert_int_equal(0x8a, ctx->raw[0]);

    /* Verify class field by scanning for key "cls" */
    size_t p = 0;
    int found_cls = 0;
    while (p < total_size - 3) {
        if (ctx->raw[p] == 0xa3 &&
            ctx->raw[p + 1] == 'c' && ctx->raw[p + 2] == 'l' && ctx->raw[p + 3] == 's') {
            p += 4;
            /* "App\Service\Foo" is 15 bytes → fixstr(15) = 0xaf */
            assert_int_equal(0xaf, ctx->raw[p++]);
            assert_memory_equal("App\\Service\\Foo", &ctx->raw[p], 15);
            found_cls = 1;
            break;
        }
        p++;
    }
    assert_true(found_cls);

    /* Verify line field by scanning for key "l" */
    p = 0;
    int found_line = 0;
    while (p < total_size - 1) {
        /* Key "l" = fixstr(1) 0xa1 'l' */
        if (ctx->raw[p] == 0xa1 && ctx->raw[p + 1] == 'l') {
            p += 2;
            /* Value 42 → positive fixint 0x2a */
            assert_int_equal(0x2a, ctx->raw[p]);
            found_line = 1;
            break;
        }
        p++;
    }
    assert_true(found_line);
}

/* --------------- Test 8: Buffer overflow --------------- */

static void test_buffer_overflow(void **state)
{
    (void)state;

    uint8_t raw[64];
    lumbre_msgpack_buf b = {.buf = raw, .capacity = 64, .pos = 0};

    /* Try to encode a 100-byte string into a 64-byte buffer → -1 */
    char big[100];
    memset(big, 'A', 100);
    assert_int_equal(-1, lumbre_msgpack_pack_str(&b, big, 100));

    /* pos must not have changed */
    assert_int_equal(0, b.pos);

    /* Short string should still work (buffer is reusable) */
    assert_int_equal(0, lumbre_msgpack_pack_str(&b, "ok", 2));
    assert_int_equal(3, b.pos); /* fixstr(2) = 1 + 2 */
    assert_int_equal(0xa2, raw[0]);
    assert_memory_equal("ok", &raw[1], 2);
}

/* --------------- Test 9: KV shortcuts --------------- */

static void test_kv_shortcuts(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    /* Encode using pack_str_kv */
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, "url", 3, "http://example.com", 18));
    size_t kv_size = b->pos;
    uint8_t kv_bytes[256];
    memcpy(kv_bytes, ctx->raw, kv_size);

    /* Encode the same using separate pack_str calls */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_str(b, "url", 3));
    assert_int_equal(0, lumbre_msgpack_pack_str(b, "http://example.com", 18));
    size_t separate_size = b->pos;

    /* Must be identical */
    assert_int_equal(kv_size, separate_size);
    assert_memory_equal(kv_bytes, ctx->raw, kv_size);

    /* Same for pack_uint_kv */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, "st", 2, 200));
    size_t uint_kv_size = b->pos;
    uint8_t uint_kv_bytes[256];
    memcpy(uint_kv_bytes, ctx->raw, uint_kv_size);

    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_str(b, "st", 2));
    assert_int_equal(0, lumbre_msgpack_pack_uint(b, 200));
    size_t uint_sep_size = b->pos;

    assert_int_equal(uint_kv_size, uint_sep_size);
    assert_memory_equal(uint_kv_bytes, ctx->raw, uint_kv_size);
}

/* --------------- Test 10: Predictable encoding size --------------- */

static void test_predictable_encoding_size(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_msgpack_buf *b = &ctx->buf;

    /*
     * Encode a simple span and verify total size matches hand-calculated value.
     *
     * map(3)                         = 1 byte   (0x83)
     * key "t" (fixstr 1)             = 2 bytes  (0xa1 + 't')
     * val 1 (fixint)                 = 1 byte   (0x01)
     * key "sid" (fixstr 3)           = 4 bytes  (0xa3 + 'sid')
     * val 42 (fixint)                = 1 byte   (0x2a)
     * key "d" (fixstr 1)             = 2 bytes  (0xa1 + 'd')
     * val 5000000 (uint32)           = 5 bytes  (0xce + 4 bytes)
     * ---
     * Total: 1 + 2 + 1 + 4 + 1 + 2 + 5 = 16 bytes
     */
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 3));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_TYPE, 1, 1));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_SPAN_ID, 3, 42));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_DURATION, 1, 5000000));

    assert_int_equal(16, b->pos);

    /*
     * Another span with a string value:
     *
     * map(2)                         = 1 byte   (0x82)
     * key "mth" (fixstr 3)           = 4 bytes  (0xa3 + 'mth')
     * val "POST" (fixstr 4)          = 5 bytes  (0xa4 + 'POST')
     * key "st" (fixstr 2)            = 3 bytes  (0xa2 + 'st')
     * val 404 (uint16)               = 3 bytes  (0xcd + 2 bytes)
     * ---
     * Total: 1 + 4 + 5 + 3 + 3 = 16 bytes
     */
    reset(ctx);
    assert_int_equal(0, lumbre_msgpack_pack_map(b, 2));
    assert_int_equal(0, lumbre_msgpack_pack_str_kv(b, LUMBRE_MKEY_METHOD, 3, "POST", 4));
    assert_int_equal(0, lumbre_msgpack_pack_uint_kv(b, LUMBRE_MKEY_STATUS, 2, 404));

    assert_int_equal(16, b->pos);
}

/* --------------- Main --------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_pack_uint_boundaries, setup, teardown),
        cmocka_unit_test_setup_teardown(test_pack_str_sizes, setup, teardown),
        cmocka_unit_test_setup_teardown(test_pack_bin_trace_id, setup, teardown),
        cmocka_unit_test_setup_teardown(test_pack_map_fixmap, setup, teardown),
        cmocka_unit_test_setup_teardown(test_roundtrip_http_out_span, setup, teardown),
        cmocka_unit_test_setup_teardown(test_roundtrip_db_span, setup, teardown),
        cmocka_unit_test_setup_teardown(test_roundtrip_func_span, setup, teardown),
        cmocka_unit_test_setup_teardown(test_buffer_overflow, setup, teardown),
        cmocka_unit_test_setup_teardown(test_kv_shortcuts, setup, teardown),
        cmocka_unit_test_setup_teardown(test_predictable_encoding_size, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
