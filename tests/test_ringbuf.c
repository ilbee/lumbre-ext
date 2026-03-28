/**
 * test_ringbuf.c — CMocka unit tests for lumbre_ringbuf
 *
 * All buffers are created in /tmp for CI portability (not /dev/shm).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "lumbre_ringbuf.h"

/* --------------- Helpers --------------- */

static const char *test_shm_dir(void)
{
    return "/tmp";
}

/**
 * Read a message from the ring buffer data area at the given offset.
 * Returns the payload length and sets *out to point at the payload.
 * Handles padding markers by skipping to offset 0.
 */
static uint32_t read_msg_at(lumbre_ringbuf_t *rb, uint32_t offset,
                            const uint8_t **out)
{
    uint32_t len;
    memcpy(&len, &rb->data[offset], 4);
    if (len == LUMBRE_RINGBUF_PADDING_MARKER) {
        /* Padding — real message is at offset 0 */
        memcpy(&len, &rb->data[0], 4);
        *out = &rb->data[4];
    } else {
        *out = &rb->data[offset + 4];
    }
    return len;
}

/* --------------- Fixtures --------------- */

typedef struct {
    lumbre_ringbuf_t rb;
    uint32_t         capacity;
} test_ctx_t;

static int setup_4096(void **state)
{
    test_ctx_t *ctx = calloc(1, sizeof(test_ctx_t));
    ctx->capacity = 4096;
    assert_int_equal(0,
        lumbre_ringbuf_init(&ctx->rb, test_shm_dir(), (uint32_t)getpid(), 0, ctx->capacity));
    *state = ctx;
    return 0;
}

static int setup_256(void **state)
{
    test_ctx_t *ctx = calloc(1, sizeof(test_ctx_t));
    ctx->capacity = 256;
    assert_int_equal(0,
        lumbre_ringbuf_init(&ctx->rb, test_shm_dir(), (uint32_t)getpid(), 0, ctx->capacity));
    *state = ctx;
    return 0;
}

static int setup_1024(void **state)
{
    test_ctx_t *ctx = calloc(1, sizeof(test_ctx_t));
    ctx->capacity = 1024;
    assert_int_equal(0,
        lumbre_ringbuf_init(&ctx->rb, test_shm_dir(), (uint32_t)getpid(), 0, ctx->capacity));
    *state = ctx;
    return 0;
}

static int teardown(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_destroy(&ctx->rb, 0);
    free(ctx);
    return 0;
}

/* --------------- Test 1: Init + header validation --------------- */

static void test_init_header_validation(void **state)
{
    (void)state;

    lumbre_ringbuf_t rb;
    uint32_t pid = (uint32_t)getpid();
    assert_int_equal(0,
        lumbre_ringbuf_init(&rb, test_shm_dir(), pid, 0, 4096));

    /* File exists */
    struct stat st;
    assert_int_equal(0, stat(rb.path, &st));
    assert_int_equal((off_t)(64 + 4096), st.st_size);

    /* Header fields */
    assert_int_equal(LUMBRE_RINGBUF_MAGIC,   rb.header->magic);
    assert_int_equal(LUMBRE_RINGBUF_VERSION,  rb.header->version);
    assert_int_equal(4096,                    rb.header->capacity);
    assert_int_equal(0,                       rb.header->write_pos);
    assert_int_equal(0,                       rb.header->read_pos);
    assert_int_equal(0,                       rb.header->dropped);
    assert_int_equal(pid,                     rb.header->pid);

    /* Destroy removes file */
    char path_copy[256];
    strncpy(path_copy, rb.path, sizeof(path_copy));
    lumbre_ringbuf_destroy(&rb, 0);
    assert_int_not_equal(0, stat(path_copy, &st));
}

/* --------------- Test 2: Non-power-of-2 capacity --------------- */

static void test_non_power_of_2_capacity(void **state)
{
    (void)state;

    lumbre_ringbuf_t rb;
    assert_int_equal(0,
        lumbre_ringbuf_init(&rb, test_shm_dir(), (uint32_t)getpid(), 0, 3000));

    /* 3000 rounds up to 4096 */
    assert_int_equal(4096, rb.capacity);
    assert_int_equal(4096, rb.header->capacity);
    assert_int_equal(4095, rb.mask);

    lumbre_ringbuf_destroy(&rb, 0);
}

