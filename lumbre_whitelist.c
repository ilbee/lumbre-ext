/**
 * lumbre_whitelist.c — I/O function whitelist hash table implementation
 *
 * Open-addressing hash table with linear probing.  Built once from a
 * static table of ~25 I/O functions that the lumbre should instrument.
 */

#include "lumbre_whitelist.h"
#include <string.h>

/* --------------- Internal helpers --------------- */

/** Maximum canonical key length ("class::func" lowercase). */
#define LUMBRE_WL_KEY_BUF_SIZE 128u

/**
 * FNV-1a 32-bit hash.
 */
uint32_t lumbre_fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 0x811c9dc5u; /* FNV offset basis */

    for (size_t i = 0; i < len; i++) {
        hash ^= (uint32_t)p[i];
        hash *= 0x01000193u;     /* FNV prime */
    }

    return hash;
}

/**
 * Build the lowercase canonical key for a class::func pair.
 *
 * Writes into `buf` (must be at least LUMBRE_WL_KEY_BUF_SIZE bytes).
 * Returns the key length, or 0 if the names would overflow the buffer.
 *
 * Lowercase is achieved with `c | 0x20` for ASCII letters (A-Z map to
 * a-z).  Non-letter bytes are copied unchanged.  PHP identifiers are
 * always ASCII so this is safe.
 */
