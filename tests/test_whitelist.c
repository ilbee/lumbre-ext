/**
 * test_whitelist.c — CMocka unit tests for lumbre_whitelist (Phase 4).
 *
 * Tests the hash table: init, match, case-insensitivity, miss,
 * full entry coverage, destroy + re-init cycle.
 *
 * Build: see Makefile.frag target test-unit-whitelist.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <cmocka.h>

#include "lumbre_whitelist.h"

/* --------------------------------------------------------------------------
 * Shared whitelist instance
 * ----------------------------------------------------------------------- */

static lumbre_whitelist_t wl;

static int setup_whitelist(void **state)
{
    (void)state;
    lumbre_whitelist_init(&wl);
    return 0;
}

static int teardown_whitelist(void **state)
{
    (void)state;
    lumbre_whitelist_destroy(&wl);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 1: init + match basic
 * ----------------------------------------------------------------------- */

static void test_match_basic(void **state)
{
    (void)state;

    const lumbre_whitelist_entry_t *e;

    /* curl_exec -> HTTP_OUT */
    e = lumbre_whitelist_match(&wl, NULL, 0, "curl_exec", 9);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_HTTP_OUT);

    /* PDO::execute -> DB */
    e = lumbre_whitelist_match(&wl, "PDO", 3, "execute", 7);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_DB);

    /* Redis::get -> REDIS */
    e = lumbre_whitelist_match(&wl, "Redis", 5, "get", 3);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_REDIS);
}

/* --------------------------------------------------------------------------
 * Test 2: miss — functions not in the whitelist
 * ----------------------------------------------------------------------- */

static void test_match_miss(void **state)
{
    (void)state;

    const lumbre_whitelist_entry_t *e;

    e = lumbre_whitelist_match(&wl, NULL, 0, "strlen", 6);
    assert_null(e);

    e = lumbre_whitelist_match(&wl, NULL, 0, "array_map", 9);
    assert_null(e);

    e = lumbre_whitelist_match(&wl, "SomeClass", 9, "doStuff", 7);
    assert_null(e);
}

/* --------------------------------------------------------------------------
 * Test 3: case-insensitive matching
 * ----------------------------------------------------------------------- */

static void test_match_case_insensitive(void **state)
{
    (void)state;

    const lumbre_whitelist_entry_t *e;

    /* lowercase class, uppercase method */
    e = lumbre_whitelist_match(&wl, "pdo", 3, "EXECUTE", 7);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_DB);

    /* uppercase class, mixed-case method */
    e = lumbre_whitelist_match(&wl, "PDO", 3, "Execute", 7);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_DB);

    /* all uppercase global function */
    e = lumbre_whitelist_match(&wl, NULL, 0, "CURL_EXEC", 9);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_HTTP_OUT);

    /* mixed case */
    e = lumbre_whitelist_match(&wl, "redis", 5, "SET", 3);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_REDIS);

    e = lumbre_whitelist_match(&wl, "MEMCACHED", 9, "Get", 3);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_MEMCACHED);
}

/* --------------------------------------------------------------------------
 * Test 4: all entries — verify every known whitelist entry matches
 * ----------------------------------------------------------------------- */

static const struct {
    const char *class_name;
    size_t      class_len;
    const char *func_name;
    size_t      func_len;
    uint8_t     expected_type;
} all_entries[] = {
    /* HTTP out */
    { NULL,        0, "curl_exec",            9, LUMBRE_SPAN_HTTP_OUT  },
    { NULL,        0, "curl_multi_exec",     15, LUMBRE_SPAN_HTTP_OUT  },
    /* DB */
    { "PDO",       3, "execute",              7, LUMBRE_SPAN_DB        },
    { "PDO",       3, "query",                5, LUMBRE_SPAN_DB        },
    { NULL,        0, "mysqli_query",         12, LUMBRE_SPAN_DB       },
    { NULL,        0, "mysqli_real_query",    17, LUMBRE_SPAN_DB       },
    { NULL,        0, "pg_query",             8, LUMBRE_SPAN_DB        },
    { NULL,        0, "pg_execute",          10, LUMBRE_SPAN_DB        },
    /* Redis */
    { "Redis",     5, "get",                  3, LUMBRE_SPAN_REDIS     },
    { "Redis",     5, "set",                  3, LUMBRE_SPAN_REDIS     },
    { "Redis",     5, "mget",                 4, LUMBRE_SPAN_REDIS     },
    { "Redis",     5, "pipeline",             8, LUMBRE_SPAN_REDIS     },
    /* Memcached */
    { "Memcached", 9, "get",                  3, LUMBRE_SPAN_MEMCACHED },
    { "Memcached", 9, "set",                  3, LUMBRE_SPAN_MEMCACHED },
    { "Memcached", 9, "getMulti",             8, LUMBRE_SPAN_MEMCACHED },
    /* File I/O */
    { NULL,        0, "file_get_contents",   17, LUMBRE_SPAN_FILE_IO   },
    { NULL,        0, "file_put_contents",   17, LUMBRE_SPAN_FILE_IO   },
    { NULL,        0, "fopen",                5, LUMBRE_SPAN_FILE_IO   },
    { NULL,        0, "fwrite",               6, LUMBRE_SPAN_FILE_IO   },
    { NULL,        0, "fread",                5, LUMBRE_SPAN_FILE_IO   },
    /* Socket */
    { NULL,        0, "fsockopen",            9, LUMBRE_SPAN_SOCKET    },
    { NULL,        0, "stream_socket_client", 20, LUMBRE_SPAN_SOCKET   },
};