/* --------------- Test 3: Write + read back simple --------------- */

static void test_write_read_simple(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    uint8_t payload[100];
    memset(payload, 0xAB, sizeof(payload));

    assert_int_equal(0, lumbre_ringbuf_write(rb, payload, 100));

    /* write_pos should be 4 (len prefix) + 100 (payload) = 104 */
    assert_int_equal(104, rb->header->write_pos);

    /* Read back manually from data area */
    uint32_t len;
    memcpy(&len, &rb->data[0], 4);
    assert_int_equal(100, len);
    assert_memory_equal(payload, &rb->data[4], 100);
}

/* --------------- Test 4: Multiple sequential writes --------------- */

static void test_multiple_sequential_writes(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    uint32_t sizes[] = {32, 64, 128, 32, 64, 128, 32, 64, 128, 32};
    uint32_t expected_pos = 0;

    for (int i = 0; i < 10; i++) {
        uint8_t payload[128];
        memset(payload, (uint8_t)(i + 1), sizes[i]);
        assert_int_equal(0, lumbre_ringbuf_write(rb, payload, sizes[i]));
        expected_pos += 4 + sizes[i];
    }

    assert_int_equal(expected_pos, rb->header->write_pos);

    /* Read each message back */
    uint32_t offset = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t len;
        memcpy(&len, &rb->data[offset], 4);
        assert_int_equal(sizes[i], len);

        /* Verify fill pattern */
        for (uint32_t j = 0; j < sizes[i]; j++) {
            assert_int_equal((uint8_t)(i + 1), rb->data[offset + 4 + j]);
        }
        offset += 4 + sizes[i];
    }
}

/* --------------- Test 5: Wrap-around --------------- */

static void test_wrap_around(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    /* capacity = 256. Advance read_pos so we have room to wrap. */
    /* Write messages until wrap-around occurs. */
    /* Each message: 4 + 60 = 64 bytes. 256/64 = 4 messages fill the buffer. */
    uint8_t payload[60];
    memset(payload, 0xCC, sizeof(payload));

    /* Simulate daemon consuming: keep read_pos near write_pos */
    /* Write 3 messages (192 bytes out of 256) */
    for (int i = 0; i < 3; i++) {
        assert_int_equal(0, lumbre_ringbuf_write(rb, payload, 60));
    }
    /* Advance read_pos to free the first 3 messages */
    __atomic_store_n(&rb->header->read_pos, rb->header->write_pos, __ATOMIC_RELEASE);

    /* Write 1 more: this takes offset 192, needed=64, 192+64=256 == capacity, fits exactly */
    assert_int_equal(0, lumbre_ringbuf_write(rb, payload, 60));

    /* Advance read_pos again */
    __atomic_store_n(&rb->header->read_pos, rb->header->write_pos, __ATOMIC_RELEASE);

    /* Now write_pos is at 256 (monotonic), offset = 256 % 256 = 0. Next write at offset 0. */
    /* Write a message that would straddle if it started at a non-zero offset */
    /* First, fill up to near the end to force a wrap */
    lumbre_ringbuf_reset(rb);

    /* Write 3 messages of 60 bytes each = 3 * 64 = 192 bytes used, 64 remaining */
    for (int i = 0; i < 3; i++) {
        memset(payload, (uint8_t)(i + 1), sizeof(payload));
        assert_int_equal(0, lumbre_ringbuf_write(rb, payload, 60));
    }

    /* Advance read_pos to free space */
    __atomic_store_n(&rb->header->read_pos, rb->header->write_pos, __ATOMIC_RELEASE);

    /* Now write a 100-byte payload (needed=104). Offset=192, 192+104=296 > 256 → wrap */
    uint8_t big_payload[100];
    memset(big_payload, 0xDD, sizeof(big_payload));
    assert_int_equal(0, lumbre_ringbuf_write(rb, big_payload, 100));

    /* Verify padding marker at offset 192 */
    uint32_t marker;
    memcpy(&marker, &rb->data[192], 4);
    assert_int_equal(LUMBRE_RINGBUF_PADDING_MARKER, marker);

    /* Verify message at offset 0 */
    uint32_t len;
    memcpy(&len, &rb->data[0], 4);
    assert_int_equal(100, len);
    assert_memory_equal(big_payload, &rb->data[4], 100);
}

