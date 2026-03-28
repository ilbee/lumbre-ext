/*
 * lumbre_msgpack.c — Hand-rolled msgpack packer (subset).
 *
 * Pure C11, no PHP headers, no dynamic allocation.
 * Only the msgpack types used by the lumbre are implemented:
 *   positive fixint, uint8/16/32/64, fixstr/str16/str32,
 *   bin8/bin16, fixmap/map16.
 *
 * All multi-byte integers are written in big-endian (msgpack spec).
 * Byte-swapping uses GCC/Clang __builtin_bswap* intrinsics.
 */

#include "lumbre_msgpack.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

#define LUMBRE_MSGPACK_CHECK_SPACE(buf, n) \
    do {                                   \
        if ((buf)->pos + (n) > (buf)->capacity) return -1; \
    } while (0)

static inline void lumbre_store16(uint8_t *dst, uint16_t val)
{
    uint16_t be = __builtin_bswap16(val);
    memcpy(dst, &be, 2);
}

static inline void lumbre_store32(uint8_t *dst, uint32_t val)
{
    uint32_t be = __builtin_bswap32(val);
    memcpy(dst, &be, 4);
}

static inline void lumbre_store64(uint8_t *dst, uint64_t val)
{
    uint64_t be = __builtin_bswap64(val);
    memcpy(dst, &be, 8);
}

/* --------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void lumbre_msgpack_reset(lumbre_msgpack_buf *buf)
{
    buf->pos = 0;
}

/* -- Map ----------------------------------------------------------------- */

int lumbre_msgpack_pack_map(lumbre_msgpack_buf *buf, uint32_t count)
{
    if (count <= 15) {
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 1);
        buf->buf[buf->pos++] = (uint8_t)(0x80 | count);
    } else {
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 3);
        buf->buf[buf->pos++] = 0xde;
        lumbre_store16(buf->buf + buf->pos, (uint16_t)count);
        buf->pos += 2;
    }
    return 0;
}

/* -- Unsigned integer ---------------------------------------------------- */

int lumbre_msgpack_pack_uint(lumbre_msgpack_buf *buf, uint64_t val)
{
    if (val < 128) {
        /* positive fixint: single byte */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 1);
        buf->buf[buf->pos++] = (uint8_t)val;
    } else if (val <= 0xFF) {
        /* uint8 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 2);
        buf->buf[buf->pos++] = 0xcc;
        buf->buf[buf->pos++] = (uint8_t)val;
    } else if (val <= 0xFFFF) {
        /* uint16 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 3);
        buf->buf[buf->pos++] = 0xcd;
        lumbre_store16(buf->buf + buf->pos, (uint16_t)val);
        buf->pos += 2;
    } else if (val <= 0xFFFFFFFF) {
        /* uint32 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 5);
        buf->buf[buf->pos++] = 0xce;
        lumbre_store32(buf->buf + buf->pos, (uint32_t)val);
        buf->pos += 4;
    } else {
        /* uint64 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 9);
        buf->buf[buf->pos++] = 0xcf;
        lumbre_store64(buf->buf + buf->pos, val);
        buf->pos += 8;
    }
    return 0;
}

/* -- String -------------------------------------------------------------- */

int lumbre_msgpack_pack_str(lumbre_msgpack_buf *buf, const char *str,
                            uint32_t len)
{
    if (len < 32) {
        /* fixstr */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 1 + len);
        buf->buf[buf->pos++] = (uint8_t)(0xa0 | len);
    } else if (len <= 0xFFFF) {
        /* str16 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 3 + len);
        buf->buf[buf->pos++] = 0xda;
        lumbre_store16(buf->buf + buf->pos, (uint16_t)len);
        buf->pos += 2;
    } else {
        /* str32 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 5 + len);
        buf->buf[buf->pos++] = 0xdb;
        lumbre_store32(buf->buf + buf->pos, len);
        buf->pos += 4;
    }

    if (len > 0) {
        memcpy(buf->buf + buf->pos, str, len);
        buf->pos += len;
    }
    return 0;
}

/* -- Binary -------------------------------------------------------------- */

int lumbre_msgpack_pack_bin(lumbre_msgpack_buf *buf, const void *data,
                            uint32_t len)
{
    if (len <= 0xFF) {
        /* bin8 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 2 + len);
        buf->buf[buf->pos++] = 0xc4;
        buf->buf[buf->pos++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        /* bin16 */
        LUMBRE_MSGPACK_CHECK_SPACE(buf, 3 + len);
        buf->buf[buf->pos++] = 0xc5;
        lumbre_store16(buf->buf + buf->pos, (uint16_t)len);
        buf->pos += 2;
    } else {
        /* bin data larger than 64 KB is not supported in this subset */
        return -1;
    }

    if (len > 0) {
        memcpy(buf->buf + buf->pos, data, len);
        buf->pos += len;
    }
    return 0;
}

/* -- Key-value helpers --------------------------------------------------- */

int lumbre_msgpack_pack_str_kv(lumbre_msgpack_buf *buf,
                               const char *key, uint32_t key_len,
                               const char *val, uint32_t val_len)
{
    int rc = lumbre_msgpack_pack_str(buf, key, key_len);
    if (rc != 0) return rc;
    return lumbre_msgpack_pack_str(buf, val, val_len);
}

int lumbre_msgpack_pack_uint_kv(lumbre_msgpack_buf *buf,
                                const char *key, uint32_t key_len,
                                uint64_t val)
{
    int rc = lumbre_msgpack_pack_str(buf, key, key_len);
    if (rc != 0) return rc;
    return lumbre_msgpack_pack_uint(buf, val);
}

int lumbre_msgpack_pack_bin_kv(lumbre_msgpack_buf *buf,
                               const char *key, uint32_t key_len,
                               const void *data, uint32_t data_len)
{
    int rc = lumbre_msgpack_pack_str(buf, key, key_len);
    if (rc != 0) return rc;
    return lumbre_msgpack_pack_bin(buf, data, data_len);
}
