/**
 * test_span.c — CMocka unit tests for lumbre_span (Phase 3).
 *
 * Tests span lifecycle: context init, span start/finish, parent chaining,
 * ring buffer encoding, min_duration filtering, and query truncation.
 *
 * Build: see Makefile.frag target test-unit-span.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#include <cmocka.h>

#include "lumbre_span.h"
#include "lumbre_ringbuf.h"
#include "lumbre_msgpack.h"

/* --------------------------------------------------------------------------
 * Mock random function — deterministic, fills with 0xAA
 * ----------------------------------------------------------------------- */

static int mock_random(void *buf, size_t len)
{
    memset(buf, 0xAA, len);
    return 0;
}

/* --------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/** Temporary directory for ring buffer shm files in tests. */
#define TEST_SHM_DIR "/tmp"
#define TEST_PID     99999
#define TEST_WORKER  0

static lumbre_ringbuf_t  test_rb;
static uint8_t           test_mp_raw[LUMBRE_MSGPACK_BUF_SIZE];
static lumbre_msgpack_buf test_mp;

/**
 * Init a ring buffer for a test. Must call cleanup_ringbuf() after.
 */
static int setup_ringbuf(uint32_t capacity)
{
    memset(&test_rb, 0, sizeof(test_rb));
    int rc = lumbre_ringbuf_init(&test_rb, TEST_SHM_DIR, TEST_PID,
                                 TEST_WORKER, capacity);
    assert_int_equal(rc, 0);

    test_mp.buf      = test_mp_raw;
    test_mp.capacity = sizeof(test_mp_raw);
    test_mp.pos      = 0;

    return 0;
}

static void cleanup_ringbuf(void)
{
    lumbre_ringbuf_destroy(&test_rb, 0);
}

/**
 * Read back the first message from the ring buffer.
 * Returns a pointer into rb->data at the msgpack payload start,
 * and sets *out_len to the payload length.
 */
static const uint8_t *rb_read_first_msg(lumbre_ringbuf_t *rb, uint32_t *out_len)
{
    if (rb->header->write_pos == 0) {
        return NULL;
    }
    /* First 4 bytes at offset 0 = payload length (little-endian native). */
    uint32_t len;
    memcpy(&len, &rb->data[0], 4);
    *out_len = len;
    return &rb->data[4];
}

/* --------------------------------------------------------------------------
 * Test 1: context init without propagation
 * ----------------------------------------------------------------------- */

static void test_context_init_no_propagation(void **state)
{
    (void)state;
    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    lumbre_context_init(&ctx, NULL, mock_random);

    assert_int_equal(ctx.active, 1);
    assert_int_equal(ctx.span_counter, 1);
    assert_int_equal(ctx.current_parent_id, 0);

    /* trace_id should be filled with 0xAA by mock_random */
    uint8_t expected[16];
    memset(expected, 0xAA, 16);
    assert_memory_equal(ctx.trace_id, expected, 16);

    /* Verify it is not all-zero */
    uint8_t zeros[16];
    memset(zeros, 0, 16);
    assert_memory_not_equal(ctx.trace_id, zeros, 16);
}

/* --------------------------------------------------------------------------
 * Test 2: context init with propagation
 * ----------------------------------------------------------------------- */

static void test_context_init_with_propagation(void **state)
{
    (void)state;
    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    uint8_t known_id[16];
    for (int i = 0; i < 16; i++) {
        known_id[i] = (uint8_t)(i + 1);
    }

    lumbre_context_init(&ctx, known_id, mock_random);

    assert_memory_equal(ctx.trace_id, known_id, 16);
    assert_int_equal(ctx.active, 1);
    assert_int_equal(ctx.span_counter, 1);
}

/* --------------------------------------------------------------------------
 * Test 3: span_start basic fields
 * ----------------------------------------------------------------------- */

static void test_span_start_basic(void **state)
{
    (void)state;
    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    lumbre_span_t span;
    lumbre_span_start(&span, LUMBRE_SPAN_DB, &ctx);

    assert_int_equal(span.type, 3);  /* LUMBRE_SPAN_DB */
    assert_int_equal(span.span_id, 1);
    assert_int_equal(span.parent_id, 0);
    assert_true(span.start_ns > 0);

    /* Context should now point to this span as parent */
    assert_int_equal(ctx.current_parent_id, 1);
    assert_int_equal(ctx.span_counter, 2);

    /* trace_id should be copied from context */
    assert_memory_equal(span.trace_id, ctx.trace_id, 16);
}

