/**
 * lumbre_ringbuf.h — SPSC lock-free ring buffer for PHP lumbre
 *
 * Shared memory ring buffer between PHP workers (producer) and
 * the Go daemon (consumer). One buffer per worker.
 *
 * Layout: [header 64 bytes][data ring `capacity` bytes]
 */

#ifndef LUMBRE_RINGBUF_H
#define LUMBRE_RINGBUF_H

#include <stdint.h>
#include <stddef.h>

/* --------------- Constants --------------- */

#define LUMBRE_RINGBUF_MAGIC           0x54524143u   /* "TRAC" */
#define LUMBRE_RINGBUF_VERSION         1u
#define LUMBRE_RINGBUF_HEADER_SIZE     64u
#define LUMBRE_RINGBUF_DEFAULT_CAPACITY 4194304u     /* 4 MB */
#define LUMBRE_RINGBUF_PADDING_MARKER  0xFFFFFFFFu

/* --------------- Macros --------------- */

#define LUMBRE_IS_POWER_OF_TWO(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))

/* --------------- Header struct (shared via mmap) --------------- */

/**
 * Placed at the first 64 bytes of the mmap'd file.
 * Packed to guarantee identical binary layout with the Go daemon.
 *
 * Offset  Size  Field
 * ------  ----  ----------
 * 0       4     magic
 * 4       4     version
 * 8       4     capacity
 * 12      4     _pad0
 * 16      8     write_pos   (atomic, PHP writes only)
 * 24      8     read_pos    (atomic, daemon writes only)
 * 32      8     dropped     (atomic, PHP increments)
 * 40      4     pid
 * 44      8     worker_id
 * 52      12    _reserved
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t _pad0;
    uint64_t write_pos;
    uint64_t read_pos;
    uint64_t dropped;
    uint32_t pid;
    uint64_t worker_id;
    uint8_t  _reserved[12];
} lumbre_ringbuf_header_t;

/* --------------- Handle struct (process-local, not shared) --------------- */

typedef struct {
    lumbre_ringbuf_header_t *header;
    uint8_t  *data;
    uint32_t  capacity;
    uint32_t  mask;
    uint64_t  local_write_pos;
    int       fd;
    size_t    mmap_size;
    char      path[256];
    uint64_t  worker_id;
} lumbre_ringbuf_t;

/* --------------- Public API --------------- */

/**
 * Initialise a ring buffer backed by a shared-memory file.
 *
 * @param rb        Handle to initialise (caller-allocated).
 * @param shm_dir   Directory for the shm file (e.g. "/dev/shm").
 * @param pid       PID of the PHP worker.
 * @param worker_id Unique worker identifier (0 for NTS/FPM, thread counter for ZTS).
 * @param capacity  Data ring size in bytes (rounded up to next power of 2).
 * @return 0 on success, -1 on error (errno set).
 */
int lumbre_ringbuf_init(
    lumbre_ringbuf_t *rb,
    const char       *shm_dir,
    uint32_t          pid,
    uint64_t          worker_id,
    uint32_t          capacity
);

/**
 * Write a span payload into the ring buffer (SPSC producer side).
 *
 * @param rb          Handle.
 * @param payload     Data to write.
 * @param payload_len Length in bytes.
 * @return 0 on success, -1 if buffer full (dropped counter incremented).
 */
int lumbre_ringbuf_write(
    lumbre_ringbuf_t *rb,
    const void       *payload,
    uint32_t          payload_len
);

/**
 * Destroy the ring buffer: munmap, close fd, optionally unlink the file.
 *
 * @param rb          Handle.
 * @param skip_unlink If non-zero, the shm file is NOT removed (used by CLI SAPI
 *                    so the daemon can drain remaining spans).
 */
void lumbre_ringbuf_destroy(lumbre_ringbuf_t *rb, int skip_unlink);

/**
 * Reset write_pos, read_pos and dropped to 0. For tests only.
 */
void lumbre_ringbuf_reset(lumbre_ringbuf_t *rb);

/* --------------- Helpers --------------- */

/**
 * Round up to the next power of 2 (returns v unchanged if already power of 2).
 */
uint32_t lumbre_next_power_of_2(uint32_t v);

#endif /* LUMBRE_RINGBUF_H */