/* --------------- Test 6: Buffer full → drop --------------- */

static void test_buffer_full_drop(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    /* capacity = 256, read_pos stays at 0 (simulates slow daemon) */
    uint8_t payload[32];
    memset(payload, 0xEE, sizeof(payload));

    int writes = 0;
    int drops = 0;
    for (int i = 0; i < 100; i++) {
        int rc = lumbre_ringbuf_write(rb, payload, 32);
        if (rc == 0) {
            writes++;
        } else {
            drops++;
        }
    }

    /* Should have written some and dropped some */
    assert_true(writes > 0);
    assert_true(drops > 0);
    assert_int_equal(drops, (int)rb->header->dropped);

    /* write_pos must not exceed capacity (no overflow beyond buffer) */
    /* write_pos is monotonic, but actual offset = write_pos % capacity <= capacity */
    uint32_t final_offset = (uint32_t)(rb->header->write_pos & rb->mask);
    assert_true(final_offset < rb->capacity);

    /* Verify successful messages are not corrupted */
    uint32_t offset = 0;
    for (int i = 0; i < writes; i++) {
        uint32_t len;
        memcpy(&len, &rb->data[offset], 4);
        if (len == LUMBRE_RINGBUF_PADDING_MARKER) {
            /* Skip padding */
            offset = 0;
            memcpy(&len, &rb->data[0], 4);
            offset = 4 + len;
            i++;
            continue;
        }
        assert_int_equal(32, len);
        assert_memory_equal(payload, &rb->data[offset + 4], 32);
        offset += 4 + 32;
    }
}

/* --------------- Test 7: Zero payload --------------- */

static void test_zero_payload(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    assert_int_equal(0, lumbre_ringbuf_write(rb, NULL, 0));

    /* write_pos advances by 4 (just the length prefix) */
    assert_int_equal(4, rb->header->write_pos);

    /* Length prefix should be 0 */
    uint32_t len;
    memcpy(&len, &rb->data[0], 4);
    assert_int_equal(0, len);
}

/* --------------- Test 8: Payload near capacity --------------- */

static void test_payload_near_capacity(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    /* capacity = 1024. Max payload that fits: 1024 - 4 = 1020 bytes */
    uint8_t payload[1020];
    memset(payload, 0xAA, sizeof(payload));

    /* 1016 bytes: needed = 4 + 1016 = 1020 <= 1024 → fits */
    assert_int_equal(0, lumbre_ringbuf_write(rb, payload, 1016));

    /* Reset for next test */
    lumbre_ringbuf_reset(rb);
    memset(rb->data, 0, rb->capacity);

    /* 1020 bytes: needed = 4 + 1020 = 1024 <= 1024 → fits exactly */
    assert_int_equal(0, lumbre_ringbuf_write(rb, payload, 1020));

    /* Reset again */
    lumbre_ringbuf_reset(rb);
    memset(rb->data, 0, rb->capacity);

    /* 1021 bytes: needed = 4 + 1021 = 1025 > 1024 → drop */
    uint8_t big_payload[1021];
    memset(big_payload, 0xBB, sizeof(big_payload));
    assert_int_equal(-1, lumbre_ringbuf_write(rb, big_payload, 1021));
    assert_int_equal(1, rb->header->dropped);
}

/* --------------- Test 9: Double destroy --------------- */

static void test_double_destroy(void **state)
{
    (void)state;

    lumbre_ringbuf_t rb;
    assert_int_equal(0,
        lumbre_ringbuf_init(&rb, test_shm_dir(), (uint32_t)getpid(), 0, 4096));

    lumbre_ringbuf_destroy(&rb, 0);
    /* Second destroy must not crash (header is NULL) */
    lumbre_ringbuf_destroy(&rb, 0);
}