/* --------------------------------------------------------------------------
 * Test 4: parent/child chaining
 * ----------------------------------------------------------------------- */

static void test_span_parent_child_chaining(void **state)
{
    (void)state;
    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    lumbre_span_t span_a, span_b, span_c;

    lumbre_span_start(&span_a, LUMBRE_SPAN_HTTP_OUT, &ctx);
    assert_int_equal(span_a.span_id, 1);
    assert_int_equal(span_a.parent_id, 0);

    lumbre_span_start(&span_b, LUMBRE_SPAN_DB, &ctx);
    assert_int_equal(span_b.span_id, 2);
    assert_int_equal(span_b.parent_id, 1);

    lumbre_span_start(&span_c, LUMBRE_SPAN_REDIS, &ctx);
    assert_int_equal(span_c.span_id, 3);
    assert_int_equal(span_c.parent_id, 2);

    /* Current parent is the most recent span */
    assert_int_equal(ctx.current_parent_id, 3);
    assert_int_equal(ctx.span_counter, 4);
}

/* --------------------------------------------------------------------------
 * Test 5: span_finish unwinds parent
 * ----------------------------------------------------------------------- */

static void test_span_finish_unwinds_parent(void **state)
{
    (void)state;

    setup_ringbuf(4096);

    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    lumbre_span_t span_a, span_b, span_c;

    lumbre_span_start(&span_a, LUMBRE_SPAN_HTTP_OUT, &ctx);
    lumbre_span_start(&span_b, LUMBRE_SPAN_DB, &ctx);
    lumbre_span_start(&span_c, LUMBRE_SPAN_REDIS, &ctx);

    /* Set minimal payloads so encoding does not fail */
    lumbre_span_set_cache(&span_c, "GET", 3, "mykey", 5);
    lumbre_span_set_db(&span_b, "SELECT 1", 8, "mysql", 5, 0, 2048);
    lumbre_span_set_http_out(&span_a, "http://x", 8, "GET", 3, 200);

    /* Finish in reverse order: c -> b -> a */
    int rc;

    rc = lumbre_span_finish(&span_c, &test_rb, &test_mp, &ctx, 0, 2048);
    assert_int_equal(rc, 0);
    assert_int_equal(ctx.current_parent_id, 2); /* span_c's parent_id */

    rc = lumbre_span_finish(&span_b, &test_rb, &test_mp, &ctx, 0, 2048);
    assert_int_equal(rc, 0);
    assert_int_equal(ctx.current_parent_id, 1); /* span_b's parent_id */

    rc = lumbre_span_finish(&span_a, &test_rb, &test_mp, &ctx, 0, 2048);
    assert_int_equal(rc, 0);
    assert_int_equal(ctx.current_parent_id, 0); /* span_a's parent_id (root) */

    cleanup_ringbuf();
}

/* --------------------------------------------------------------------------
 * Test 6: span_finish encodes to ring buffer
 * ----------------------------------------------------------------------- */

static void test_span_finish_encodes_to_ringbuf(void **state)
{
    (void)state;

    setup_ringbuf(4096);

    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    lumbre_span_t span;
    lumbre_span_start(&span, LUMBRE_SPAN_DB, &ctx);
    lumbre_span_set_db(&span, "SELECT 1", 8, "mysql", 5, 1, 2048);

    int rc = lumbre_span_finish(&span, &test_rb, &test_mp, &ctx, 0, 2048);
    assert_int_equal(rc, 0);

    /* Ring buffer should have data written */
    assert_true(test_rb.header->write_pos > 0);

    /* Read back the msgpack payload */
    uint32_t msg_len = 0;
    const uint8_t *msg = rb_read_first_msg(&test_rb, &msg_len);
    assert_non_null(msg);
    assert_true(msg_len > 0);

    /*
     * Manually verify the msgpack payload:
     * - First byte should be a fixmap header (0x80 | count).
     *   DB spans have 9 fields, so expect 0x89.
     * - Next bytes should be the first key "t" (type):
     *   fixstr of length 1 = 0xA1, then 't' = 0x74.
     * - Then the value for type = 3 (positive fixint = 0x03).
     */
    assert_int_equal(msg[0], 0x89); /* fixmap(9) */
    assert_int_equal(msg[1], 0xA1); /* fixstr(1) */
    assert_int_equal(msg[2], 0x74); /* 't' */
    assert_int_equal(msg[3], 0x03); /* type = 3 (DB) */

    cleanup_ringbuf();
}

