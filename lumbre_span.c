/**
 * lumbre_span.c — Span lifecycle implementation.
 *
 * Pure C — no PHP headers. All state is passed explicitly via parameters
 * so this file can be tested without a PHP runtime.
 */

#include "lumbre_span.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Helper macro: early-return on packer failure
 * ----------------------------------------------------------------------- */

#define LUMBRE_PACK_OR_FAIL(expr) \
    do { if ((expr) < 0) return -1; } while (0)

/* --------------------------------------------------------------------------
 * Map field counts per span type (6 common + N specific)
 * ----------------------------------------------------------------------- */

static uint32_t lumbre_span_field_count(lumbre_span_type_t type)
{
    switch (type) {
    case LUMBRE_SPAN_HTTP_IN:                        return 8;  /* +uri, mth */
    case LUMBRE_SPAN_HTTP_OUT:                       return 9;  /* +url, mth, st */
    case LUMBRE_SPAN_DB:                             return 9;  /* +q, dbt, r */
    case LUMBRE_SPAN_REDIS:    /* fall through */
    case LUMBRE_SPAN_MEMCACHED:                      return 8;  /* +cmd, k */
    case LUMBRE_SPAN_FILE_IO:  /* fall through */
    case LUMBRE_SPAN_SOCKET:                         return 7;  /* +url */
    case LUMBRE_SPAN_FUNC:                           return 10; /* +cls, fn, f, l */
    default:                                         return 6;  /* common only */
    }
}

/* --------------------------------------------------------------------------
 * lumbre_context_init
 * ----------------------------------------------------------------------- */

void lumbre_context_init(
    lumbre_context_t *ctx,
    const uint8_t    *propagated_trace_id,
    int             (*random_func)(void *buf, size_t len))
{
    if (propagated_trace_id) {
        memcpy(ctx->trace_id, propagated_trace_id, 16);
    } else if (random_func) {
        random_func(ctx->trace_id, 16);
    } else {
        memset(ctx->trace_id, 0, 16);
    }

    ctx->span_counter     = 1;
    ctx->current_parent_id = 0;
    ctx->root_span_id     = 0;
    ctx->active            = 1;
    ctx->full_trace        = 0;
}

/* --------------------------------------------------------------------------
 * lumbre_context_reset
 * ----------------------------------------------------------------------- */

void lumbre_context_reset(lumbre_context_t *ctx)
{
    ctx->active = 0;
    memset(ctx->trace_id, 0, 16);
}

/* --------------------------------------------------------------------------
 * lumbre_span_start
 * ----------------------------------------------------------------------- */

void lumbre_span_start(
    lumbre_span_t      *span,
    lumbre_span_type_t  type,
    lumbre_context_t   *ctx)
{
    memset(span, 0, sizeof(lumbre_span_t));

    span->type = type;
    memcpy(span->trace_id, ctx->trace_id, 16);
    span->span_id   = ctx->span_counter++;
    span->parent_id = ctx->current_parent_id;
    span->start_ns  = lumbre_clock_ns();

    /* This span becomes the current parent for nested children. */
    ctx->current_parent_id = span->span_id;

    /* Track root span. */
    if (span->parent_id == 0) {
        ctx->root_span_id = span->span_id;
    }
}

/* --------------------------------------------------------------------------
 * lumbre_span_encode
 * ----------------------------------------------------------------------- */

int lumbre_span_encode(const lumbre_span_t *span, lumbre_msgpack_buf *mp_buf)
{
    uint32_t field_count = lumbre_span_field_count(span->type);

    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_map(mp_buf, field_count));

    /* --- 6 common fields --- */
    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
        mp_buf, LUMBRE_MKEY_TYPE, 1, span->type));
    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_bin_kv(
        mp_buf, LUMBRE_MKEY_TRACE_ID, 3, span->trace_id, 16));
    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
        mp_buf, LUMBRE_MKEY_SPAN_ID, 3, span->span_id));
    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
        mp_buf, LUMBRE_MKEY_PARENT_ID, 3, span->parent_id));
    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
        mp_buf, LUMBRE_MKEY_START_NS, 1, span->start_ns));
    LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
        mp_buf, LUMBRE_MKEY_DURATION, 1, span->duration_ns));

    /* --- Type-specific fields --- */
    switch (span->type) {
    case LUMBRE_SPAN_HTTP_IN:
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_URI, 3,
            span->request_uri, span->request_uri_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_METHOD, 3,
            span->method, span->method_len));
        break;

    case LUMBRE_SPAN_HTTP_OUT:
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_URL, 3,
            span->url, span->url_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_METHOD, 3,
            span->method, span->method_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
            mp_buf, LUMBRE_MKEY_STATUS, 2,
            span->status_code));
        break;

    case LUMBRE_SPAN_DB:
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_QUERY, 1,
            span->query, span->query_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_DB_TYPE, 3,
            span->db_type, span->db_type_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
            mp_buf, LUMBRE_MKEY_ROWS, 1,
            span->rows_affected));
        break;

    case LUMBRE_SPAN_REDIS:
    case LUMBRE_SPAN_MEMCACHED:
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_CACHE_CMD, 3,
            span->cache_cmd, span->cache_cmd_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_CACHE_KEY, 1,
            span->cache_key, span->cache_key_len));
        break;

    case LUMBRE_SPAN_FILE_IO:
    case LUMBRE_SPAN_SOCKET:
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_URL, 3,
            span->url, span->url_len));
        break;

    case LUMBRE_SPAN_FUNC:
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_FUNC_CLASS, 3,
            span->func_class, span->func_class_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_FUNC_NAME, 2,
            span->func_name, span->func_name_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_str_kv(
            mp_buf, LUMBRE_MKEY_FUNC_FILE, 1,
            span->func_file, span->func_file_len));
        LUMBRE_PACK_OR_FAIL(lumbre_msgpack_pack_uint_kv(
            mp_buf, LUMBRE_MKEY_FUNC_LINE, 1,
            span->func_line));
        break;

    default:
        /* LUMBRE_SPAN_CACHE or unknown — common fields only */
        break;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * lumbre_span_finish
 * ----------------------------------------------------------------------- */

int lumbre_span_finish(
    lumbre_span_t     *span,
    lumbre_ringbuf_t  *rb,
    lumbre_msgpack_buf *mp_buf,
    lumbre_context_t  *ctx,
    uint64_t           min_duration_ns,
    uint32_t           max_query_len)
{
    span->duration_ns = lumbre_clock_ns() - span->start_ns;

    /* Filter short spans — but never filter the root span (HTTP_IN). */
    if (min_duration_ns > 0
        && span->duration_ns < min_duration_ns
        && span->type != LUMBRE_SPAN_HTTP_IN) {
        /* Restore parent before returning. */
        ctx->current_parent_id = span->parent_id;
        return 1; /* filtered */
    }

    /* Restore parent (unwind the implicit stack) BEFORE writing.
       If the write fails, parent chaining stays consistent. */
    ctx->current_parent_id = span->parent_id;

    /* Apply query truncation if not already done by the setter. */
    if (span->type == LUMBRE_SPAN_DB && max_query_len > 0
        && span->query_len > max_query_len) {
        span->query_len = max_query_len;
    }

    /* Encode to msgpack. */
    lumbre_msgpack_reset(mp_buf);
    if (lumbre_span_encode(span, mp_buf) < 0) {
        return -1; /* encode overflow — silent drop */
    }

    /* Write to ring buffer. */
    return lumbre_ringbuf_write(rb, mp_buf->buf, (uint32_t)mp_buf->pos);
}
