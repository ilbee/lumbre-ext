/**
 * lumbre_whitelist.h — I/O function whitelist hash table
 *
 * Open-addressing hash table (linear probing) mapping PHP function
 * names to span types.  Built once at MINIT from a static table of
 * ~25 entries.  Lookup is O(1) amortised (~8-10 ns).
 *
 * Keys are case-insensitive canonical forms: "class::func" for
 * methods, "func" for global functions, all lowercase.
 */

#ifndef LUMBRE_WHITELIST_H
#define LUMBRE_WHITELIST_H

#include <stdint.h>
#include <stddef.h>

/* --------------- Constants --------------- */

#define LUMBRE_WHITELIST_SIZE  64u
#define LUMBRE_WHITELIST_MASK  63u  /* LUMBRE_WHITELIST_SIZE - 1 */

/* Span type constants (mirrors lumbre_span.h if/when it exists).
 * Guarded so that if lumbre_span.h is included first, we don't
 * redefine them. */
#ifndef LUMBRE_SPAN_HTTP_IN
#define LUMBRE_SPAN_HTTP_IN   1
#define LUMBRE_SPAN_HTTP_OUT  2
#define LUMBRE_SPAN_DB        3
#define LUMBRE_SPAN_REDIS     4
#define LUMBRE_SPAN_MEMCACHED 5
#define LUMBRE_SPAN_FILE_IO   6
#define LUMBRE_SPAN_SOCKET    7
#define LUMBRE_SPAN_FUNC      8
#define LUMBRE_SPAN_CACHE     9
#endif

/* --------------- Entry struct --------------- */

typedef struct {
    const char *class_name;   /* NULL for global functions */
    uint8_t     class_len;
    const char *func_name;
    uint8_t     func_len;
    uint8_t     span_type;    /* LUMBRE_SPAN_* constant */
    uint32_t    key_hash;     /* pre-computed FNV-1a of canonical key */
    int         occupied;     /* 0 or 1 */
} lumbre_whitelist_entry_t;

/* --------------- Hash table struct --------------- */

typedef struct {
    lumbre_whitelist_entry_t entries[LUMBRE_WHITELIST_SIZE];
} lumbre_whitelist_t;

/* --------------- Public API --------------- */

/**
 * Build the hash table from the built-in static whitelist entries.
 * Must be called once (e.g. at MINIT).
 */
void lumbre_whitelist_init(lumbre_whitelist_t *wl);

/**
 * Look up a function in the whitelist.
 *
 * @param wl         Initialised whitelist.
 * @param class_name Class name (NULL for global functions).
 * @param class_len  Length of class_name (0 if NULL).
 * @param func_name  Function/method name.
 * @param func_len   Length of func_name.
 * @return Pointer to the matching entry, or NULL if not found.
 */
const lumbre_whitelist_entry_t *lumbre_whitelist_match(
    const lumbre_whitelist_t *wl,
    const char               *class_name,
    size_t                    class_len,
    const char               *func_name,
    size_t                    func_len
);

/**
 * Zero out the hash table.  No dynamic memory to free.
 */
void lumbre_whitelist_destroy(lumbre_whitelist_t *wl);

/* --------------- Hash function --------------- */

/**
 * FNV-1a 32-bit hash.
 */
uint32_t lumbre_fnv1a(const void *data, size_t len);

#endif /* LUMBRE_WHITELIST_H */