static size_t lumbre_wl_build_key(
    char         *buf,
    const char   *class_name,
    size_t        class_len,
    const char   *func_name,
    size_t        func_len
)
{
    size_t pos = 0;

    if (class_name != NULL && class_len > 0) {
        /* "class::" prefix */
        if (class_len + 2 + func_len >= LUMBRE_WL_KEY_BUF_SIZE) {
            return 0; /* overflow guard */
        }
        for (size_t i = 0; i < class_len; i++) {
            unsigned char c = (unsigned char)class_name[i];
            buf[pos++] = (char)((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
        }
        buf[pos++] = ':';
        buf[pos++] = ':';
    } else {
        if (func_len >= LUMBRE_WL_KEY_BUF_SIZE) {
            return 0;
        }
    }

    for (size_t i = 0; i < func_len; i++) {
        unsigned char c = (unsigned char)func_name[i];
        buf[pos++] = (char)((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
    }

    return pos;
}

/* --------------- Static whitelist entries --------------- */

/**
 * Raw whitelist table.  Strings are compile-time literals (process
 * lifetime).  class_name is NULL for global functions.
 *
 * Span types are defined in lumbre_whitelist.h (LUMBRE_SPAN_*).
 *
 * NOTE: file_get_contents and fopen are registered with FILE_IO as
 * default type.  The Zend hook (phase 5) overrides to HTTP_OUT when
 * the first argument is a URL.
 */
typedef struct {
    const char *class_name;
    uint8_t     class_len;
    const char *func_name;
    uint8_t     func_len;
    uint8_t     span_type;
} lumbre_wl_static_entry_t;

static const lumbre_wl_static_entry_t lumbre_wl_static_table[] = {
    /* --- HTTP (type=HTTP_OUT=2) --- */
    { NULL, 0, "curl_exec",       9, LUMBRE_SPAN_HTTP_OUT  },
    { NULL, 0, "curl_multi_exec", 15, LUMBRE_SPAN_HTTP_OUT },

    /* --- Database (type=DB=3) --- */
    { "PDO",  3, "execute",          7, LUMBRE_SPAN_DB },
    { "PDO",  3, "query",            5, LUMBRE_SPAN_DB },
    { NULL,   0, "mysqli_query",    12, LUMBRE_SPAN_DB },
    { NULL,   0, "mysqli_real_query", 17, LUMBRE_SPAN_DB },
    { NULL,   0, "pg_query",         8, LUMBRE_SPAN_DB },
    { NULL,   0, "pg_execute",      10, LUMBRE_SPAN_DB },

    /* --- Redis (type=REDIS=4) --- */
    { "Redis", 5, "get",      3, LUMBRE_SPAN_REDIS },
    { "Redis", 5, "set",      3, LUMBRE_SPAN_REDIS },
    { "Redis", 5, "mget",     4, LUMBRE_SPAN_REDIS },
    { "Redis", 5, "pipeline", 8, LUMBRE_SPAN_REDIS },

    /* --- Memcached (type=MEMCACHED=5) --- */
    { "Memcached", 9, "get",      3, LUMBRE_SPAN_MEMCACHED },
    { "Memcached", 9, "set",      3, LUMBRE_SPAN_MEMCACHED },
    { "Memcached", 9, "getMulti", 8, LUMBRE_SPAN_MEMCACHED },

    /* --- File I/O (type=FILE_IO=6) --- */
    { NULL, 0, "file_get_contents", 17, LUMBRE_SPAN_FILE_IO },
    { NULL, 0, "file_put_contents", 17, LUMBRE_SPAN_FILE_IO },
    { NULL, 0, "fopen",              5, LUMBRE_SPAN_FILE_IO },
    { NULL, 0, "fwrite",             6, LUMBRE_SPAN_FILE_IO },
    { NULL, 0, "fread",              5, LUMBRE_SPAN_FILE_IO },

    /* --- Socket (type=SOCKET=7) --- */
    { NULL, 0, "fsockopen",            9, LUMBRE_SPAN_SOCKET },
    { NULL, 0, "stream_socket_client", 20, LUMBRE_SPAN_SOCKET },
};

#define LUMBRE_WL_STATIC_COUNT \
    (sizeof(lumbre_wl_static_table) / sizeof(lumbre_wl_static_table[0]))

/* --------------- Public API --------------- */

void lumbre_whitelist_init(lumbre_whitelist_t *wl)
{
    memset(wl, 0, sizeof(lumbre_whitelist_t));

    for (size_t i = 0; i < LUMBRE_WL_STATIC_COUNT; i++) {
        const lumbre_wl_static_entry_t *src = &lumbre_wl_static_table[i];

        /* Build canonical key */
        char key_buf[LUMBRE_WL_KEY_BUF_SIZE];
        size_t key_len = lumbre_wl_build_key(
            key_buf, src->class_name, src->class_len,
            src->func_name, src->func_len
        );
        if (key_len == 0) {
            continue; /* should never happen with our entries */
        }

        uint32_t hash = lumbre_fnv1a(key_buf, key_len);
        uint32_t slot = hash & LUMBRE_WHITELIST_MASK;

        /* Linear probe for an empty slot */
        while (wl->entries[slot].occupied) {
            slot = (slot + 1) & LUMBRE_WHITELIST_MASK;
        }

        /* Populate the slot */
        lumbre_whitelist_entry_t *dst = &wl->entries[slot];
        dst->class_name = src->class_name;
        dst->class_len  = src->class_len;
        dst->func_name  = src->func_name;
        dst->func_len   = src->func_len;
        dst->span_type  = src->span_type;
        dst->key_hash   = hash;
        dst->occupied   = 1;
    }
}

const lumbre_whitelist_entry_t *lumbre_whitelist_match(
    const lumbre_whitelist_t *wl,
    const char               *class_name,
    size_t                    class_len,
    const char               *func_name,
    size_t                    func_len
)
{
    /* Build canonical key from input */
    char key_buf[LUMBRE_WL_KEY_BUF_SIZE];
    size_t key_len = lumbre_wl_build_key(
        key_buf, class_name, class_len, func_name, func_len
    );
    if (key_len == 0) {
        return NULL;
    }

    uint32_t hash = lumbre_fnv1a(key_buf, key_len);
    uint32_t slot = hash & LUMBRE_WHITELIST_MASK;

    /* Linear probe */
    while (wl->entries[slot].occupied) {
        const lumbre_whitelist_entry_t *e = &wl->entries[slot];

        if (e->key_hash == hash) {
            /* Hash matches — rebuild canonical key from entry to compare */
            char entry_key[LUMBRE_WL_KEY_BUF_SIZE];
            size_t entry_key_len = lumbre_wl_build_key(
                entry_key, e->class_name, e->class_len,
                e->func_name, e->func_len
            );
            if (entry_key_len == key_len &&
                memcmp(key_buf, entry_key, key_len) == 0) {
                return e;
            }
        }

        slot = (slot + 1) & LUMBRE_WHITELIST_MASK;
    }

    return NULL;
}

void lumbre_whitelist_destroy(lumbre_whitelist_t *wl)
{
    memset(wl, 0, sizeof(lumbre_whitelist_t));
}