/* --------------------------------------------------------------------------
 * Test 7: min_duration filter
 * ----------------------------------------------------------------------- */

static void test_span_finish_min_duration_filter(void **state)
{
    (void)state;

    setup_ringbuf(4096);

    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    /* Very high min_duration: 1 second (spans execute in ns) */
    uint64_t min_duration = 1000000000ULL;

    /* Non-root span (DB): should be filtered */
    lumbre_span_t span_db;
    lumbre_span_start(&span_db, LUMBRE_SPAN_DB, &ctx);
    lumbre_span_set_db(&span_db, "SELECT 1", 8, "mysql", 5, 0, 2048);
    int rc = lumbre_span_finish(&span_db, &test_rb, &test_mp, &ctx, min_duration, 2048);
    assert_int_equal(rc, 1); /* filtered */

    /* Ring buffer should still be empty */
    assert_int_equal(test_rb.header->write_pos, 0);

    /* Root span (HTTP_IN): should NOT be filtered even if short */
    lumbre_span_t span_root;
    lumbre_span_start(&span_root, LUMBRE_SPAN_HTTP_IN, &ctx);
    lumbre_span_set_http_in(&span_root, "/test", 5, "GET", 3);
    rc = lumbre_span_finish(&span_root, &test_rb, &test_mp, &ctx, min_duration, 0);
    assert_int_equal(rc, 0); /* not filtered */

    /* Ring buffer should now have data */
    assert_true(test_rb.header->write_pos > 0);

    cleanup_ringbuf();
}

/* --------------------------------------------------------------------------
 * Test 8: ring buffer full
 * ----------------------------------------------------------------------- */

static void test_span_finish_ringbuf_full(void **state)
{
    (void)state;

    /* Tiny ring buffer: 128 bytes (power of 2). After header overhead
     * for the message format [len:4][payload], space is very limited. */
    setup_ringbuf(128);

    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    /* Fill the buffer with spans until it is full */
    int written = 0;
    int dropped = 0;

    for (int i = 0; i < 20; i++) {
        lumbre_span_t span;
        lumbre_span_start(&span, LUMBRE_SPAN_DB, &ctx);
        lumbre_span_set_db(&span, "SELECT 1", 8, "mysql", 5, 0, 2048);
        int rc = lumbre_span_finish(&span, &test_rb, &test_mp, &ctx, 0, 2048);
        if (rc == 0) {
            written++;
        } else if (rc == -1) {
            dropped++;
        }
    }

    /* We should have at least 1 successful write and at least 1 drop */
    assert_true(written >= 1);
    assert_true(dropped >= 1);

    /* Dropped counter in the header should match */
    assert_true(test_rb.header->dropped >= (uint64_t)dropped);

    cleanup_ringbuf();
}

/* --------------------------------------------------------------------------
 * Test 9: DB query truncation
 * ----------------------------------------------------------------------- */

static void test_span_set_db_query_truncation(void **state)
{
    (void)state;

    lumbre_span_t span;
    memset(&span, 0, sizeof(span));

    /* Build a 5000-byte query */
    char long_query[5000];
    memset(long_query, 'X', sizeof(long_query));

    uint32_t max_query_len = 2048;

    lumbre_span_set_db(&span, long_query, 5000, "mysql", 5, 0, max_query_len);

    /* query_len should be truncated to max_query_len */
    assert_int_equal(span.query_len, 2048);

    /* query pointer should still point to the original (no copy) */
    assert_ptr_equal(span.query, long_query);
}

/* --------------------------------------------------------------------------
 * Test 10: encode each span type — verify field counts
 * ----------------------------------------------------------------------- */

/**
 * Expected field counts per type (from lumbre_span_field_count):
 *   HTTP_IN=8, HTTP_OUT=9, DB=9, REDIS=8, MEMCACHED=8,
 *   FILE_IO=7, SOCKET=7, FUNC=10, CACHE=6 (common only)
 */
static const struct {
    lumbre_span_type_t type;
    uint8_t            expected_map_count;
} span_type_field_counts[] = {
    { LUMBRE_SPAN_HTTP_IN,    8 },
    { LUMBRE_SPAN_HTTP_OUT,   9 },
    { LUMBRE_SPAN_DB,         9 },
    { LUMBRE_SPAN_REDIS,      8 },
    { LUMBRE_SPAN_MEMCACHED,  8 },
    { LUMBRE_SPAN_FILE_IO,    7 },
    { LUMBRE_SPAN_SOCKET,     7 },
    { LUMBRE_SPAN_FUNC,      10 },
    { LUMBRE_SPAN_CACHE,      6 },
};

