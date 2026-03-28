/**
 * lumbre_span.h — Span lifecycle: types, structs, context, API.
 *
 * Pure C — no PHP headers. Designed for testability: all functions
 * take explicit parameters instead of accessing module globals.
 * The PHP integration layer (phase 5) wraps these and passes LUMBRE_G.
 */

#ifndef LUMBRE_SPAN_H
#define LUMBRE_SPAN_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <time.h>

#include "lumbre_ringbuf.h"
#include "lumbre_msgpack.h"

/* --------------------------------------------------------------------------
 * Span type constants (uint8_t) — match ClickHouse Enum8 and Go daemon
 * ----------------------------------------------------------------------- */

#define LUMBRE_SPAN_HTTP_IN    ((lumbre_span_type_t)1)
#define LUMBRE_SPAN_HTTP_OUT   ((lumbre_span_type_t)2)
#define LUMBRE_SPAN_DB         ((lumbre_span_type_t)3)
#define LUMBRE_SPAN_REDIS      ((lumbre_span_type_t)4)
#define LUMBRE_SPAN_MEMCACHED  ((lumbre_span_type_t)5)
#define LUMBRE_SPAN_FILE_IO    ((lumbre_span_type_t)6)
#define LUMBRE_SPAN_SOCKET     ((lumbre_span_type_t)7)
#define LUMBRE_SPAN_FUNC       ((lumbre_span_type_t)8)
#define LUMBRE_SPAN_CACHE      ((lumbre_span_type_t)9)

typedef uint8_t lumbre_span_type_t;

/* --------------------------------------------------------------------------
 * Span struct — stack-allocated in Zend hooks, ~200 bytes
 * ----------------------------------------------------------------------- */

typedef struct {
    /* Fixed fields */
    lumbre_span_type_t type;
    uint8_t  trace_id[16];
    uint64_t span_id;
    uint64_t parent_id;
    uint64_t start_ns;
    uint64_t duration_ns;

    /* Payload fields (flat, not union). Only fields relevant to `type`
       are populated; the rest stay zeroed from memset in span_start. */

    /* HTTP out / file_io / socket */
    const char *url;
    uint32_t    url_len;

    /* HTTP in / HTTP out */
    const char *method;
    uint32_t    method_len;

    /* HTTP out */
    uint16_t status_code;

    /* DB */
    const char *query;
    uint32_t    query_len;
    const char *db_type;
    uint32_t    db_type_len;
    uint32_t    rows_affected;

    /* Redis / Memcached */
    const char *cache_cmd;
    uint32_t    cache_cmd_len;
    const char *cache_key;
    uint32_t    cache_key_len;

    /* Func */
    const char *func_class;
    uint32_t    func_class_len;
    const char *func_name;
    uint32_t    func_name_len;
    const char *func_file;
    uint32_t    func_file_len;
    uint32_t    func_line;

    /* HTTP in */
    const char *request_uri;
    uint32_t    request_uri_len;
} lumbre_span_t;

/* --------------------------------------------------------------------------
 * Request context — one per worker, stored in module globals
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t  trace_id[16];
    uint64_t root_span_id;
    uint64_t current_parent_id;
    uint64_t span_counter;
    int      active;
    int      full_trace;
} lumbre_context_t;

/* --------------------------------------------------------------------------
 * Clock
 * ----------------------------------------------------------------------- */

static inline uint64_t lumbre_clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* --------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/**
 * Initialise request context.
 *
 * @param ctx                 Context to initialise.
 * @param propagated_trace_id 16-byte trace ID from upstream (NULL to generate).
 * @param random_func         Function that fills buf with len random bytes.
 *                            Returns 0 on success, -1 on failure.
 */
void lumbre_context_init(
    lumbre_context_t *ctx,
    const uint8_t    *propagated_trace_id,
    int             (*random_func)(void *buf, size_t len)
);

