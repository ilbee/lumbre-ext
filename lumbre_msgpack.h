/*
 * lumbre_msgpack.h — Hand-rolled msgpack packer (subset).
 *
 * Pure C, no PHP headers. Supports: positive fixint, uint8/16/32/64,
 * fixstr/str16/str32, bin8/bin16, fixmap/map16.
 *
 * All multi-byte integers are big-endian per msgpack spec.
 */

#ifndef LUMBRE_MSGPACK_H
#define LUMBRE_MSGPACK_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Buffer
 * ----------------------------------------------------------------------- */

#define LUMBRE_MSGPACK_BUF_SIZE 16384 /* 16 KB */

typedef struct {
    uint8_t *buf;      /* pre-allocated buffer (not owned) */
    size_t   capacity; /* total size in bytes               */
    size_t   pos;      /* current write offset              */
} lumbre_msgpack_buf;

/* --------------------------------------------------------------------------
 * Msgpack map-key constants (shared contract with daemon Go)
 * ----------------------------------------------------------------------- */

/* Common span fields */
#define LUMBRE_MKEY_TYPE       "t"
#define LUMBRE_MKEY_TRACE_ID   "tid"
#define LUMBRE_MKEY_SPAN_ID    "sid"
#define LUMBRE_MKEY_PARENT_ID  "pid"
#define LUMBRE_MKEY_START_NS   "s"
#define LUMBRE_MKEY_DURATION   "d"

/* HTTP out / HTTP in */
#define LUMBRE_MKEY_URL        "url"
#define LUMBRE_MKEY_METHOD     "mth"
#define LUMBRE_MKEY_STATUS     "st"

/* DB */
#define LUMBRE_MKEY_QUERY      "q"
#define LUMBRE_MKEY_DB_TYPE    "dbt"
#define LUMBRE_MKEY_ROWS       "r"

/* Redis / Memcached */
#define LUMBRE_MKEY_CACHE_CMD  "cmd"
#define LUMBRE_MKEY_CACHE_KEY  "k"

/* Function */
#define LUMBRE_MKEY_FUNC_CLASS "cls"
#define LUMBRE_MKEY_FUNC_NAME  "fn"
#define LUMBRE_MKEY_FUNC_FILE  "f"
#define LUMBRE_MKEY_FUNC_LINE  "l"

/* HTTP in (request URI) */
#define LUMBRE_MKEY_URI        "uri"

/* --------------------------------------------------------------------------
 * API — all return 0 on success, -1 on overflow
 * ----------------------------------------------------------------------- */

/* Reset write position to 0. */
void lumbre_msgpack_reset(lumbre_msgpack_buf *buf);

/* Map header: fixmap (count <= 15) or map16. */
int lumbre_msgpack_pack_map(lumbre_msgpack_buf *buf, uint32_t count);

/* Unsigned integer: positive fixint / uint8 / uint16 / uint32 / uint64. */
int lumbre_msgpack_pack_uint(lumbre_msgpack_buf *buf, uint64_t val);

/* String with length prefix: fixstr / str16 / str32. */
int lumbre_msgpack_pack_str(lumbre_msgpack_buf *buf, const char *str,
                            uint32_t len);

/* Binary data: bin8 / bin16. */
int lumbre_msgpack_pack_bin(lumbre_msgpack_buf *buf, const void *data,
                            uint32_t len);

/* Key-value helpers: pack key as str, then value. */
int lumbre_msgpack_pack_str_kv(lumbre_msgpack_buf *buf,
                               const char *key, uint32_t key_len,
                               const char *val, uint32_t val_len);

int lumbre_msgpack_pack_uint_kv(lumbre_msgpack_buf *buf,
                                const char *key, uint32_t key_len,
                                uint64_t val);

int lumbre_msgpack_pack_bin_kv(lumbre_msgpack_buf *buf,
                               const char *key, uint32_t key_len,
                               const void *data, uint32_t data_len);

#endif /* LUMBRE_MSGPACK_H */