static void test_span_encode_each_type(void **state)
{
    (void)state;

    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    size_t count = sizeof(span_type_field_counts) / sizeof(span_type_field_counts[0]);

    for (size_t i = 0; i < count; i++) {
        lumbre_span_type_t type = span_type_field_counts[i].type;
        uint8_t expected_count  = span_type_field_counts[i].expected_map_count;

        lumbre_span_t span;
        lumbre_span_start(&span, type, &ctx);

        /* Set payload based on type */
        switch (type) {
        case LUMBRE_SPAN_HTTP_IN:
            lumbre_span_set_http_in(&span, "/path", 5, "GET", 3);
            break;
        case LUMBRE_SPAN_HTTP_OUT:
            lumbre_span_set_http_out(&span, "http://example.com", 18, "POST", 4, 200);
            break;
        case LUMBRE_SPAN_DB:
            lumbre_span_set_db(&span, "SELECT 1", 8, "mysql", 5, 1, 2048);
            break;
        case LUMBRE_SPAN_REDIS:
        case LUMBRE_SPAN_MEMCACHED:
            lumbre_span_set_cache(&span, "GET", 3, "mykey", 5);
            break;
        case LUMBRE_SPAN_FILE_IO:
        case LUMBRE_SPAN_SOCKET:
            lumbre_span_set_file_io(&span, "/tmp/test", 9);
            break;
        case LUMBRE_SPAN_FUNC:
            lumbre_span_set_func(&span, "MyClass", 7, "doStuff", 7, "file.php", 8, 42);
            break;
        default:
            /* CACHE: no special payload */
            break;
        }

        /* Encode directly (not via finish, to avoid needing a ringbuf) */
        lumbre_msgpack_buf mp;
        uint8_t raw[LUMBRE_MSGPACK_BUF_SIZE];
        mp.buf      = raw;
        mp.capacity = sizeof(raw);
        mp.pos      = 0;

        int rc = lumbre_span_encode(&span, &mp);
        assert_int_equal(rc, 0);
        assert_true(mp.pos > 0);

        /*
         * Verify the fixmap header byte.
         * For counts <= 15: 0x80 | count.
         */
        assert_true(expected_count <= 15);
        uint8_t expected_header = (uint8_t)(0x80 | expected_count);
        assert_int_equal(raw[0], expected_header);
    }
}

/* --------------------------------------------------------------------------
 * Test 11: context reset
 * ----------------------------------------------------------------------- */

static void test_context_reset(void **state)
{
    (void)state;

    lumbre_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    lumbre_context_init(&ctx, NULL, mock_random);

    /* Start a few spans (just start, do not finish, to simulate mid-request) */
    lumbre_span_t span_a, span_b;
    lumbre_span_start(&span_a, LUMBRE_SPAN_HTTP_IN, &ctx);
    lumbre_span_start(&span_b, LUMBRE_SPAN_DB, &ctx);

    assert_int_equal(ctx.active, 1);
    assert_int_equal(ctx.span_counter, 3); /* 1 (init) + 2 starts */

    lumbre_context_reset(&ctx);

    assert_int_equal(ctx.active, 0);

    /* trace_id should be zeroed */
    uint8_t zeros[16];
    memset(zeros, 0, 16);
    assert_memory_equal(ctx.trace_id, zeros, 16);

    /* span_counter is NOT reset (per design: it doesn't matter after RSHUTDOWN) */
    assert_int_equal(ctx.span_counter, 3);
}

/* --------------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_context_init_no_propagation),
        cmocka_unit_test(test_context_init_with_propagation),
        cmocka_unit_test(test_span_start_basic),
        cmocka_unit_test(test_span_parent_child_chaining),
        cmocka_unit_test(test_span_finish_unwinds_parent),
        cmocka_unit_test(test_span_finish_encodes_to_ringbuf),
        cmocka_unit_test(test_span_finish_min_duration_filter),
        cmocka_unit_test(test_span_finish_ringbuf_full),
        cmocka_unit_test(test_span_set_db_query_truncation),
        cmocka_unit_test(test_span_encode_each_type),
        cmocka_unit_test(test_context_reset),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