static void test_match_all_entries(void **state)
{
    (void)state;

    size_t count = sizeof(all_entries) / sizeof(all_entries[0]);

    for (size_t i = 0; i < count; i++) {
        const lumbre_whitelist_entry_t *e = lumbre_whitelist_match(
            &wl,
            all_entries[i].class_name,
            all_entries[i].class_len,
            all_entries[i].func_name,
            all_entries[i].func_len
        );

        assert_non_null(e);
        assert_int_equal(e->span_type, all_entries[i].expected_type);
    }
}

/* --------------------------------------------------------------------------
 * Test 5: collision handling (implicitly tested by having 22 entries in 64
 *         slots, but we explicitly verify that all are reachable)
 * ----------------------------------------------------------------------- */

static void test_collision_handling(void **state)
{
    (void)state;

    /* Count occupied slots */
    int occupied = 0;
    for (uint32_t i = 0; i < LUMBRE_WHITELIST_SIZE; i++) {
        if (wl.entries[i].occupied) {
            occupied++;
        }
    }

    /* We should have exactly 22 entries (the static table count) */
    assert_int_equal(occupied, 22);

    /* Verify all are findable via match (same as test 4 but proves
     * linear probing resolves all collisions correctly) */
    size_t count = sizeof(all_entries) / sizeof(all_entries[0]);
    for (size_t i = 0; i < count; i++) {
        const lumbre_whitelist_entry_t *e = lumbre_whitelist_match(
            &wl,
            all_entries[i].class_name,
            all_entries[i].class_len,
            all_entries[i].func_name,
            all_entries[i].func_len
        );
        assert_non_null(e);
    }
}

/* --------------------------------------------------------------------------
 * Test 6: performance benchmark (informational, not a pass/fail)
 * ----------------------------------------------------------------------- */

static void test_lookup_performance(void **state)
{
    (void)state;

    const int iterations = 1000000;
    size_t entry_count = sizeof(all_entries) / sizeof(all_entries[0]);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < iterations; i++) {
        size_t idx = (size_t)i % entry_count;
        const lumbre_whitelist_entry_t *e = lumbre_whitelist_match(
            &wl,
            all_entries[idx].class_name,
            all_entries[idx].class_len,
            all_entries[idx].func_name,
            all_entries[idx].func_len
        );
        /* Prevent compiler from optimizing away the call */
        assert_non_null(e);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9
                      + (double)(end.tv_nsec - start.tv_nsec);
    double avg_ns = elapsed_ns / iterations;

    /* Print benchmark result (informational) */
    fprintf(stderr, "\n[bench] whitelist lookup: %.1f ns/op (%d iterations)\n",
            avg_ns, iterations);

    /* Soft assertion: should be well under 100ns/op on modern hardware */
    assert_true(avg_ns < 1000.0);
}

/* --------------------------------------------------------------------------
 * Test 7: destroy + re-init — verify whitelist works after a full cycle
 * ----------------------------------------------------------------------- */

static void test_destroy_and_reinit(void **state)
{
    (void)state;

    /* wl is already init'd by setup. Match something first. */
    const lumbre_whitelist_entry_t *e;
    e = lumbre_whitelist_match(&wl, NULL, 0, "curl_exec", 9);
    assert_non_null(e);

    /* Destroy */
    lumbre_whitelist_destroy(&wl);

    /* After destroy, all entries should be zeroed (misses) */
    e = lumbre_whitelist_match(&wl, NULL, 0, "curl_exec", 9);
    assert_null(e);

    /* Re-init */
    lumbre_whitelist_init(&wl);

    /* Should match again */
    e = lumbre_whitelist_match(&wl, NULL, 0, "curl_exec", 9);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_HTTP_OUT);

    e = lumbre_whitelist_match(&wl, "Redis", 5, "get", 3);
    assert_non_null(e);
    assert_int_equal(e->span_type, LUMBRE_SPAN_REDIS);
}

/* --------------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_match_basic,
            setup_whitelist, teardown_whitelist),
        cmocka_unit_test_setup_teardown(test_match_miss,
            setup_whitelist, teardown_whitelist),
        cmocka_unit_test_setup_teardown(test_match_case_insensitive,
            setup_whitelist, teardown_whitelist),
        cmocka_unit_test_setup_teardown(test_match_all_entries,
            setup_whitelist, teardown_whitelist),
        cmocka_unit_test_setup_teardown(test_collision_handling,
            setup_whitelist, teardown_whitelist),
        cmocka_unit_test_setup_teardown(test_lookup_performance,
            setup_whitelist, teardown_whitelist),
        cmocka_unit_test_setup_teardown(test_destroy_and_reinit,
            setup_whitelist, teardown_whitelist),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