/**
 * Reset context after RSHUTDOWN. Sets active=0, zeroes trace_id.
 */
void lumbre_context_reset(lumbre_context_t *ctx);

/**
 * Start a span: zero-fill, assign IDs from context, record start time.
 *
 * @param span Stack-allocated span to initialise.
 * @param type Span type constant.
 * @param ctx  Current request context.
 */
void lumbre_span_start(
    lumbre_span_t      *span,
    lumbre_span_type_t  type,
    lumbre_context_t   *ctx
);

/**
 * Finish a span: compute duration, encode to msgpack, write to ring buffer.
 *
 * @param span            The span to finish.
 * @param rb              Ring buffer handle.
 * @param mp_buf          Pre-allocated msgpack buffer.
 * @param ctx             Current request context.
 * @param min_duration_ns Minimum duration to record (0 = no filter).
 *                        Root spans (HTTP_IN) are never filtered.
 * @param max_query_len   Maximum query length for DB spans (for truncation
 *                        that was not already applied by the setter).
 * @return 0 on success, -1 on error (ring full / encode overflow),
 *         1 if span was filtered (too short).
 */
int lumbre_span_finish(
    lumbre_span_t     *span,
    lumbre_ringbuf_t  *rb,
    lumbre_msgpack_buf *mp_buf,
    lumbre_context_t  *ctx,
    uint64_t           min_duration_ns,
    uint32_t           max_query_len
);

/* --------------------------------------------------------------------------
 * Payload setters — call between span_start and span_finish
 * ----------------------------------------------------------------------- */

static inline void lumbre_span_set_http_out(
    lumbre_span_t *span,
    const char *url,    uint32_t url_len,
    const char *method, uint32_t method_len,
    uint16_t    status_code)
{
    span->url         = url;
    span->url_len     = url_len;
    span->method      = method;
    span->method_len  = method_len;
    span->status_code = status_code;
}

static inline void lumbre_span_set_db(
    lumbre_span_t *span,
    const char *query,   uint32_t query_len,
    const char *db_type, uint32_t db_type_len,
    uint32_t    rows_affected,
    uint32_t    max_query_len)
{
    span->query         = query;
    span->query_len     = (query_len > max_query_len) ? max_query_len : query_len;
    span->db_type       = db_type;
    span->db_type_len   = db_type_len;
    span->rows_affected = rows_affected;
}

static inline void lumbre_span_set_cache(
    lumbre_span_t *span,
    const char *cmd, uint32_t cmd_len,
    const char *key, uint32_t key_len)
{
    span->cache_cmd     = cmd;
    span->cache_cmd_len = cmd_len;
    span->cache_key     = key;
    span->cache_key_len = key_len;
}

static inline void lumbre_span_set_func(
    lumbre_span_t *span,
    const char *cls,  uint32_t cls_len,
    const char *name, uint32_t name_len,
    const char *file, uint32_t file_len,
    uint32_t    line)
{
    span->func_class     = cls;
    span->func_class_len = cls_len;
    span->func_name      = name;
    span->func_name_len  = name_len;
    span->func_file      = file;
    span->func_file_len  = file_len;
    span->func_line      = line;
}

static inline void lumbre_span_set_http_in(
    lumbre_span_t *span,
    const char *uri,    uint32_t uri_len,
    const char *method, uint32_t method_len)
{
    span->request_uri     = uri;
    span->request_uri_len = uri_len;
    span->method          = method;
    span->method_len      = method_len;
}

static inline void lumbre_span_set_file_io(
    lumbre_span_t *span,
    const char *path, uint32_t path_len)
{
    span->url     = path;
    span->url_len = path_len;
}

/* --------------------------------------------------------------------------
 * Internal — encode span to msgpack (declared here for test access)
 * ----------------------------------------------------------------------- */

int lumbre_span_encode(const lumbre_span_t *span, lumbre_msgpack_buf *mp_buf);

#endif /* LUMBRE_SPAN_H */