/* --------------- Test 10: SPSC simulation --------------- */

static void test_spsc_simulation(void **state)
{
    test_ctx_t *ctx = *state;
    lumbre_ringbuf_t *rb = &ctx->rb;

    /* capacity = 256. Write small messages, simulate daemon consuming. */
    uint8_t payload[20];
    int total_written = 0;

    /* Phase 1: Write messages until buffer is full (some may drop due to
     * wrap-around padding consuming extra space — that's expected). */
    int phase1_count = 0;
    for (int i = 0; i < 20; i++) {
        memset(payload, (uint8_t)(i + 1), sizeof(payload));
        int rc = lumbre_ringbuf_write(rb, payload, 20);
        if (rc == 0) {
            phase1_count++;
            total_written++;
        } else {
            break;
        }
    }
    assert_true(phase1_count > 0);

    /* Simulate daemon: read all messages and advance read_pos */
    uint64_t daemon_read_pos = 0;
    for (int i = 0; i < phase1_count; i++) {
        uint32_t offset = (uint32_t)(daemon_read_pos & rb->mask);
        uint32_t len;
        memcpy(&len, &rb->data[offset], 4);

        if (len == LUMBRE_RINGBUF_PADDING_MARKER) {
            daemon_read_pos += (uint64_t)(rb->capacity - offset);
            offset = 0;
            memcpy(&len, &rb->data[0], 4);
        }

        assert_int_equal(20, len);
        /* Verify fill pattern */
        assert_int_equal((uint8_t)(i + 1), rb->data[offset + 4]);

        daemon_read_pos += 4 + len;
    }

    /* Publish read_pos so producer sees free space */
    __atomic_store_n(&rb->header->read_pos, daemon_read_pos, __ATOMIC_RELEASE);

    /* Phase 2: Write more messages into freed space.
     * Some may drop due to wrap-around padding — that's OK. */
    int phase2_count = 0;
    for (int i = 0; i < 20; i++) {
        memset(payload, (uint8_t)(i + 100), sizeof(payload));
        int rc = lumbre_ringbuf_write(rb, payload, 20);
        if (rc == 0) {
            phase2_count++;
            total_written++;
        } else {
            break;
        }
    }
    assert_true(phase2_count > 0);

    /* Verify phase 2 messages by reading from daemon_read_pos */
    for (int i = 0; i < phase2_count; i++) {
        uint32_t offset = (uint32_t)(daemon_read_pos & rb->mask);
        uint32_t len;
        memcpy(&len, &rb->data[offset], 4);

        if (len == LUMBRE_RINGBUF_PADDING_MARKER) {
            daemon_read_pos += (uint64_t)(rb->capacity - offset);
            offset = 0;
            memcpy(&len, &rb->data[0], 4);
        }

        assert_int_equal(20, len);
        assert_int_equal((uint8_t)(i + 100), rb->data[offset + 4]);

        daemon_read_pos += 4 + len;
    }

    /* SPSC cycle works: we successfully wrote, consumed, and wrote again.
     * The key invariant is data integrity, not zero drops (wrap-around
     * padding can cause drops in a small buffer — that's by design). */
    assert_true(total_written > phase1_count);  /* Phase 2 did write something */
}

/* --------------- Main --------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init_header_validation),
        cmocka_unit_test(test_non_power_of_2_capacity),
        cmocka_unit_test_setup_teardown(test_write_read_simple, setup_4096, teardown),
        cmocka_unit_test_setup_teardown(test_multiple_sequential_writes, setup_4096, teardown),
        cmocka_unit_test_setup_teardown(test_wrap_around, setup_256, teardown),
        cmocka_unit_test_setup_teardown(test_buffer_full_drop, setup_256, teardown),
        cmocka_unit_test_setup_teardown(test_zero_payload, setup_4096, teardown),
        cmocka_unit_test_setup_teardown(test_payload_near_capacity, setup_1024, teardown),
        cmocka_unit_test(test_double_destroy),
        cmocka_unit_test_setup_teardown(test_spsc_simulation, setup_256, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
