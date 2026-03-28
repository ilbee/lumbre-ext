/**
 * php_lumbre.h -- PHP module header for the lumbre extension.
 *
 * Declares module globals (per-worker), version macro, extern module entry.
 */

#ifndef PHP_LUMBRE_H
#define PHP_LUMBRE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_API.h"
#include "zend_execute.h"

#include "lumbre_ringbuf.h"
#include "lumbre_msgpack.h"
#include "lumbre_span.h"
#include "lumbre_whitelist.h"

/* --------------------------------------------------------------------------
 * Version
 * ----------------------------------------------------------------------- */

#define PHP_LUMBRE_VERSION "0.1.0"

/* --------------------------------------------------------------------------
 * Module entry
 * ----------------------------------------------------------------------- */

extern zend_module_entry lumbre_module_entry;
#define phpext_lumbre_ptr &lumbre_module_entry

/* --------------------------------------------------------------------------
 * Module globals — one set per worker (per-thread in ZTS, per-process in NTS)
 * ----------------------------------------------------------------------- */

ZEND_BEGIN_MODULE_GLOBALS(lumbre)
    /* INI-backed settings */
    zend_bool  enabled;           /* master switch */
    zend_long  mode;              /* 0 = io, 1 = full */
    char      *shm_dir;          /* shared memory directory */
    zend_long  buffer_size;      /* ring buffer capacity */
    char      *trigger_header;   /* header name to force full trace */
    zend_long  max_query_len;    /* SQL truncation limit */
    zend_long  min_duration_ns;  /* minimum span duration to record */
    char      *trace_namespaces; /* comma-separated namespace prefixes for full mode */

    /* Ring buffer (lazy-init at first RINIT) */
    lumbre_ringbuf_t  ringbuf;
    int               ringbuf_initialized;

    /* Msgpack serialisation buffer (pemalloc'd persistent) */
    lumbre_msgpack_buf mp_buf;
    uint8_t           *mp_storage;

    /* Per-request context */
    lumbre_context_t   ctx;
    lumbre_span_t      root_span;

    /* Whitelist (built at MINIT) */
    lumbre_whitelist_t whitelist;

    /* ZTS worker identifier */
    uint64_t worker_id;
ZEND_END_MODULE_GLOBALS(lumbre)

/* --------------------------------------------------------------------------
 * Globals accessor macro
 * ----------------------------------------------------------------------- */

#ifdef ZTS
#include "TSRM.h"
#define LUMBRE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(lumbre, v)
#else
#define LUMBRE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(lumbre, v)
#endif

/* Declare the globals storage */
ZEND_EXTERN_MODULE_GLOBALS(lumbre)

#endif /* PHP_LUMBRE_H */
