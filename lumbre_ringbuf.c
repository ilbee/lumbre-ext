/**
 * lumbre_ringbuf.c — SPSC lock-free ring buffer implementation
 *
 * Pure C11, no PHP headers.  Uses GCC atomic builtins for portability
 * across GCC and Clang without requiring <stdatomic.h>.
 */

#include "lumbre_ringbuf.h"

#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

/* --------------- Compiler pragmas --------------- */

/*
 * The packed header struct causes Clang to warn about misaligned atomics.
 * The fields we access atomically (write_pos @16, read_pos @24, dropped @32)
 * are in fact 8-byte aligned by design — the compiler just cannot verify
 * this through the packed attribute.  Suppress the false positive.
 */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#endif

/* --------------- Static assertions --------------- */

_Static_assert(
    sizeof(lumbre_ringbuf_header_t) == 64,
    "lumbre_ringbuf_header_t must be exactly 64 bytes"
);

/* Verify atomic fields are naturally aligned within the header */
_Static_assert(
    __builtin_offsetof(lumbre_ringbuf_header_t, write_pos) % 8 == 0,
    "write_pos must be 8-byte aligned"
);
_Static_assert(
    __builtin_offsetof(lumbre_ringbuf_header_t, read_pos) % 8 == 0,
    "read_pos must be 8-byte aligned"
);
_Static_assert(
    __builtin_offsetof(lumbre_ringbuf_header_t, dropped) % 8 == 0,
    "dropped must be 8-byte aligned"
);

/* --------------- Helpers --------------- */

uint32_t lumbre_next_power_of_2(uint32_t v)
{
    if (v == 0) {
        return 1;
    }
    if (LUMBRE_IS_POWER_OF_TWO(v)) {
        return v;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/* --------------- Init --------------- */

int lumbre_ringbuf_init(
    lumbre_ringbuf_t *rb,
    const char       *shm_dir,
    uint32_t          pid,
    uint64_t          worker_id,
    uint32_t          capacity)
{
    /* Round capacity to next power of 2 */
    capacity = lumbre_next_power_of_2(capacity);

    /* Build path: <shm_dir>/lumbre_<pid>_<worker_id> */
    int n = snprintf(rb->path, sizeof(rb->path), "%s/lumbre_%u_%lu",
                     shm_dir, (unsigned)pid, (unsigned long)worker_id);
    if (n < 0 || (size_t)n >= sizeof(rb->path)) {
        return -1;
    }

    /* Open / create / truncate */
    int fd = open(rb->path, O_RDWR | O_CREAT | O_TRUNC, 0660);
    if (fd < 0) {
        return -1;
    }

    size_t total_size = (size_t)LUMBRE_RINGBUF_HEADER_SIZE + (size_t)capacity;

    if (ftruncate(fd, (off_t)total_size) != 0) {
        close(fd);
        unlink(rb->path);
        return -1;
    }

    /* mmap shared */
    void *map = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        unlink(rb->path);
        return -1;
    }

    /* Zero the entire region */
    memset(map, 0, total_size);

    /* Write header */
    lumbre_ringbuf_header_t *hdr = (lumbre_ringbuf_header_t *)map;
    hdr->magic     = LUMBRE_RINGBUF_MAGIC;
    hdr->version   = LUMBRE_RINGBUF_VERSION;
    hdr->capacity  = capacity;
    hdr->pid       = pid;
    hdr->worker_id = worker_id;
    /* write_pos, read_pos, dropped already 0 from memset */

    /* Fill handle */
    rb->header         = hdr;
    rb->data           = (uint8_t *)map + LUMBRE_RINGBUF_HEADER_SIZE;
    rb->capacity       = capacity;
    rb->mask           = capacity - 1;
    rb->local_write_pos = 0;
    rb->fd             = fd;
    rb->mmap_size      = total_size;
    rb->worker_id      = worker_id;

    return 0;
}

/* --------------- Write (SPSC producer hot path) --------------- */

int lumbre_ringbuf_write(
    lumbre_ringbuf_t *rb,
    const void       *payload,
    uint32_t          payload_len)
{
    uint32_t needed = 4 + payload_len;  /* 4-byte length prefix + payload */

    /* Read consumer position (relaxed: stale value is safe, worst case we drop) */
    uint64_t read_pos = __atomic_load_n(&rb->header->read_pos, __ATOMIC_RELAXED);

    /* Check space */
    if ((rb->local_write_pos - read_pos) + needed > rb->capacity) {
        __atomic_fetch_add(&rb->header->dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }

    uint32_t offset = (uint32_t)(rb->local_write_pos & rb->mask);

    /* Wrap-around: message would straddle the end of the buffer */
    if (offset + needed > rb->capacity) {
        /* Write padding marker at current offset */
        uint32_t marker = LUMBRE_RINGBUF_PADDING_MARKER;
        memcpy(&rb->data[offset], &marker, 4);

        /* Advance past the remaining space to wrap to offset 0 */
        rb->local_write_pos += (uint64_t)(rb->capacity - offset);
        offset = 0;

        /* Re-check space after consuming padding bytes */
        if ((rb->local_write_pos - read_pos) + needed > rb->capacity) {
            __atomic_fetch_add(&rb->header->dropped, 1, __ATOMIC_RELAXED);
            return -1;
        }
    }

    /* Write length prefix (little-endian native) */
    memcpy(&rb->data[offset], &payload_len, 4);

    /* Write payload */
    memcpy(&rb->data[offset + 4], payload, payload_len);

    /* Advance local write position */
    rb->local_write_pos += needed;

    /* Publish to consumer with release semantics */
    __atomic_store_n(&rb->header->write_pos, rb->local_write_pos, __ATOMIC_RELEASE);

    return 0;
}

/* --------------- Destroy --------------- */

void lumbre_ringbuf_destroy(lumbre_ringbuf_t *rb, int skip_unlink)
{
    if (rb->header == NULL) {
        return;  /* double-destroy safe */
    }

    munmap(rb->header, rb->mmap_size);
    close(rb->fd);

    if (!skip_unlink) {
        unlink(rb->path);
    }

    rb->header = NULL;
}

/* --------------- Reset (test utility) --------------- */

void lumbre_ringbuf_reset(lumbre_ringbuf_t *rb)
{
    __atomic_store_n(&rb->header->write_pos, (uint64_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->header->read_pos, (uint64_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->header->dropped, (uint64_t)0, __ATOMIC_RELAXED);
    rb->local_write_pos = 0;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
